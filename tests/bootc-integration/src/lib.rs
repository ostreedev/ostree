//! Integration test infrastructure for ostree bootc testing.
//!
//! Provides test registration via [`linkme`] distributed slices and the
//! [`integration_test!`] macro, collected and executed by a custom
//! [`libtest_mimic`] harness in `main.rs`.

// linkme requires unsafe for distributed slices
#![allow(unsafe_code)]

/// A test function that returns a Result.
pub type TestFn = fn() -> anyhow::Result<()>;

/// Metadata for a registered integration test.
#[derive(Debug)]
pub struct IntegrationTest {
    /// Name of the integration test.
    pub name: &'static str,
    /// Test function to execute.
    pub f: TestFn,
}

impl IntegrationTest {
    /// Create a new integration test with the given name and function.
    pub const fn new(name: &'static str, f: TestFn) -> Self {
        Self { name, f }
    }
}

/// Distributed slice holding all registered integration tests.
#[linkme::distributed_slice]
pub static INTEGRATION_TESTS: [IntegrationTest];

/// Register an integration test function.
///
/// # Examples
///
/// ```ignore
/// fn test_something() -> anyhow::Result<()> {
///     Ok(())
/// }
/// integration_test!(test_something);
/// ```
#[macro_export]
macro_rules! integration_test {
    ($fn_name:ident) => {
        ::paste::paste! {
            #[::linkme::distributed_slice($crate::INTEGRATION_TESTS)]
            static [<$fn_name:upper>]: $crate::IntegrationTest =
                $crate::IntegrationTest::new(stringify!($fn_name), $fn_name);
        }
    };
}
