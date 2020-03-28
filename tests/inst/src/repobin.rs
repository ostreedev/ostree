//! Tests that mostly use the CLI and operate on temporary
//! repositories.

use std::path::Path;

use crate::test::*;
use anyhow::{Context, Result};
use commandspec::{sh_command, sh_execute};
use tokio::runtime::Runtime;
use with_procspawn_tempdir::with_procspawn_tempdir;

#[itest]
fn test_basic() -> Result<()> {
    sh_execute!(r"ostree --help >/dev/null")?;
    Ok(())
}

#[itest]
#[with_procspawn_tempdir]
fn test_nofifo() -> Result<()> {
    assert!(std::path::Path::new(".procspawn-tmpdir").exists());
    sh_execute!(
        r"ostree --repo=repo init --mode=archive
    mkdir tmproot
    mkfifo tmproot/afile
"
    )?;
    cmd_fails_with(
        sh_command!(
            r#"ostree --repo=repo commit -b fifotest -s "commit fifo" --tree=dir=./tmproot"#
        )
        .unwrap(),
        "Not a regular file or symlink",
    )?;
    Ok(())
}

#[itest]
#[with_procspawn_tempdir]
fn test_mtime() -> Result<()> {
    sh_execute!(
        r"ostree --repo=repo init --mode=archive
    mkdir tmproot
    echo afile > tmproot/afile
    ostree --repo=repo commit -b test --tree=dir=tmproot >/dev/null
"
    )?;
    let ts = Path::new("repo").metadata()?.modified().unwrap();
    sh_execute!(
        r#"ostree --repo=repo commit -b test -s "bump mtime" --tree=dir=tmproot >/dev/null"#
    )?;
    assert_ne!(ts, Path::new("repo").metadata()?.modified().unwrap());
    Ok(())
}

#[itest]
#[with_procspawn_tempdir]
fn test_extensions() -> Result<()> {
    sh_execute!(r"ostree --repo=repo init --mode=bare")?;
    assert!(Path::new("repo/extensions").exists());
    Ok(())
}

async fn impl_test_pull_basicauth() -> Result<()> {
    let opts = TestHttpServerOpts {
        basicauth: true,
        ..Default::default()
    };
    let serverrepo = Path::new("server/repo");
    std::fs::create_dir_all(&serverrepo)?;
    let addr = http_server(&serverrepo, opts).await?;
    tokio::task::spawn_blocking(move || -> Result<()> {
        let baseuri = http::Uri::from_maybe_shared(format!("http://{}/", addr).into_bytes())?;
        let unauthuri =
            http::Uri::from_maybe_shared(format!("http://unknown:badpw@{}/", addr).into_bytes())?;
        let authuri = http::Uri::from_maybe_shared(
            format!("http://{}@{}/", TEST_HTTP_BASIC_AUTH, addr).into_bytes(),
        )?;
        let osroot = Path::new("osroot");
        mkroot(&osroot)?;
        sh_execute!(
            r#"ostree --repo={serverrepo} init --mode=archive
        ostree --repo={serverrepo} commit -b os --tree=dir={osroot} >/dev/null
        mkdir client
        cd client
        ostree --repo=repo init --mode=archive
        ostree --repo=repo remote add --set=gpg-verify=false origin-unauth {baseuri}
        ostree --repo=repo remote add --set=gpg-verify=false origin-badauth {unauthuri}
        ostree --repo=repo remote add --set=gpg-verify=false origin-goodauth {authuri}
        "#,
            osroot = osroot.to_str(),
            serverrepo = serverrepo.to_str(),
            baseuri = baseuri.to_string(),
            unauthuri = unauthuri.to_string(),
            authuri = authuri.to_string()
        )?;
        for rem in &["unauth", "badauth"] {
            cmd_fails_with(
                sh_command!(
                    r#"ostree --repo=client/repo pull origin-{rem} os >/dev/null"#,
                    rem = *rem
                )
                .unwrap(),
                "HTTP 403",
            )
            .context(rem)?;
        }
        sh_execute!(r#"ostree --repo=client/repo pull origin-goodauth os >/dev/null"#,)?;
        Ok(())
    })
    .await??;
    Ok(())
}

#[itest]
#[with_procspawn_tempdir]
fn test_pull_basicauth() -> Result<()> {
    let mut rt = Runtime::new()?;
    rt.block_on(async move { impl_test_pull_basicauth().await })?;
    Ok(())
}
