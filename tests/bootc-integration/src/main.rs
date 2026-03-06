//! Integration test runner for ostree bootc testing.
//!
//! This binary uses [`libtest_mimic`] as a custom test harness.
//! Tests are registered via the [`integration_test!`] macro in submodules
//! and collected from the [`INTEGRATION_TESTS`] distributed slice at startup.

// linkme requires unsafe for distributed slices
#![allow(unsafe_code)]

use libtest_mimic::{Arguments, Trial};

pub(crate) use ostree_bootc_integration_tests::{integration_test, INTEGRATION_TESTS};

mod tests;

fn main() {
    let args = Arguments::from_args();

    let tests: Vec<Trial> = INTEGRATION_TESTS
        .iter()
        .map(|t| {
            let f = t.f;
            Trial::test(t.name, move || f().map_err(|e| format!("{e:?}").into()))
        })
        .collect();

    libtest_mimic::run(&args, tests).exit();
}
