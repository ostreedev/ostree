use anyhow::{Context, Result};
use serde_derive::Deserialize;
use std::process::Command;

#[derive(Deserialize)]
#[serde(rename_all = "kebab-case")]
#[allow(unused)]
pub(crate) struct Status {
    pub(crate) deployments: Vec<Deployment>,
}

#[derive(Deserialize)]
#[serde(rename_all = "kebab-case")]
#[allow(unused)]
pub(crate) struct Deployment {
    pub(crate) unlocked: Option<String>,
    pub(crate) osname: String,
    pub(crate) pinned: bool,
    pub(crate) checksum: String,
    pub(crate) staged: Option<bool>,
    pub(crate) booted: bool,
    pub(crate) serial: u32,
    pub(crate) origin: String,
}

pub(crate) fn query_status() -> Result<Status> {
    // Retry on temporary activation failures, see
    // https://github.com/coreos/rpm-ostree/issues/2531
    let pause = std::time::Duration::from_secs(1);
    let mut retries = 0;
    let cmd_res = loop {
        retries += 1;
        let res = Command::new("rpm-ostree")
            .args(&["status", "--json"])
            .output()
            .context("failed to spawn 'rpm-ostree status'")?;

        if res.status.success() || retries >= 10 {
            break res;
        }
        std::thread::sleep(pause);
    };

    if !cmd_res.status.success() {
        anyhow::bail!(
            "running 'rpm-ostree status' failed: {}",
            String::from_utf8_lossy(&cmd_res.stderr)
        );
    }

    serde_json::from_slice(&cmd_res.stdout).context("failed to parse 'rpm-ostree status' output")
}
