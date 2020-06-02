use anyhow::Result;
// use structopt::StructOpt;
// // https://github.com/clap-rs/clap/pull/1397
// #[macro_use]
// extern crate clap;

mod repobin;
mod sysroot;
mod test;

fn gather_tests() -> Vec<test::TestImpl> {
    test::TESTS
        .iter()
        .map(|t| libtest_mimic::Test {
            name: t.name.into(),
            kind: "".into(),
            is_ignored: false,
            is_bench: false,
            data: t,
        })
        .collect()
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
    procspawn::init();

    // Ensure we're always in tempdir so we can rely on it globally
    let tmp_dir = tempfile::Builder::new()
        .prefix("ostree-insttest-top")
        .tempdir()?;
    std::env::set_current_dir(tmp_dir.path())?;

    let args = libtest_mimic::Arguments::from_args();
    let tests = gather_tests();
    libtest_mimic::run_tests(&args, tests, run_test).exit();
}
