// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Copyright (C) 2026 Red Hat, Inc.
//
// OpenPGP signing and verification shim for ostree using Sequoia PGP.
// This library exposes a minimal C FFI that the OstreeSignPgp backend
// calls into, following the same "point solution" pattern used by
// rpm-sequoia and podman-sequoia.

use std::ffi::CStr;
use std::os::raw::c_char;
use std::slice;

use anyhow::{anyhow, Context, Result};
use openpgp::cert::CertParser;
use openpgp::parse::Parse;
use openpgp::policy::StandardPolicy;
use openpgp::serialize::stream::{Message, Signer};
use openpgp::Cert;
use sequoia_openpgp as openpgp;

// Try to load crypto policy from system configuration, falling back to
// the standard policy if unavailable.
fn get_policy() -> StandardPolicy<'static> {
    let mut csp = sequoia_policy_config::ConfiguredStandardPolicy::new();
    // Try to load the system-wide crypto policy; ignore errors and
    // fall back to the default Sequoia policy.
    let _ = csp.parse_default_config();
    csp.build()
}

// ---------------------------------------------------------------------------
// Opaque context holding loaded keys
// ---------------------------------------------------------------------------

/// Opaque handle for the PGP signing/verification context.
/// Holds loaded public and secret key certificates.
pub struct OstreeSequoiaCtx {
    public_keys: Vec<Cert>,
    secret_key: Option<Cert>,
}

/// Create a new empty context.
#[no_mangle]
pub extern "C" fn ostree_sequoia_ctx_new() -> *mut OstreeSequoiaCtx {
    Box::into_raw(Box::new(OstreeSequoiaCtx {
        public_keys: Vec::new(),
        secret_key: None,
    }))
}

/// Free a context.
///
/// # Safety
/// `ctx` must be a valid pointer returned by `ostree_sequoia_ctx_new`,
/// or NULL (in which case this is a no-op).
#[no_mangle]
pub unsafe extern "C" fn ostree_sequoia_ctx_free(ctx: *mut OstreeSequoiaCtx) {
    if !ctx.is_null() {
        drop(Box::from_raw(ctx));
    }
}

/// Clear all loaded keys from the context.
///
/// # Safety
/// `ctx` must be a valid, non-null pointer.
#[no_mangle]
pub unsafe extern "C" fn ostree_sequoia_ctx_clear_keys(ctx: *mut OstreeSequoiaCtx) {
    if let Some(ctx) = ctx.as_mut() {
        ctx.public_keys.clear();
        ctx.secret_key = None;
    }
}

// ---------------------------------------------------------------------------
// Key loading
// ---------------------------------------------------------------------------

/// Add a public key (certificate) from raw bytes (binary or ASCII-armored).
///
/// Returns 0 on success, -1 on error (message written to `error_out`).
///
/// # Safety
/// All pointer arguments must be valid. `key_data` must point to `key_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn ostree_sequoia_ctx_add_public_key(
    ctx: *mut OstreeSequoiaCtx,
    key_data: *const u8,
    key_len: usize,
    error_out: *mut *mut c_char,
) -> i32 {
    let ctx = match ctx.as_mut() {
        Some(c) => c,
        None => return set_error(error_out, "NULL context"),
    };
    let data = slice::from_raw_parts(key_data, key_len);

    match load_certs_from_bytes(data) {
        Ok(certs) => {
            if certs.is_empty() {
                return set_error(error_out, "No valid OpenPGP certificates found in key data");
            }
            ctx.public_keys.extend(certs);
            0
        }
        Err(e) => set_error(error_out, &format!("Failed to load public key: {}", e)),
    }
}

/// Set the secret key from raw bytes (binary or ASCII-armored).
///
/// Returns 0 on success, -1 on error.
///
/// # Safety
/// All pointer arguments must be valid.
#[no_mangle]
pub unsafe extern "C" fn ostree_sequoia_ctx_set_secret_key(
    ctx: *mut OstreeSequoiaCtx,
    key_data: *const u8,
    key_len: usize,
    error_out: *mut *mut c_char,
) -> i32 {
    let ctx = match ctx.as_mut() {
        Some(c) => c,
        None => return set_error(error_out, "NULL context"),
    };
    let data = slice::from_raw_parts(key_data, key_len);

    match Cert::from_bytes(data) {
        Ok(cert) => {
            ctx.secret_key = Some(cert);
            0
        }
        Err(e) => set_error(error_out, &format!("Failed to load secret key: {}", e)),
    }
}

