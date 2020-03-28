use std::borrow::BorrowMut;
use std::fs::File;
use std::io::prelude::*;
use std::path::Path;
use std::process::Command;

use anyhow::{bail, Context, Result};
use linkme::distributed_slice;

pub use itest_macro::itest;
pub use with_procspawn_tempdir::with_procspawn_tempdir;

// HTTP Server deps
use futures_util::future;
use hyper::service::{make_service_fn, service_fn};
use hyper::{Body, Request, Response};
use hyper_staticfile::Static;

pub(crate) type TestFn = fn() -> Result<()>;

#[derive(Debug)]
pub(crate) struct Test {
    pub(crate) name: &'static str,
    pub(crate) f: TestFn,
}

pub(crate) type TestImpl = libtest_mimic::Test<&'static Test>;

#[distributed_slice]
pub(crate) static TESTS: [Test] = [..];

/// Run command and assert that its stderr contains pat
pub(crate) fn cmd_fails_with<C: BorrowMut<Command>>(mut c: C, pat: &str) -> Result<()> {
    let c = c.borrow_mut();
    let o = c.output()?;
    if o.status.success() {
        bail!("Command {:?} unexpectedly succeeded", c);
    }
    if !twoway::find_bytes(&o.stderr, pat.as_bytes()).is_some() {
        dbg!(String::from_utf8_lossy(&o.stdout));
        dbg!(String::from_utf8_lossy(&o.stderr));
        bail!("Command {:?} stderr did not match: {}", c, pat);
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

pub(crate) fn mkroot<P: AsRef<Path>>(p: P) -> Result<()> {
    let p = p.as_ref();
    for v in &["usr/bin", "etc"] {
        std::fs::create_dir_all(p.join(v))?;
    }
    let verpath = p.join("etc/version");
    let v: u32 = if verpath.exists() {
        let s = std::fs::read_to_string(&verpath)?;
        let v: u32 = s.trim_end().parse()?;
        v + 1
    } else {
        0
    };
    write_file(&verpath, &format!("{}", v))?;
    write_file(p.join("usr/bin/somebinary"), &format!("somebinary v{}", v))?;
    write_file(p.join("etc/someconf"), &format!("someconf v{}", v))?;
    write_file(p.join("usr/bin/vmod2"), &format!("somebinary v{}", v % 2))?;
    write_file(p.join("usr/bin/vmod3"), &format!("somebinary v{}", v % 3))?;
    Ok(())
}

#[derive(Default, Debug, Copy, Clone)]
pub(crate) struct TestHttpServerOpts {
    pub(crate) basicauth: bool,
}

pub(crate) const TEST_HTTP_BASIC_AUTH: &'static str = "foouser:barpw";

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
        let opts = opts.clone();
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

// I put tests in your tests so you can test while you test
#[cfg(test)]
mod tests {
    use super::*;

    fn oops() -> Command {
        let mut c = Command::new("/bin/bash");
        c.args(&["-c", "echo oops 1>&2; exit 1"]);
        c
    }

    #[test]
    fn test_fails_with_matches() -> Result<()> {
        cmd_fails_with(Command::new("false"), "")?;
        cmd_fails_with(oops(), "oops")?;
        Ok(())
    }

    #[test]
    fn test_fails_with_fails() {
        cmd_fails_with(Command::new("true"), "somepat").expect_err("true");
        cmd_fails_with(oops(), "nomatch").expect_err("nomatch");
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
