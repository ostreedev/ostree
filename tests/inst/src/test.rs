use std::fs::File;
use std::io::prelude::*;
use std::path::Path;
use std::time;

use anyhow::{bail, Context, Result};
use rand::Rng;
use xshell::Cmd;

// HTTP Server deps
use futures_util::future;
use hyper::service::{make_service_fn, service_fn};
use hyper::{Body, Request, Response};
use hyper_staticfile::Static;
use tokio::runtime::Runtime;

/// Run command and assert that its stderr contains pat
pub fn cmd_fails_with(cmd: Cmd, pat: &str) -> Result<()> {
    let cmd_str = cmd.to_string();
    let output = cmd.ignore_status().output()?;

    if output.status.success() {
        bail!("Command {:?} unexpectedly succeeded", cmd_str);
    }

    if twoway::find_bytes(&output.stderr, pat.as_bytes()).is_none() {
        dbg!(String::from_utf8_lossy(&output.stdout));
        dbg!(String::from_utf8_lossy(&output.stderr));
        bail!("Command {:?} stderr did not match: {}", cmd_str, pat);
    }

    Ok(())
}

/// Run command and assert that its stdout contains `pat`
pub(crate) fn cmd_has_output(cmd: Cmd, pat: &str) -> Result<()> {
    let cmd_str = cmd.to_string();
    let output = cmd.output()?; // run, error if command fails

    if !output.status.success() {
        bail!("Command {} failed", cmd_str);
    }
    if twoway::find_bytes(&output.stdout, pat.as_bytes()).is_none() {
        dbg!(String::from_utf8_lossy(&output.stdout));
        bail!("Command {} stdout did not match: {}", cmd_str, pat);
    }
    Ok(())
}

pub(crate) fn write_file<P: AsRef<Path>>(p: P, buf: &str) -> Result<()> {
    let p = p.as_ref();
    let mut f = File::create(p)?;
    f.write_all(buf.as_bytes())?;
    f.flush()?;
    Ok(())
}

#[derive(Default, Debug, Copy, Clone)]
pub(crate) struct TestHttpServerOpts {
    pub(crate) basicauth: bool,
    pub(crate) random_delay: Option<time::Duration>,
}

pub(crate) const TEST_HTTP_BASIC_AUTH: &str = "foouser:barpw";

fn validate_authz(value: &[u8]) -> Result<bool> {
    let buf = std::str::from_utf8(&value)?;
    if let Some(o) = buf.find("Basic ") {
        let (_, buf) = buf.split_at(o + "Basic ".len());
        let buf = base64::decode(buf).context("decoding")?;
        let buf = std::str::from_utf8(&buf)?;
        Ok(buf == TEST_HTTP_BASIC_AUTH)
    } else {
        bail!("Missing Basic")
    }
}

pub(crate) async fn http_server<P: AsRef<Path>>(
    p: P,
    opts: TestHttpServerOpts,
) -> Result<std::net::SocketAddr> {
    let addr = ([127, 0, 0, 1], 0).into();
    let sv = Static::new(p.as_ref());

    async fn handle_request<B: std::fmt::Debug>(
        req: Request<B>,
        sv: Static,
        opts: TestHttpServerOpts,
    ) -> Result<Response<Body>> {
        if let Some(random_delay) = opts.random_delay {
            let slices = 100u32;
            let n: u32 = rand::thread_rng().gen_range(0..slices);
            std::thread::sleep((random_delay / slices) * n);
        }
        if opts.basicauth {
            if let Some(ref authz) = req.headers().get(http::header::AUTHORIZATION) {
                match validate_authz(authz.as_ref()) {
                    Ok(true) => {
                        return Ok(sv.clone().serve(req).await?);
                    }
                    Ok(false) => {
                        // Fall through
                    }
                    Err(e) => {
                        return Ok(Response::builder()
                            .status(hyper::StatusCode::INTERNAL_SERVER_ERROR)
                            .body(Body::from(e.to_string()))
                            .unwrap());
                    }
                }
            };
            return Ok(Response::builder()
                .status(hyper::StatusCode::FORBIDDEN)
                .header("x-test-auth", "true")
                .body(Body::from("not authorized\n"))
                .unwrap());
        }
        Ok(sv.clone().serve(req).await?)
    }

    let make_service = make_service_fn(move |_| {
        let sv = sv.clone();
        future::ok::<_, hyper::Error>(service_fn(move |req| handle_request(req, sv.clone(), opts)))
    });
    let server: hyper::Server<_, _, _> = hyper::Server::bind(&addr).serve(make_service);
    let addr = server.local_addr();
    tokio::spawn(async move {
        let r = server.await;
        dbg!("server finished!");
        r
    });
    Ok(addr)
}