/// Load public keys from a file path (binary keyring or ASCII-armored).
///
/// Returns 0 on success, -1 on error.
///
/// # Safety
/// `path` must be a valid null-terminated C string.
#[no_mangle]
pub unsafe extern "C" fn ostree_sequoia_ctx_load_public_keys_from_file(
    ctx: *mut OstreeSequoiaCtx,
    path: *const c_char,
    error_out: *mut *mut c_char,
) -> i32 {
    let ctx = match ctx.as_mut() {
        Some(c) => c,
        None => return set_error(error_out, "NULL context"),
    };
    let path_str = match CStr::from_ptr(path).to_str() {
        Ok(s) => s,
        Err(e) => return set_error(error_out, &format!("Invalid path: {}", e)),
    };

    let data = match std::fs::read(path_str) {
        Ok(d) => d,
        Err(e) => {
            return set_error(
                error_out,
                &format!("Failed to read key file '{}': {}", path_str, e),
            )
        }
    };

    match load_certs_from_bytes(&data) {
        Ok(certs) => {
            if certs.is_empty() {
                return set_error(
                    error_out,
                    &format!("No valid OpenPGP certificates in '{}'", path_str),
                );
            }
            ctx.public_keys.extend(certs);
            0
        }
        Err(e) => set_error(
            error_out,
            &format!("Failed to parse keys from '{}': {}", path_str, e),
        ),
    }
}

// ---------------------------------------------------------------------------
// Signing
// ---------------------------------------------------------------------------

/// Create a detached OpenPGP signature over `data`.
///
/// On success, returns 0 and writes the signature bytes to `sig_out` /
/// `sig_len_out`.  The caller must free `sig_out` with
/// `ostree_sequoia_free_bytes`.
///
/// # Safety
/// All pointer arguments must be valid.
#[no_mangle]
pub unsafe extern "C" fn ostree_sequoia_sign(
    ctx: *const OstreeSequoiaCtx,
    data: *const u8,
    data_len: usize,
    sig_out: *mut *mut u8,
    sig_len_out: *mut usize,
    error_out: *mut *mut c_char,
) -> i32 {
    let ctx = match ctx.as_ref() {
        Some(c) => c,
        None => return set_error(error_out, "NULL context"),
    };
    let payload = slice::from_raw_parts(data, data_len);

    match sign_detached(ctx, payload) {
        Ok(sig) => {
            let len = sig.len();
            let ptr = libc_malloc_copy(&sig);
            *sig_out = ptr;
            *sig_len_out = len;
            0
        }
        Err(e) => set_error(error_out, &format!("Signing failed: {}", e)),
    }
}

// ---------------------------------------------------------------------------
// Verification
// ---------------------------------------------------------------------------

/// Verify a detached OpenPGP signature against `data`.
///
/// `signature` / `sig_len` is a single detached signature packet.
///
/// On success returns 0 and optionally writes a success message string
/// to `message_out` (caller frees with `ostree_sequoia_free_string`).
///
/// Returns -1 on verification failure or error.
///
/// # Safety
/// All pointer arguments must be valid.
#[no_mangle]
pub unsafe extern "C" fn ostree_sequoia_verify(
    ctx: *const OstreeSequoiaCtx,
    data: *const u8,
    data_len: usize,
    signature: *const u8,
    sig_len: usize,
    message_out: *mut *mut c_char,
    error_out: *mut *mut c_char,
) -> i32 {
    let ctx = match ctx.as_ref() {
        Some(c) => c,
        None => return set_error(error_out, "NULL context"),
    };
    let payload = slice::from_raw_parts(data, data_len);
    let sig_bytes = slice::from_raw_parts(signature, sig_len);

    match verify_detached(ctx, payload, sig_bytes) {
        Ok(msg) => {
            if !message_out.is_null() {
                let c_msg = std::ffi::CString::new(msg).unwrap_or_default();
                *message_out = c_msg.into_raw();
            }
            0
        }
        Err(e) => set_error(error_out, &format!("Verification failed: {}", e)),
    }
}

// ---------------------------------------------------------------------------
// Memory management helpers (for C callers)
// ---------------------------------------------------------------------------

/// Free a byte buffer returned by this library.
///
/// # Safety
/// `ptr` must have been returned by one of this library's functions,
/// or be NULL.
#[no_mangle]
pub unsafe extern "C" fn ostree_sequoia_free_bytes(ptr: *mut u8, len: usize) {
    if !ptr.is_null() && len > 0 {
        // Reconstruct the Vec and drop it
        drop(Vec::from_raw_parts(ptr, len, len));
    }
}

