use crate::util::*;
#[cfg(feature = "v2017_10")]
use cap_std::fs::Dir;
#[cfg(feature = "v2017_10")]
use cap_tempfile::cap_std;
use ostree::prelude::*;
use ostree::{ObjectName, ObjectType};
#[cfg(feature = "v2017_10")]
use std::os::fd::AsFd;

#[cfg(any(feature = "v2016_8", feature = "dox"))]
mod checkout_at;
mod generate_static;

#[test]
fn should_commit_content_to_repo_and_list_refs_again() {
    let test_repo = TestRepo::new();

    assert!(test_repo.repo.require_rev("nosuchrev").is_err());

    let mtree = create_mtree(&test_repo.repo);
    let checksum = commit(&test_repo.repo, &mtree, "test");

    assert_eq!(test_repo.repo.require_rev("test").unwrap(), checksum);

    let repo = ostree::Repo::new_for_path(test_repo.dir.path());
    repo.open(gio::Cancellable::NONE).expect("OSTree test_repo");
    let refs = repo
        .list_refs(None, gio::Cancellable::NONE)
        .expect("failed to list refs");
    assert_eq!(1, refs.len());
    assert_eq!(checksum, refs["test"]);
}

#[test]
fn list_commits() {
    let cancellable = ostree::gio::Cancellable::NONE;
    let test_repo = TestRepo::new();

    for prefix in [None, Some("a"), Some("0abcde")] {
        let commits = test_repo
            .repo
            .list_commit_objects_starting_with(prefix, cancellable)
            .unwrap();
        assert_eq!(commits.len(), 0);
    }

    let rev = test_repo.test_commit("testref");

    for prefix in [None, Some(&rev[0..1]), Some(&rev[0..5])] {
        let commits = test_repo
            .repo
            .list_commit_objects_starting_with(prefix, cancellable)
            .unwrap()
            .into_iter()
            .collect::<Vec<_>>();
        assert_eq!(commits.len(), 1);
        assert_eq!(commits[0].as_str(), rev.as_str());
    }
}

#[test]
#[cfg(feature = "v2017_10")]
fn cap_std_commit() {
    let test_repo = CapTestRepo::new();

    assert!(test_repo.dir.exists("config"));
    // Also test re-acquiring a new dfd
    assert!(Dir::reopen_dir(&test_repo.repo.dfd_borrow())
        .unwrap()
        .exists("config"));

    assert!(test_repo.repo.require_rev("nosuchrev").is_err());

    let mtree = create_mtree(&test_repo.repo);
    let checksum = commit(&test_repo.repo, &mtree, "test");

    assert_eq!(test_repo.repo.require_rev("test").unwrap(), checksum);

    let repo2 = ostree::Repo::open_at_dir(test_repo.dir.as_fd(), ".").unwrap();
    let refs = repo2
        .list_refs(None, gio::Cancellable::NONE)
        .expect("failed to list refs");
    assert_eq!(1, refs.len());
    assert_eq!(checksum, refs["test"]);
}

#[test]
fn repo_traverse_and_read() {
    let test_repo = TestRepo::new();
    let checksum = test_repo.test_commit("test");

    let objects = test_repo
        .repo
        .traverse_commit(&checksum, -1, gio::Cancellable::NONE)
        .expect("traverse commit");

    assert_eq!(
        maplit::hashset!(
            ObjectName::new(
                "89f84ca9854a80e85b583e46a115ba4985254437027bad34f0b113219323d3f8",
                ObjectType::File
            ),
            ObjectName::new(
                "5280a884f930cae329e2e39d52f2c8e910c2ef4733216b67679db32a2b56c4db",
                ObjectType::DirTree
            ),
            ObjectName::new(
                "c81acde323d73f8639fc84f1ded17bbafc415e645f845e9f3b16a4906857c2d4",
                ObjectType::DirTree
            ),
            ObjectName::new(
                "ad49a0f4e3bc165361b6d17e8a865d479b373ee67d89ac6f0ce871f27da1be6d",
                ObjectType::DirMeta
            ),
            ObjectName::new(checksum, ObjectType::Commit)
        ),
        objects
    );

    let dirmeta = test_repo
        .repo
        .read_dirmeta("ad49a0f4e3bc165361b6d17e8a865d479b373ee67d89ac6f0ce871f27da1be6d")
        .unwrap();
    // Right now, the uid/gid are actually that of the test runner
    assert_eq!(dirmeta.mode, 0o40750);

    let (finfo, _xattrs) = test_repo
        .repo
        .query_file(
            "89f84ca9854a80e85b583e46a115ba4985254437027bad34f0b113219323d3f8",
            gio::Cancellable::NONE,
        )
        .unwrap();
    assert_eq!(finfo.size(), 5);
}

#[test]
fn should_checkout_tree() {
    let test_repo = TestRepo::new();
    let _ = test_repo.test_commit("test");

    let checkout_dir = tempfile::tempdir().expect("checkout dir");
    let file = test_repo
        .repo
        .read_commit("test", gio::Cancellable::NONE)
        .expect("read commit")
        .0
        .downcast::<ostree::RepoFile>()
        .expect("RepoFile");
    let info = file
        .query_info("*", gio::FileQueryInfoFlags::NONE, gio::Cancellable::NONE)
        .expect("file info");
    test_repo
        .repo
        .checkout_tree(
            ostree::RepoCheckoutMode::User,
            ostree::RepoCheckoutOverwriteMode::None,
            &gio::File::for_path(checkout_dir.path().join("test-checkout")),
            &file,
            &info,
            gio::Cancellable::NONE,
        )
        .expect("checkout tree");

    assert_test_file(checkout_dir.path());
}

#[test]
fn should_write_content_to_repo() {
    let src = TestRepo::new();
    let mtree = create_mtree(&src.repo);
    let checksum = commit(&src.repo, &mtree, "test");

    let dest = TestRepo::new();
    let objects = src
        .repo
        .traverse_commit(&checksum, -1, gio::Cancellable::NONE)
        .expect("traverse");
    for obj in objects {
        match obj.object_type() {
            ObjectType::File => copy_file(&src, &dest, &obj),
            _ => copy_metadata(&src, &dest, &obj),
        }
    }
}

#[test]
#[cfg(any(feature = "v2016_4", feature = "dox"))]
fn repo_file() {
    use std::os::unix::fs::MetadataExt;
    let test_repo = TestRepo::new();
    let m1 = test_repo.repo.dfd_as_file().unwrap().metadata().unwrap();
    let m2 = test_repo.repo.dfd_as_file().unwrap().metadata().unwrap();
    assert_eq!(m1.dev(), m2.dev());
    assert_eq!(m1.ino(), m2.ino());
}

fn copy_file(src: &TestRepo, dest: &TestRepo, obj: &ObjectName) {
    let (stream, len) = src
        .repo
        .load_object_stream(obj.object_type(), obj.checksum(), gio::Cancellable::NONE)
        .expect("load object stream");
    let out_csum = dest
        .repo
        .write_content(None, &stream, len, gio::Cancellable::NONE)
        .expect("write content");
    assert_eq!(out_csum.to_string(), obj.checksum());
}

fn copy_metadata(src: &TestRepo, dest: &TestRepo, obj: &ObjectName) {
    let data = src
        .repo
        .load_variant(obj.object_type(), obj.checksum())
        .expect("load variant");
    let out_csum = dest
        .repo
        .write_metadata(obj.object_type(), None, &data, gio::Cancellable::NONE)
        .expect("write metadata");
    assert_eq!(out_csum.to_string(), obj.checksum());
}
