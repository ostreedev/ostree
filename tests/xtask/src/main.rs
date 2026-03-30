use anyhow::Result;
use clap::Parser;

mod tmt;

/// Development task runner for ostree integration testing.
#[derive(Debug, Parser)]
enum Opt {
    /// Run TMT tests inside bcvk-deployed VMs.
    ///
    /// Each plan runs in its own VM for isolation, following the
    /// bootc-dev/bootc cargo xtask run-tmt pattern.
    RunTmt(tmt::RunTmtArgs),
}

fn main() -> Result<()> {
    let opt = Opt::parse();
    let sh = xshell::Shell::new()?;
    match opt {
        Opt::RunTmt(args) => tmt::run_tmt(&sh, args),
    }
}
