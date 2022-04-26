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
        TestRepo::new_with_mode(ostree::RepoMode::Archive)
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

#[derive(Debug)]
#[cfg(feature = "cap-std-apis")]
pub struct CapTestRepo {
    pub dir: cap_tempfile::TempDir,
    pub repo: ostree::Repo,
}

#[cfg(feature = "cap-std-apis")]
impl CapTestRepo {
    pub fn new() -> Self {
        Self::new_with_mode(ostree::RepoMode::Archive)
    }

    pub fn new_with_mode(repo_mode: ostree::RepoMode) -> Self {
        let dir = cap_tempfile::tempdir(cap_std::ambient_authority()).unwrap();
        let repo = ostree::Repo::create_at_dir(&dir, ".", repo_mode, None).expect("repo create");
        Self { dir, repo }
    }
}

pub fn create_mtree(repo: &ostree::Repo) -> ostree::MutableTree {
    let mtree = ostree::MutableTree::new();
    assert_eq!(mtree.copy_files().len(), 0);
    assert_eq!(mtree.copy_subdirs().len(), 0);
    let file = gio::File::for_path(
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
    let txn = repo
        .auto_transaction(NONE_CANCELLABLE)
        .expect("prepare transaction");
    let repo_file = txn
        .repo()
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
    txn.commit(NONE_CANCELLABLE).expect("commit transaction");
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
