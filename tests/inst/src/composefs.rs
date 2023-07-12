use anyhow::Result;
use ostree_ext::glib;
use xshell::cmd;

pub(crate) fn itest_composefs() -> Result<()> {
    let sh = xshell::Shell::new()?;
    if !cmd!(sh, "ostree --version").read()?.contains("- composefs") {
        println!("SKIP no composefs support");
        return Ok(());
    }
    let mark = match crate::test::get_reboot_mark()? {
        None => {
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
    if mark != "1" {
        anyhow::bail!("Invalid reboot mark: {mark}")
    }

    let fstype = cmd!(sh, "findmnt -n -o FSTYPE /").read()?;
    assert_eq!(fstype.as_str(), "overlay");

    let metadata = std::fs::read("/run/ostree-booted")?;
    let metadata = glib::Variant::from_bytes::<glib::VariantDict>(&glib::Bytes::from(&metadata));
    let metadata = glib::VariantDict::new(Some(&metadata));

    assert_eq!(metadata.lookup::<bool>("composefs").unwrap(), Some(true));

    Ok(())
}
