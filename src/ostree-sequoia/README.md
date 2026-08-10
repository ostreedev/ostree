# ostree-sequoia: OpenPGP backend for ostree using Sequoia PGP

This crate provides an OpenPGP signing and verification backend for
ostree, replacing the legacy GPGME dependency with
[Sequoia PGP](https://sequoia-pgp.org/).  It produces a shared library
(`libostree_sequoia.so`) that exposes a minimal C FFI consumed by the
`OstreeSignPgp` GObject implementation in `src/libostree/ostree-sign-pgp.c`.

## Why this exists

GnuPG (and its library, GPGME) depends on `libgcrypt`, which is being
deprecated in RHEL 10 and **removed in RHEL 11**.  `libgcrypt` will not
receive FIPS certification going forward.  ostree's existing GPG
signature verification is deeply coupled to GPGME, and GPGME itself is
essentially a wrapper that spawns `gpg` subprocesses.

The RHEL crypto team has asked all GnuPG/GPGME consumers to migrate to
Sequoia PGP, which uses OpenSSL (or other certified backends) for
cryptographic operations and integrates with the system-wide crypto
policy at `/etc/crypto-policies/back-ends/sequoia.config`.

Tracked by: [RHEL-56361](https://issues.redhat.com/browse/RHEL-56361),
parent epic [CRYPTO-23599](https://issues.redhat.com/browse/CRYPTO-23599)
("Get rid of users of GnuPG").

## Architecture

```
 Rust consumers                          C consumers
 (rpm-ostree, bootc, ...)               (ostree CLI, Flatpak, ...)
        |                                       |
        v                                       v
 rust-bindings/                          src/libostree/
   Safe Rust wrappers                      OstreeSign interface
   (calls into libostree C API)            |
                                           v
                                    ostree-sign-pgp.c
                                    (GObject implementing OstreeSign,
                                     calls into ostree-sequoia FFI)
                                           |
                                           v
                                    src/ostree-sequoia/     <-- THIS CRATE
                                      libostree_sequoia.so
                                      (Rust, exports extern "C")
                                           |
                                           v
                                    sequoia-openpgp (Rust crate)
                                           |
                                           v
                                    OpenSSL / system crypto
```

### Why a separate crate (not in `rust-bindings/`)

The `rust-bindings/` directory is a **consumer-facing** crate published to
crates.io as `ostree`.  It wraps the C `libostree-1.so` via GObject
Introspection so Rust programs can call *into* ostree.

This crate goes the **opposite direction**: it is a Rust library that
exports C symbols so the C library can call *into* Rust.  Mixing them
would force every Rust consumer of the `ostree` crate to depend on
`sequoia-openpgp`, even if they never use PGP signatures.

| | `rust-bindings/` | `src/ostree-sequoia/` |
|---|---|---|
| Direction | C -> Rust consumers | Rust -> C library |
| Published | crates.io (`ostree`) | Internal build artifact |
| Crate type | `rlib` | `cdylib` + `staticlib` |
| License | MIT | LGPL-2.0-or-later |
| Dependencies | `ostree-sys` (FFI to C) | `sequoia-openpgp` |

## Design choices

### 1. "Point solution" Rust shim (not subprocess, not GPGME replacement)

We considered four approaches:

| Approach | Pros | Cons |
|---|---|---|
| **A. Replace GPGME with Sequoia in-place** | Complete migration | Touches 26 public API symbols, ABI break, massive diff |
| **B. Disable GPG, rely on OCI signatures** | Minimal code change | Doesn't solve Flatpak; leaves FIPS gap |
| **C. New OstreeSign backend with Rust shim** | Clean separation, no ABI break, parallel to ed25519/SPKI | New build dependency (Rust) |
| **D. Subprocess to `sqv`/`gpgv`** | No library deps | Fragile in sandboxed environments, parsing issues |

We chose **Option C** because:

- The `OstreeSign` interface already exists with ed25519 and SPKI backends
  that follow the exact same pattern (pluggable sign/verify engines).
- It adds PGP as a *new* engine (`--sign-type=pgp`) without touching the
  legacy GPGME code path, so nothing breaks.
- It follows the proven pattern used by **rpm-sequoia** (RPM),
  **podman-sequoia** (containers/image), and **sequoia-octopus-librnp**
  (Thunderbird) -- all shipped in production on Fedora/RHEL.
- The legacy GPGME path can be disabled independently via
  `--without-gpgme` when distros are ready.

### 2. sequoia-openpgp, not the (removed) FFI crate

The `sequoia-openpgp-ffi` crate was **removed** from the Sequoia
workspace in 2024.  The current approach across the ecosystem is to
write a thin Rust shim per-project that wraps `sequoia-openpgp` and
exports the specific C symbols needed.  This is what rpm-sequoia,
podman-sequoia, and sequoia-octopus-librnp all do.

### 3. Crypto backend selection

The default feature is `crypto-openssl`, which uses OpenSSL for all
cryptographic operations.  This is critical for:

- **FIPS compliance**: OpenSSL is the certified crypto provider on RHEL.
- **System crypto policy**: `sequoia-policy-config` reads
  `/etc/crypto-policies/back-ends/sequoia.config` to enforce
  algorithm restrictions.

A `crypto-rust` feature is available for environments without OpenSSL
(e.g., embedded or minimal containers), using pure-Rust crypto crates.

### 4. Separate metadata key (`ostree.sign.pgp`)

Signatures produced by this backend are stored under
`ostree.sign.pgp` in commit detached metadata, **not** under the
legacy `ostree.gpgsigs` key used by the GPGME path.  This means:

- Old GPGME-signed commits remain verifiable by the old code path.
- New PGP-signed commits use the `OstreeSign` interface consistently.
- Both can coexist: a commit can have both `ostree.gpgsigs` and
  `ostree.sign.pgp` signatures.

### 5. Cross-distro availability

Every distro that ships ostree also has the required build dependencies
in their main repositories:

| Dependency | Fedora | Debian | Arch | Alpine | openSUSE |
|---|---|---|---|---|---|
| `sequoia-openpgp` | `rust-sequoia-openpgp-devel` | `librust-sequoia-openpgp-dev` | vendored | vendored | vendored |
| `cbindgen` | 0.29.4 | 0.29.4 | 0.29.4 | 0.29.4 | 0.29.4 |
| `rustc + cargo` | yes | yes | yes | yes | yes |

Distros that do not want the PGP backend can build ostree with
`--without-pgp` (or `--with-pgp=no`).  The `HAVE_PGP` / `USE_PGP`
conditionals ensure zero impact on the build when disabled.

## Building

### As part of ostree (autotools)

```sh
# With PGP support (requires ostree-sequoia.pc in PKG_CONFIG_PATH):
cd src/ostree-sequoia
cargo build --release
export PKG_CONFIG_PATH=$PWD/target/release:$PKG_CONFIG_PATH

cd ../..
./autogen.sh --with-pgp
make
```

### Standalone (for development)

```sh
cd src/ostree-sequoia
cargo build --release
cargo test
```

### With a different crypto backend

```sh
cargo build --release --no-default-features --features crypto-rust
```

## C FFI surface

The library exports a small, stable set of C functions:

| Function | Purpose |
|---|---|
| `ostree_sequoia_ctx_new()` | Create an opaque context |
| `ostree_sequoia_ctx_free()` | Free a context |
| `ostree_sequoia_ctx_clear_keys()` | Clear all loaded keys |
| `ostree_sequoia_ctx_add_public_key()` | Add a public key (binary or ASCII-armored) |
| `ostree_sequoia_ctx_set_secret_key()` | Set the signing key |
| `ostree_sequoia_ctx_load_public_keys_from_file()` | Load keys from a keyring file |
| `ostree_sequoia_sign()` | Create a detached OpenPGP signature |
| `ostree_sequoia_verify()` | Verify a detached signature |
| `ostree_sequoia_free_bytes()` | Free a byte buffer returned by sign |
| `ostree_sequoia_free_string()` | Free a string returned by verify/errors |

The C header is generated automatically by `cbindgen` during the build.

## Usage (once integrated)

```sh
# Sign a commit
ostree sign --sign-type=pgp COMMIT_CHECKSUM --keys-file=/path/to/secret.key

# Verify
ostree sign --sign-type=pgp --verify COMMIT_CHECKSUM --keys-file=/path/to/pubkey.asc

# Configure a remote to use PGP verification
ostree remote add myremote \
  --sign-verify=pgp=file:/etc/ostree/trusted.pgp \
  https://example.com/repo

# In repo config:
# [remote "myremote"]
# sign-verify=pgp
# verification-pgp-file=/etc/ostree/trusted.pgp
```

Public keys are loaded from well-known locations:
- `/etc/ostree/trusted.pgp`
- `/etc/ostree/trusted.pgp.d/*.asc`
- `$DATADIR/ostree/trusted.pgp`
- `$DATADIR/ostree/trusted.pgp.d/*.asc`

## Prior art

| Project | What it replaced | Approach | Status |
|---|---|---|---|
| [rpm-sequoia](https://github.com/rpm-software-management/rpm-sequoia) | RPM's pgp interface (GPGME) | Rust lib + C FFI | Shipped (Fedora 36+, RHEL 9+) |
| [podman-sequoia](https://github.com/containers/image/pull/2876) | GPGME in containers/image | Rust lib + CGo FFI | Merged (Aug 2025) |
| [sequoia-octopus-librnp](https://gitlab.com/sequoia-pgp/sequoia-octopus-librnp) | RNP in Thunderbird | Rust lib reimplements C API | Shipped (Fedora, openSUSE) |
| **ostree-sequoia** (this crate) | GPGME in ostree | Rust lib + C FFI | In development |

## License

LGPL-2.0-or-later, matching both ostree and sequoia-openpgp.
