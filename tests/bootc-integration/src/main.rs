//! Integration test runner for ostree bootc testing.
//!
//! This binary uses [`libtest_mimic`] as a custom test harness.
//! Tests are registered via the [`integration_test!`] macro in submodules
//! and collected from the [`INTEGRATION_TESTS`] distributed slice at startup.
//!
//! When the `JUNIT_OUTPUT` environment variable is set to a file path,
//! JUnit XML results are written there after tests complete.

// linkme requires unsafe for distributed slices
#![allow(unsafe_code)]

use std::sync::{Arc, Mutex};
use std::time::Instant;

use libtest_mimic::{Arguments, Trial};

pub(crate) use ostree_bootc_integration_tests::{integration_test, INTEGRATION_TESTS};

mod tests;

/// Outcome of a single test, recorded during execution.
struct TestOutcome {
    name: String,
    duration: std::time::Duration,
    result: Result<(), String>,
}

fn main() {
    let args = Arguments::from_args();
    let outcomes: Arc<Mutex<Vec<TestOutcome>>> = Arc::new(Mutex::new(Vec::new()));

    let tests: Vec<Trial> = INTEGRATION_TESTS
        .iter()
        .map(|t| {
            let f = t.f;
            let name = t.name.to_owned();
            let outcomes = Arc::clone(&outcomes);
            Trial::test(t.name, move || {
                let start = Instant::now();
                let result = f();
                let duration = start.elapsed();
                let outcome = TestOutcome {
                    name,
                    duration,
                    result: result.as_ref().map(|_| ()).map_err(|e| format!("{e:?}")),
                };
                outcomes.lock().unwrap().push(outcome);
                result.map_err(|e| format!("{e:?}").into())
            })
        })
        .collect();

    let conclusion = libtest_mimic::run(&args, tests);

    // Write JUnit XML if requested
    if let Ok(path) = std::env::var("JUNIT_OUTPUT") {
        if let Err(e) = write_junit(&path, &outcomes.lock().unwrap()) {
            eprintln!("warning: failed to write JUnit XML to {path}: {e}");
        }
    }

    std::process::exit(if conclusion.has_failed() { 101 } else { 0 });
}

fn write_junit(path: &str, outcomes: &[TestOutcome]) -> anyhow::Result<()> {
    use quick_junit::{NonSuccessKind, Report, TestCase, TestCaseStatus, TestSuite};

    let mut report = Report::new("ostree-bootc-integration-tests");
    let mut suite = TestSuite::new("privileged");

    for outcome in outcomes {
        let status = match &outcome.result {
            Ok(()) => TestCaseStatus::success(),
            Err(msg) => {
                let mut status = TestCaseStatus::non_success(NonSuccessKind::Failure);
                status.set_message(msg.clone());
                status
            }
        };
        let mut tc = TestCase::new(outcome.name.clone(), status);
        tc.set_time(outcome.duration);
        suite.add_test_case(tc);
    }

    report.add_test_suite(suite);
    let xml = report
        .to_string()
        .map_err(|e| anyhow::anyhow!("JUnit serialization failed: {e}"))?;
    std::fs::write(path, xml)?;
    eprintln!("JUnit XML written to {path}");
    Ok(())
}
