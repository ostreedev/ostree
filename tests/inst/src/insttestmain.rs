use anyhow::{bail, Result};
use structopt::StructOpt;

mod destructive;
mod fsverity;
mod repobin;
mod rpmostree;
mod sysroot;
mod test;
mod treegen;

// Written by Ignition
const DESTRUCTIVE_TEST_STAMP: &'static str = "/etc/ostree-destructive-test-ok";

#[derive(Debug, StructOpt)]
#[structopt(rename_all = "kebab-case")]
/// Main options struct
enum Opt {
    /// List the destructive tests
    ListDestructive,
    /// Run a destructive test (requires ostree-based host, may break it!)
    RunDestructive { name: String },
    /// Run the non-destructive tests
    NonDestructive(NonDestructiveOpts),
}

#[derive(Debug, StructOpt)]
#[structopt(rename_all = "kebab-case")]
enum NonDestructiveOpts {
    #[structopt(external_subcommand)]
    Args(Vec<String>),
}

fn libtest_from_test(t: &'static test::Test) -> test::TestImpl {
    libtest_mimic::Test {
        name: t.name.into(),
        kind: "".into(),
        is_ignored: false,
        is_bench: false,
        data: t,
    }
}

fn run_test(test: &test::TestImpl) -> libtest_mimic::Outcome {
    if let Err(e) = (test.data.f)() {
        libtest_mimic::Outcome::Failed {
            msg: Some(e.to_string()),
        }
    } else {
        libtest_mimic::Outcome::Passed
    }
}

fn main() -> Result<()> {
    // Ensure we're always in tempdir so we can rely on it globally.
    // We use /var/tmp to ensure we have storage space in the destructive
    // case.
    let tmp_dir = tempfile::Builder::new()
        .prefix("ostree-insttest-top")
        .tempdir_in("/var/tmp")?;
    std::env::set_current_dir(tmp_dir.path())?;

    procspawn::init();
    let args: Vec<String> = std::env::args().collect();
    let opt = {
        if args.len() == 1 {
            println!("No arguments provided, running non-destructive tests");
            Opt::NonDestructive(NonDestructiveOpts::Args(Vec::new()))
        } else {
            Opt::from_iter(args.iter())
        }
    };

    match opt {
        Opt::ListDestructive => {
            for t in test::DESTRUCTIVE_TESTS.iter() {
                println!("{}", t.name);
            }
            return Ok(());
        }
        Opt::NonDestructive(subopt) => {
            // FIXME add method to parse subargs
            let iter = match subopt {
                NonDestructiveOpts::Args(subargs) => subargs,
            };
            let libtestargs = libtest_mimic::Arguments::from_iter(iter);
            let tests: Vec<_> = test::NONDESTRUCTIVE_TESTS
                .iter()
                .map(libtest_from_test)
                .collect();
            libtest_mimic::run_tests(&libtestargs, tests, run_test).exit();
        }
        Opt::RunDestructive { name } => {
            if !std::path::Path::new(DESTRUCTIVE_TEST_STAMP).exists() {
                bail!(
                    "This is a destructive test; signal acceptance by creating {}",
                    DESTRUCTIVE_TEST_STAMP
                )
            }
            if !std::path::Path::new("/run/ostree-booted").exists() {
                bail!("An ostree-based host is required")
            }

            for t in test::DESTRUCTIVE_TESTS.iter() {
                if t.name == name {
                    (t.f)()?;
                    println!("ok destructive test: {}", t.name);
                    return Ok(());
                }
            }
            bail!("Unknown destructive test: {}", name);
        }
    }
}
