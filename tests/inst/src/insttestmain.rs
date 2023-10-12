use anyhow::{bail, Result};
use libtest_mimic::Trial;
use structopt::StructOpt;

mod composefs;
mod destructive;
mod repobin;
mod sysroot;
mod test;
mod treegen;

// Written by Ignition
const DESTRUCTIVE_TEST_STAMP: &str = "/etc/ostree-destructive-test-ok";

macro_rules! test {
    ($f: path) => {
        (stringify!($f), $f)
    };
}

type StaticTest = (&'static str, fn() -> Result<()>);

const TESTS: &[StaticTest] = &[
    test!(sysroot::itest_sysroot_ro),
    test!(sysroot::itest_immutable_bit),
    test!(sysroot::itest_tmpfiles),
    test!(sysroot::itest_osinit_unshare),
    test!(repobin::itest_basic),
    test!(repobin::itest_nofifo),
    test!(repobin::itest_mtime),
    test!(repobin::itest_extensions),
    test!(repobin::itest_pull_basicauth),
];
const DESTRUCTIVE_TESTS: &[StaticTest] = &[
    test!(destructive::itest_transactionality),
    test!(composefs::itest_composefs),
];

#[derive(Debug, StructOpt)]
#[structopt(rename_all = "kebab-case")]
#[allow(clippy::enum_variant_names)]
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
            for t in DESTRUCTIVE_TESTS {
                println!("{}", t.0);
            }
            Ok(())
        }
        Opt::NonDestructive(subopt) => {
            // FIXME add method to parse subargs
            let NonDestructiveOpts::Args(iter) = subopt;
            let libtestargs = libtest_mimic::Arguments::from_iter(iter);
            let tests: Vec<_> = TESTS
                .iter()
                .map(|(name, fun)| Trial::test(*name, move || fun().map_err(Into::into)))
                .collect();
            libtest_mimic::run(&libtestargs, tests).exit();
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

            for (tname, f) in DESTRUCTIVE_TESTS {
                if *tname == name.as_str() {
                    (f)()?;
                    println!("ok destructive test: {}", tname);
                    return Ok(());
                }
            }
            bail!("Unknown destructive test: {}", name);
        }
    }
}
