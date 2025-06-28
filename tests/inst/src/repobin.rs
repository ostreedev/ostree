//! Tests that mostly use the CLI and operate on temporary
//! repositories.

use std::path::Path;

use crate::test::*;
use anyhow::{Context, Result};
use sh_inline::bash_command;
use with_procspawn_tempdir::with_procspawn_tempdir;
use xshell::cmd;

pub(crate) fn itest_basic() -> Result<()> {
    let sh = xshell::Shell::new()?;
    cmd!(sh, r"ostree --help >/dev/null").run()?;
    Ok(())
}

#[with_procspawn_tempdir]
pub(crate) fn itest_nofifo() -> Result<()> {
    let sh = xshell::Shell::new()?;
    assert!(std::path::Path::new(".procspawn-tmpdir").exists());
    cmd!(
        sh,
        r"ostree --repo=repo init --mode=archive
    mkdir tmproot
    mkfifo tmproot/afile
"
    )
    .run()?;
    cmd_fails_with(
        bash_command!(
            r#"ostree --repo=repo commit -b fifotest -s "commit fifo" --tree=dir=./tmproot"#
        )
        .unwrap(),
        "Not a regular file or symlink",
    )?;
    Ok(())
}

#[with_procspawn_tempdir]
pub(crate) fn itest_mtime() -> Result<()> {
    let sh = xshell::Shell::new()?;
    cmd!(
        sh,
        r"ostree --repo=repo init --mode=archive
    mkdir tmproot
    echo afile > tmproot/afile
    ostree --repo=repo commit -b test --tree=dir=tmproot >/dev/null
"
    )
    .run()?;
    let ts = Path::new("repo").metadata()?.modified().unwrap();
    std::thread::sleep(std::time::Duration::from_secs(1));
    cmd!(
        sh,
        r#"ostree --repo=repo commit -b test -s "bump mtime" --tree=dir=tmproot >/dev/null"#
    )
    .run()?;
    assert_ne!(ts, Path::new("repo").metadata()?.modified().unwrap());
    Ok(())
}

#[with_procspawn_tempdir]
pub(crate) fn itest_extensions() -> Result<()> {
    let sh = xshell::Shell::new()?;
    cmd!(sh, r"ostree --repo=repo init --mode=bare").run()?;
    assert!(Path::new("repo/extensions").exists());
    Ok(())
}

#[with_procspawn_tempdir]
pub(crate) fn itest_pull_basicauth() -> Result<()> {
    let sh = xshell::Shell::new()?;
    let opts = TestHttpServerOpts {
        basicauth: true,
        ..Default::default()
    };
    let serverrepo = Path::new("server/repo");
    std::fs::create_dir_all(&serverrepo)?;
    with_webserver_in(&serverrepo, &opts, move |addr| {
        let baseuri = http::Uri::from_maybe_shared(format!("http://{}/", addr).into_bytes())?;
        let unauthuri =
            http::Uri::from_maybe_shared(format!("http://unknown:badpw@{}/", addr).into_bytes())?;
        let authuri = http::Uri::from_maybe_shared(
            format!("http://{}@{}/", TEST_HTTP_BASIC_AUTH, addr).into_bytes(),
        )?;
        let osroot = Path::new("osroot");
        crate::treegen::mkroot(&osroot)?;
        cmd!(
            sh,
            r#"ostree --repo=${serverrepo} init --mode=archive
        ostree --repo=${serverrepo} commit -b os --tree=dir=${osroot} >/dev/null
        mkdir client
        cd client
        ostree --repo=repo init --mode=archive
        ostree --repo=repo remote add --set=gpg-verify=false origin-unauth ${baseuri}
        ostree --repo=repo remote add --set=gpg-verify=false origin-badauth ${unauthuri}
        ostree --repo=repo remote add --set=gpg-verify=false origin-goodauth ${authuri}
        "#,
            osroot = osroot,
            serverrepo = serverrepo,
            baseuri = baseuri.to_string(),
            unauthuri = unauthuri.to_string(),
            authuri = authuri.to_string()
        )
        .run()?;
        for rem in &["unauth", "badauth"] {
            cmd_fails_with(
                bash_command!(
                    r#"ostree --repo=client/repo pull origin-${rem} os >/dev/null"#,
                    rem = *rem
                )
                .unwrap(),
                "HTTP 403",
            )
            .context(rem)?;
        }
        cmd!(
            sh,
            r#"ostree --repo=client/repo pull origin-goodauth os >/dev/null"#,
        )
        .run()?;
        Ok(())
    })?;
    Ok(())
}
