use std::io::Write;
use std::os::unix::fs::MetadataExt;
use std::path::Path;

use anyhow::Result;
use ostree_ext::{gio, glib};
use xshell::cmd;

use crate::test::reboot;

const BINDING_KEYPATH: &str = "/etc/ostree/initramfs-root-binding.key";
const PREPARE_ROOT_PATH: &str = "/etc/ostree/prepare-root.conf";

struct Keypair {
    public: Vec<u8>,
    private: Vec<u8>,
}

fn generate_raw_ed25519_keypair(sh: &xshell::Shell) -> Result<Keypair> {
    let keydata = cmd!(sh, "openssl genpkey -algorithm ed25519 -outform PEM")
        .output()?
        .stdout;
    let mut public = cmd!(sh, "openssl pkey -outform DER -pubout")
        .stdin(&keydata)
        .output()?
        .stdout;
    assert_eq!(public.len(), 44);
    let _ = public.drain(..12);
    let mut seed = cmd!(sh, "openssl pkey -outform DER")
        .stdin(&keydata)
        .stdin(&keydata)
        .output()?
        .stdout;
    assert_eq!(seed.len(), 48);
    let _ = seed.drain(..16);
    assert_eq!(seed.len(), 32);
    let private = seed.iter().chain(&public).copied().collect::<Vec<u8>>();
    Ok(Keypair { public, private })
}

fn read_booted_metadata() -> Result<glib::VariantDict> {
    let metadata = std::fs::read("/run/ostree-booted")?;
    let metadata = glib::Variant::from_bytes::<glib::VariantDict>(&glib::Bytes::from(&metadata));
    Ok(glib::VariantDict::new(Some(&metadata)))
}

fn verify_composefs_sanity(sh: &xshell::Shell, metadata: &glib::VariantDict) -> Result<()> {
    let fstype = cmd!(sh, "findmnt -n -o FSTYPE /").read()?;
    assert_eq!(fstype.as_str(), "overlay");

    assert_eq!(metadata.lookup::<bool>("composefs").unwrap(), Some(true));

    let private_dir = Path::new("/run/ostree/.private");
    assert_eq!(
        std::fs::symlink_metadata(private_dir)?.mode() & !libc::S_IFMT,
        0
    );
    assert!(std::fs::read_dir(private_dir.join("cfsroot-lower"))?
        .next()
        .is_none());

    Ok(())
}

fn prepare_composefs_signed(sh: &xshell::Shell) -> Result<()> {
    let sysroot = ostree_ext::ostree::Sysroot::new_default();
    sysroot.load(gio::Cancellable::NONE)?;

    // Generate a keypair, writing the public half to /etc and the private stays in memory
    let keypair = generate_raw_ed25519_keypair(sh)?;
    let mut pubkey = base64::encode(keypair.public);
    pubkey.push_str("\n");
    std::fs::write(BINDING_KEYPATH, pubkey)?;
    let mut tmp_privkey = tempfile::NamedTempFile::new()?;
    let priv_base64 = base64::encode(keypair.private);
    tmp_privkey
        .as_file_mut()
        .write_all(priv_base64.as_bytes())?;

    // Note rpm-ostree initramfs-etc changes the final commit hash
    std::fs::create_dir_all("/etc/ostree")?;
    std::fs::write(
        PREPARE_ROOT_PATH,
        r##"[composefs]
enabled=signed
"##,
    )?;
    cmd!(
        sh,
        "rpm-ostree initramfs-etc --track {BINDING_KEYPATH} --track {PREPARE_ROOT_PATH}"
    )
    .run()?;

    sysroot.load_if_changed(gio::Cancellable::NONE)?;
    let pending_deployment = sysroot.staged_deployment().expect("staged deployment");
    let target_commit = &pending_deployment.csum();

    // Sign
    let tmp_privkey_path = tmp_privkey.path();
    cmd!(
        sh,
        "ostree sign -s ed25519 --keys-file {tmp_privkey_path} {target_commit}"
    )
    .run()?;
    println!("Signed commit");
    // And verify
    cmd!(
        sh,
        "ostree sign --verify --keys-file {BINDING_KEYPATH} {target_commit}"
    )
    .run()?;

    // We explicitly throw away the private key now
    tmp_privkey.close()?;

    Ok(())
}

fn verify_composefs_signed(sh: &xshell::Shell, metadata: &glib::VariantDict) -> Result<()> {
    verify_composefs_sanity(sh, metadata)?;
    // Verify signature
    assert!(metadata
        .lookup::<String>("composefs.signed")
        .unwrap()
        .is_some());
    cmd!(
        sh,
        "journalctl -u ostree-prepare-root --grep='Validated commit signature'"
    )
    .run()?;
    Ok(())
}

fn verify_disable_composefs(sh: &xshell::Shell, metadata: &glib::VariantDict) -> Result<()> {
    assert_eq!(
        metadata
            .lookup::<bool>("composefs")
            .unwrap()
            .unwrap_or_default(),
        false
    );
    let fstype = cmd!(sh, "findmnt -n -o FSTYPE /").read()?;
    assert_ne!(fstype.as_str(), "overlay");
    Ok(())
}

pub(crate) fn itest_composefs() -> Result<()> {
    let sh = &xshell::Shell::new()?;
    let mark = match crate::test::get_reboot_mark()? {
        None => {
            if !cmd!(sh, "ostree --version").read()?.contains("- composefs") {
                println!("SKIP no composefs support");
                return Ok(());
            }
            {
                let fstype = cmd!(sh, "stat -f /sysroot -c %T").read()?;
                if fstype.trim() == "xfs" {
                    println!("SKIP no xfs fsverity yet");
                    return Ok(());
                }
            }
            cmd!(
                sh,
                "ostree --repo=/ostree/repo config set ex-integrity.composefs true"
            )
            .run()?;
            // A dummy change; TODO add an ostree command for this
            cmd!(sh, "rpm-ostree kargs --append=foo=bar").run()?;
            return Err(crate::test::reboot("1").into());
        }
        Some(v) => v,
    };
    let metadata = read_booted_metadata()?;
    match mark.as_str() {
        "1" => {
            verify_composefs_sanity(sh, &metadata)?;
            prepare_composefs_signed(sh)?;
            Err(reboot("2"))?;
            Ok(())
        }
        "2" => {
            verify_composefs_signed(sh, &metadata)?;
            cmd!(
                sh,
                "rpm-ostree kargs --append=ostree.prepare-root.composefs=0"
            )
            .run()?;
            Err(reboot("3"))
        }
        "3" => verify_disable_composefs(sh, &metadata),
        o => anyhow::bail!("Unrecognized reboot mark {o}"),
    }
}

#[test]
fn gen_keypair() -> Result<()> {
    let sh = &xshell::Shell::new()?;
    let keypair = generate_raw_ed25519_keypair(sh).unwrap();
    assert_eq!(keypair.public.len(), 32);
    assert_eq!(keypair.private.len(), 64);
    Ok(())
}
