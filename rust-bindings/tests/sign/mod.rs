use std::process::Command;

use ostree::prelude::SignExt;
use ostree::prelude::*;
use ostree::Sign;
use ostree::{gio, glib};

#[test]
fn sign_api_should_work() {
    let dummy_sign = ostree::Sign::by_name("dummy").unwrap();
    assert_eq!(dummy_sign.name(), "dummy");

    let result = ostree::prelude::SignExt::data(
        &dummy_sign,
        &glib::Bytes::from_static(b"1234"),
        gio::Cancellable::NONE,
    );
    assert!(result.is_err());

    let result = dummy_sign.data_verify(&glib::Bytes::from_static(b"1234"), &"1234".to_variant());
    assert!(result.is_err());

    let result = ostree::Sign::by_name("NOPE");
    assert!(result.is_err());
}

fn inner_sign_ed25519<T: SignExt>(signer: T) {
    assert_eq!(signer.name(), "ed25519");

    let td = tempfile::tempdir().unwrap();
    let path = td.path();

    // Horrible bits to reuse libtest shell script code to generate keys
    let pwd = std::env::current_dir().unwrap();
    let cmd = format!(
        r#". {:?}/tests/libtest.sh
gen_ed25519_keys
echo $ED25519PUBLIC > ed25519.public
echo $ED25519SEED > ed25519.seed
echo $ED25519SECRET > ed25519.secret
"#,
        pwd
    );
    let s = Command::new("bash")
        .env("G_TEST_SRCDIR", pwd)
        .env("OSTREE_HTTPD", "")
        .current_dir(path)
        .args(["-euo", "pipefail"])
        .args(["-c", cmd.as_str()])
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::piped())
        .output()
        .unwrap();
    if !s.status.success() {
        let mut stderr = std::io::stderr().lock();
        let _ = std::io::copy(&mut std::io::Cursor::new(&s.stderr), &mut stderr);
        panic!("failed to source libtest: {:?}", s.status);
    }

    let seckey = std::fs::read_to_string(path.join("ed25519.secret")).unwrap();
    let seckey = seckey.to_variant();
    signer.set_sk(&seckey).unwrap();
    let pubkey = std::fs::read_to_string(path.join("ed25519.public")).unwrap();
    let pubkey = pubkey.to_variant();
    signer.add_pk(&pubkey).unwrap();

    let payload = &glib::Bytes::from_static(b"1234");

    let signature = signer.data(payload, gio::Cancellable::NONE).unwrap();
    let signatures = [&*signature].to_variant();

    let msg = signer.data_verify(payload, &signatures).unwrap().unwrap();
    assert!(msg.starts_with("ed25519: Signature verified successfully"));

    assert!(signer
        .data_verify(&glib::Bytes::from_static(b""), &signatures)
        .is_err());

    let badsigs = [b"".as_slice()].to_variant();

    let e = signer.data_verify(payload, &badsigs).err().unwrap();
    assert!(e.to_string().contains("Ill-formed input"), "{}", e)
}

#[test]
fn sign_ed25519() {
    if let Some(signer) = Sign::by_name("ed25519").ok() {
        inner_sign_ed25519(signer)
    }
}
