use anyhow::Result;
use serde_derive::Deserialize;
use serde_json;
use std::process::{Command, Stdio};

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
    let cmd = Command::new("rpm-ostree")
        .args(&["status", "--json"])
        .stdout(Stdio::piped())
        .spawn()?;
    Ok(serde_json::from_reader(cmd.stdout.unwrap())?)
}
