use crate::util::*;
use gio::NONE_CANCELLABLE;
use ostree::*;
use std::os::unix::io::AsRawFd;
use std::path::PathBuf;

#[test]
fn should_checkout_at_with_none_options() {
    let test_repo = TestRepo::new();
    let checksum = test_repo.test_commit("test");
    let checkout_dir = tempfile::tempdir().expect("checkout dir");

    let dirfd = openat::Dir::open(checkout_dir.path()).expect("openat");
    test_repo
        .repo
        .checkout_at(
            None,
            dirfd.as_raw_fd(),
            "test-checkout",
            &checksum,
            NONE_CANCELLABLE,
        )
        .expect("checkout at");

    assert_test_file(checkout_dir.path());
}

#[test]
fn should_checkout_at_with_default_options() {
    let test_repo = TestRepo::new();
    let checksum = test_repo.test_commit("test");
    let checkout_dir = tempfile::tempdir().expect("checkout dir");

    let dirfd = openat::Dir::open(checkout_dir.path()).expect("openat");
    test_repo
        .repo
        .checkout_at(
            Some(&RepoCheckoutAtOptions::default()),
            dirfd.as_raw_fd(),
            "test-checkout",
            &checksum,
            NONE_CANCELLABLE,
        )
        .expect("checkout at");

    assert_test_file(checkout_dir.path());
}

#[test]
fn should_checkout_at_with_options() {
    let test_repo = TestRepo::new();
    let checksum = test_repo.test_commit("test");
    let checkout_dir = tempfile::tempdir().expect("checkout dir");

    let dirfd = openat::Dir::open(checkout_dir.path()).expect("openat");
    test_repo
        .repo
        .checkout_at(
            Some(&RepoCheckoutAtOptions {
                mode: RepoCheckoutMode::User,
                overwrite_mode: RepoCheckoutOverwriteMode::AddFiles,
                enable_fsync: true,
                force_copy: true,
                force_copy_zerosized: true,
                devino_to_csum_cache: Some(RepoDevInoCache::new()),
                filter: RepoCheckoutFilter::new(|_repo, _path, _stat| {
                    RepoCheckoutFilterResult::Allow
                }),
                ..Default::default()
            }),
            dirfd.as_raw_fd(),
            "test-checkout",
            &checksum,
            NONE_CANCELLABLE,
        )
        .expect("checkout at");

    assert_test_file(checkout_dir.path());
}

#[test]
fn should_checkout_at_with_filter() {
    let test_repo = TestRepo::new();
    let checksum = test_repo.test_commit("test");
    let checkout_dir = tempfile::tempdir().expect("checkout dir");

    let dirfd = openat::Dir::open(checkout_dir.path()).expect("openat");
    test_repo
        .repo
        .checkout_at(
            Some(&RepoCheckoutAtOptions {
                filter: RepoCheckoutFilter::new(|_repo, path, _stat| {
                    if path == PathBuf::from("/testdir/testfile") {
                        RepoCheckoutFilterResult::Skip
                    } else {
                        RepoCheckoutFilterResult::Allow
                    }
                }),
                ..Default::default()
            }),
            dirfd.as_raw_fd(),
            "test-checkout",
            &checksum,
            NONE_CANCELLABLE,
        )
        .expect("checkout at");

    let testdir = checkout_dir.path().join("test-checkout").join("testdir");
    assert!(std::fs::read_dir(&testdir).is_ok());
    assert!(std::fs::File::open(&testdir.join("testfile")).is_err());
}