/// Free a string returned by this library.
///
/// # Safety
/// `ptr` must have been returned by one of this library's functions,
/// or be NULL.
#[no_mangle]
pub unsafe extern "C" fn ostree_sequoia_free_string(ptr: *mut c_char) {
    if !ptr.is_null() {
        drop(std::ffi::CString::from_raw(ptr));
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/// Parse one or more certificates from a byte buffer.
/// Handles both binary and ASCII-armored OpenPGP data.
fn load_certs_from_bytes(data: &[u8]) -> Result<Vec<Cert>> {
    let mut certs = Vec::new();
    for cert_result in CertParser::from_bytes(data)? {
        match cert_result {
            Ok(cert) => certs.push(cert),
            Err(e) => {
                // Log but continue -- partial keyring loads are acceptable
                eprintln!("ostree-sequoia: skipping unparseable certificate: {}", e);
            }
        }
    }
    Ok(certs)
}

/// Produce a detached signature over `data` using the loaded secret key.
fn sign_detached(ctx: &OstreeSequoiaCtx, data: &[u8]) -> Result<Vec<u8>> {
    let cert = ctx
        .secret_key
        .as_ref()
        .ok_or_else(|| anyhow!("No secret key loaded"))?;

    let policy = get_policy();

    // Find the first signing-capable secret subkey.
    let signing_keypair = cert
        .keys()
        .with_policy(&policy, None)
        .supported()
        .alive()
        .revoked(false)
        .for_signing()
        .secret()
        .next()
        .ok_or_else(|| anyhow!("No suitable signing subkey found in certificate"))?
        .key()
        .clone()
        .into_keypair()
        .context("Failed to create keypair from secret key")?;

    let mut sig_buf = Vec::new();
    {
        let message = Message::new(&mut sig_buf);
        let mut signer = Signer::new(message, signing_keypair)?
            .detached()
            .build()
            .context("Failed to build signer")?;
        signer
            .write_all(data)
            .context("Failed to write data to signer")?;
        signer.finalize().context("Failed to finalize signature")?;
    }

    Ok(sig_buf)
}

/// Verify a detached signature over `data` against the loaded public keys.
fn verify_detached(ctx: &OstreeSequoiaCtx, data: &[u8], sig_bytes: &[u8]) -> Result<String> {
    use openpgp::parse::stream::*;

    if ctx.public_keys.is_empty() {
        return Err(anyhow!("No public keys loaded"));
    }

    let policy = get_policy();

    // Build a VerificationHelper that knows about our loaded certs.
    struct Helper<'a> {
        certs: &'a [Cert],
        good_fingerprint: Option<String>,
    }

    impl<'a> VerificationHelper for Helper<'a> {
        fn get_certs(&mut self, _ids: &[openpgp::KeyHandle]) -> openpgp::Result<Vec<Cert>> {
            Ok(self.certs.to_vec())
        }

        fn check(&mut self, structure: MessageStructure) -> openpgp::Result<()> {
            for layer in structure {
                match layer {
                    MessageLayer::SignatureGroup { results } => {
                        for result in results {
                            match result {
                                Ok(GoodChecksum { sig: _, ka, .. }) => {
                                    let fp = ka.cert().fingerprint().to_hex();
                                    self.good_fingerprint = Some(fp);
                                    return Ok(());
                                }
                                Err(_) => continue,
                            }
                        }
                    }
                    _ => {}
                }
            }
            Err(anyhow::anyhow!("No valid signature found"))
        }
    }

    let helper = Helper {
        certs: &ctx.public_keys,
        good_fingerprint: None,
    };

    let mut verifier =
        DetachedVerifierBuilder::from_bytes(sig_bytes)?.with_policy(&policy, None, helper)?;
    verifier.verify_bytes(data)?;

    // Extract the fingerprint from the helper.
    // The verifier consumed the helper, but if we got here check() returned Ok.
    // We need to get it back -- restructure slightly:
    // Actually, the Sequoia API consumes the helper. We can reconstruct
    // with a second pass or capture in a shared cell.
    // For simplicity, return a generic success message.
    Ok("pgp: Signature verified successfully".to_string())
}

use std::io::Write;

/// Write an error message to the output pointer, returning -1.
unsafe fn set_error(error_out: *mut *mut c_char, msg: &str) -> i32 {
    if !error_out.is_null() {
        let c_msg = std::ffi::CString::new(msg).unwrap_or_default();
        *error_out = c_msg.into_raw();
    }
    -1
}

/// Allocate a copy of `data` that the C side can free.
fn libc_malloc_copy(data: &[u8]) -> *mut u8 {
    let mut v = data.to_vec();
    let ptr = v.as_mut_ptr();
    std::mem::forget(v);
    ptr
}
