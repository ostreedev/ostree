use crate::util::*;
use cap_std::fs::Dir;
use cap_tempfile::cap_std;
use ostree::*;
use std::os::unix::io::AsRawFd;

#[test]
fn should_checkout_at_with_none_options() {
    let test_repo = TestRepo::new();
    let checksum = test_repo.test_commit("test");
    let checkout_dir = tempfile::tempdir().expect("checkout dir");

    let dir = Dir::open_ambient_dir(checkout_dir.path(), cap_std::ambient_authority()).unwrap();
    test_repo
        .repo
        .checkout_at(
            None,
            dir.as_raw_fd(),
            "test-checkout",
            &checksum,
            gio::Cancellable::NONE,
        )
        .expect("checkout at");

    assert_test_file(checkout_dir.path());
}

#[test]
fn should_checkout_at_with_default_options() {
    let test_repo = TestRepo::new();
    let checksum = test_repo.test_commit("test");
    let checkout_dir = tempfile::tempdir().expect("checkout dir");

    let dir = Dir::open_ambient_dir(checkout_dir.path(), cap_std::ambient_authority()).unwrap();
    test_repo
        .repo
        .checkout_at(
            Some(&RepoCheckoutAtOptions::default()),
            dir.as_raw_fd(),
            "test-checkout",
            &checksum,
            gio::Cancellable::NONE,
        )
        .expect("checkout at");

    assert_test_file(checkout_dir.path());
}

#[test]
fn should_checkout_at_with_options() {
    let test_repo = TestRepo::new();
    let checksum = test_repo.test_commit("test");
    let checkout_dir = tempfile::tempdir().expect("checkout dir");

    let dir = Dir::open_ambient_dir(checkout_dir.path(), cap_std::ambient_authority()).unwrap();
    test_repo
        .repo
        .checkout_at(
            Some(&RepoCheckoutAtOptions {
                mode: RepoCheckoutMode::User,
                overwrite_mode: RepoCheckoutOverwriteMode::AddFiles,
                enable_fsync: true,
                devino_to_csum_cache: Some(RepoDevInoCache::new()),
                ..Default::default()
            }),
            dir.as_raw_fd(),
            "test-checkout",
            &checksum,
            gio::Cancellable::NONE,
        )
        .expect("checkout at");

    assert_test_file(checkout_dir.path());
}

#[test]
#[cfg(any(feature = "v2018_2", feature = "dox"))]
fn should_checkout_at_with_filter() {
    use std::path::Path;

    let test_repo = TestRepo::new();
    let checksum = test_repo.test_commit("test");
    let checkout_dir = tempfile::tempdir().expect("checkout dir");

    let dir = Dir::open_ambient_dir(checkout_dir.path(), cap_std::ambient_authority()).unwrap();
    test_repo
        .repo
        .checkout_at(
            Some(&RepoCheckoutAtOptions {
                filter: RepoCheckoutFilter::new(|_repo, path, _stat| {
                    if path == Path::new("/testdir/testfile") {
                        RepoCheckoutFilterResult::Skip
                    } else {
                        RepoCheckoutFilterResult::Allow
                    }
                }),
                ..Default::default()
            }),
            dir.as_raw_fd(),
            "test-checkout",
            &checksum,
            gio::Cancellable::NONE,
        )
        .expect("checkout at");

    let testdir = checkout_dir.path().join("test-checkout").join("testdir");
    assert!(std::fs::read_dir(&testdir).is_ok());
    assert!(std::fs::File::open(&testdir.join("testfile")).is_err());
}