pub(crate) fn with_webserver_in<P: AsRef<Path>, F>(
    path: P,
    opts: &TestHttpServerOpts,
    f: F,
) -> Result<()>
where
    F: FnOnce(&std::net::SocketAddr) -> Result<()>,
    F: Send + 'static,
{
    let path = path.as_ref();
    let rt = Runtime::new()?;
    rt.block_on(async move {
        let addr = http_server(path, *opts).await?;
        tokio::task::spawn_blocking(move || f(&addr)).await?
    })?;
    Ok(())
}

/// Parse an environment variable as UTF-8
pub(crate) fn getenv_utf8(n: &str) -> Result<Option<String>> {
    if let Some(v) = std::env::var_os(n) {
        Ok(Some(
            v.to_str()
                .ok_or_else(|| anyhow::anyhow!("{} is invalid UTF-8", n))?
                .to_string(),
        ))
    } else {
        Ok(None)
    }
}

/// Defined by the autopkgtest specification
pub(crate) fn get_reboot_mark() -> Result<Option<String>> {
    getenv_utf8("AUTOPKGTEST_REBOOT_MARK")
}

/// Initiate a clean reboot; on next boot get_reboot_mark() will return `mark`.
#[allow(dead_code)]
pub(crate) fn reboot<M: AsRef<str>>(mark: M) -> anyhow::Error {
    let mark = mark.as_ref();
    use std::os::unix::process::CommandExt;
    if let Err(e) = std::io::stderr().flush() {
        return e.into();
    }
    if let Err(e) = std::io::stdout().flush() {
        return e.into();
    }
    std::process::Command::new("/tmp/autopkgtest-reboot")
        .arg(mark)
        .exec()
        .into()
}

/// Prepare a reboot - you should then initiate a reboot however you like.
/// On next boot get_reboot_mark() will return `mark`.
#[allow(dead_code)]
pub(crate) fn prepare_reboot<M: AsRef<str>>(mark: M) -> Result<()> {
    let mark = mark.as_ref();
    let s = std::process::Command::new("/tmp/autopkgtest-reboot-prepare")
        .arg(mark)
        .status()?;
    if !s.success() {
        anyhow::bail!("{:?}", s);
    }
    Ok(())
}

// I put tests in your tests so you can test while you test
#[cfg(test)]
mod tests {
    use super::*;

    fn oops(sh: &Shell) -> Cmd {
        cmd!(sh, "bash -c 'echo oops 1>&2; exit 1'")
    }

    #[test]
    fn test_fails_with_matches() -> Result<()> {
        cmd_fails_with(cmd!(sh, "false"), "")?;
        cmd_fails_with(oops(&sh), "oops")?;
        Ok(())
    }

    #[test]
    fn test_fails_with_fails() -> Result<()> {
        let sh = Shell::new()?;
        cmd_fails_with(cmd!(sh, "true"), "somepat").expect_err("true");
        cmd_fails_with(oops(&sh), "nomatch").expect_err("nomatch");
        Ok(())
    }

    #[test]
    fn test_output() -> Result<()> {
        let sh = Shell::new()?;
        cmd_has_output(cmd!(sh, "echo foobarbaz; echo fooblahbaz"), "blah")?;
        assert!(cmd_has_output(cmd!(sh, "echo foobarbaz"), "blah").is_err());
        Ok(())
    }

    #[test]
    fn test_validate_authz() -> Result<()> {
        assert!(validate_authz("Basic Zm9vdXNlcjpiYXJwdw==".as_bytes())?);
        assert!(!validate_authz("Basic dW5rbm93bjpiYWRwdw==".as_bytes())?);
        assert!(validate_authz("Basic oops".as_bytes()).is_err());
        assert!(validate_authz("oops".as_bytes()).is_err());
        Ok(())
    }
}
