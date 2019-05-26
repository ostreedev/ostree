use gio::NONE_CANCELLABLE;
use glib::prelude::*;
use glib::GString;
use std::path::Path;

#[derive(Debug)]
pub struct TestRepo {
    pub dir: tempfile::TempDir,
    pub repo: ostree::Repo,
}

impl TestRepo {
    pub fn new() -> TestRepo {
        TestRepo::new_with_mode(ostree::RepoMode::BareUser)
    }

    pub fn new_with_mode(repo_mode: ostree::RepoMode) -> TestRepo {
        let dir = tempfile::tempdir().expect("temp repo dir");
        let repo = ostree::Repo::new_for_path(dir.path());
        repo.create(repo_mode, NONE_CANCELLABLE)
            .expect("OSTree repo");
        TestRepo { dir, repo }
    }

    pub fn test_commit(&self, ref_: &str) -> GString {
        let mtree = create_mtree(&self.repo);
        commit(&self.repo, &mtree, ref_)
    }
}

pub fn create_mtree(repo: &ostree::Repo) -> ostree::MutableTree {
    let mtree = ostree::MutableTree::new();
    let file = gio::File::new_for_path(
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("tests")
            .join("data")
            .join("test.tar"),
    );
    repo.write_archive_to_mtree(&file, &mtree, None, true, NONE_CANCELLABLE)
        .expect("test mtree");
    mtree
}

pub fn commit(repo: &ostree::Repo, mtree: &ostree::MutableTree, ref_: &str) -> GString {
    repo.prepare_transaction(NONE_CANCELLABLE)
        .expect("prepare transaction");
    let repo_file = repo
        .write_mtree(mtree, NONE_CANCELLABLE)
        .expect("write mtree")
        .downcast::<ostree::RepoFile>()
        .unwrap();
    let checksum = repo
        .write_commit(
            None,
            "Test Commit".into(),
            None,
            None,
            &repo_file,
            NONE_CANCELLABLE,
        )
        .expect("write commit");
    repo.transaction_set_ref(None, ref_, checksum.as_str().into());
    repo.commit_transaction(NONE_CANCELLABLE)
        .expect("commit transaction");
    checksum
}

pub fn assert_test_file(checkout: &Path) {
    let testfile_path = checkout
        .join("test-checkout")
        .join("testdir")
        .join("testfile");
    let testfile_contents = std::fs::read_to_string(testfile_path).expect("test file");
    assert_eq!("test\n", testfile_contents);
}
