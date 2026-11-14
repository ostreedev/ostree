//! Tests that mostly use the CLI and operate on temporary
//! repositories.

use std::path::Path;

use crate::test::*;
use anyhow::{Context, Result};
use with_procspawn_tempdir::with_procspawn_tempdir;
use xshell::cmd;

pub(crate) fn itest_basic() -> Result<()> {
    let sh = xshell::Shell::new()?;
    cmd!(sh, "ostree --help >/dev/null").run()?;
    Ok(())
}

#[with_procspawn_tempdir]
pub(crate) fn itest_nofifo() -> Result<()> {
    let sh = xshell::Shell::new()?;
    assert!(std::path::Path::new(".procspawn-tmpdir").exists());
    cmd!(sh, "ostree --repo=repo init --mode=archive").run()?;
    cmd!(sh, "mkdir tmproot").run()?;
    cmd!(sh, "mkfifo tmproot/afile").run()?;
    cmd_fails_with(
        cmd!(
            sh,
            "ostree --repo=repo commit -b fifotest -s 'commit fifo' --tree=dir=./tmproot"
        ),
        "Not a regular file or symlink",
    )?;
    Ok(())
}

#[with_procspawn_tempdir]
pub(crate) fn itest_mtime() -> Result<()> {
    let sh = xshell::Shell::new()?;
    cmd!(sh, "ostree --repo=repo init --mode=archive").run()?;
    cmd!(sh, "mkdir tmproot").run()?;
    cmd!(sh, "echo afile > tmproot/afile").run()?;
    cmd!(
        sh,
        "ostree --repo=repo commit -b test --tree=dir=tmproot >/dev/null"
    )
    .run()?;
    let ts = Path::new("repo").metadata()?.modified().unwrap();
    std::thread::sleep(std::time::Duration::from_secs(1));
    cmd!(
        sh,
        "ostree --repo=repo commit -b test -s 'bump mtime' --tree=dir=tmproot >/dev/null"
    )
    .run()?;
    assert_ne!(ts, Path::new("repo").metadata()?.modified().unwrap());
    Ok(())
}

#[with_procspawn_tempdir]
pub(crate) fn itest_extensions() -> Result<()> {
    let sh = xshell::Shell::new()?;
    cmd!(sh, "ostree --repo=repo init --mode=bare").run()?;
    assert!(Path::new("repo/extensions").exists());
    Ok(())
}

#[with_procspawn_tempdir]
pub(crate) fn itest_pull_basicauth() -> Result<()> {
    let opts = TestHttpServerOpts {
        basicauth: true,
        ..Default::default()
    };
    let serverrepo = Path::new("server/repo");
    let sh = xshell::Shell::new()?;
    std::fs::create_dir_all(&serverrepo)?;
    with_webserver_in(&serverrepo, &opts, move |addr| {
        let baseuri =
            http::Uri::from_maybe_shared(format!("http://{}/", addr).into_bytes())?.to_string();
        let unauthuri =
            http::Uri::from_maybe_shared(format!("http://unknown:badpw@{}/", addr).into_bytes())?
                .to_string();
        let authuri = http::Uri::from_maybe_shared(
            format!("http://{}@{}/", TEST_HTTP_BASIC_AUTH, addr).into_bytes(),
        )?
        .to_string();
        let osroot = Path::new("osroot");
        crate::treegen::mkroot(&osroot)?;
        cmd!(sh, "ostree --repo={serverrepo} init --mode=archive").run()?;
        cmd!(
            sh,
            "ostree --repo={serverrepo} commit -b os --tree=dir={osroot} >/dev/null"
        )
        .run()?;
        let basedir = sh.current_dir();
        let dir = sh.create_dir("client")?;
        let _p = sh.push_dir(dir);
        cmd!(sh, "ostree --repo=repo init --mode=archive").run()?;
        cmd!(
            sh,
            "ostree --repo=repo remote add --set=gpg-verify=false origin-unauth {baseuri}"
        )
        .run()?;
        cmd!(
            sh,
            "ostree --repo=repo remote add --set=gpg-verify=false origin-badauth {unauthuri}"
        )
        .run()?;
        cmd!(
            sh,
            "ostree --repo=repo remote add --set=gpg-verify=false origin-goodauth {authuri}"
        )
        .run()?;

        let _p = sh.push_dir(basedir);
        for rem in &["unauth", "badauth"] {
            cmd_fails_with(
                cmd!(sh, "ostree --repo=client/repo pull origin-{rem} os")
                    .quiet()
                    .ignore_stdout(),
                "HTTP 403",
            )
            .context(rem)?;
        }
        cmd!(sh, "ostree --repo=client/repo pull origin-goodauth os")
            .ignore_stdout()
            .run()?;
        Ok(())
    })?;
    Ok(())
}
