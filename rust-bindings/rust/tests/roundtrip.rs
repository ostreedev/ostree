extern crate gio;
extern crate glib;
extern crate ostree;
extern crate tempfile;

use glib::prelude::*;
use ostree::prelude::*;
use std::fs;
use std::io;
use std::io::Write;

fn create_repo(repodir: &tempfile::TempDir) -> Result<ostree::Repo, glib::Error> {
    let repo = ostree::Repo::new_for_path(repodir.path());
    repo.create(ostree::RepoMode::Archive, None)?;
    Ok(repo)
}

fn create_test_file(treedir: &tempfile::TempDir) -> Result<(), io::Error> {
    let mut testfile = fs::File::create(treedir.path().join("test.txt"))?;
    write!(testfile, "test")?;
    Ok(())
}

fn create_mtree(
    treedir: &tempfile::TempDir,
    repo: &ostree::Repo,
) -> Result<ostree::MutableTree, glib::Error> {
    let gfile = gio::File::new_for_path(treedir.path());
    let mtree = ostree::MutableTree::new();
    repo.write_directory_to_mtree(&gfile, &mtree, None, None)?;
    Ok(mtree)
}

fn commit_mtree(
    repo: &ostree::Repo,
    mtree: &ostree::MutableTree,
) -> Result<String, glib::Error> {
    repo.prepare_transaction(None)?;
    let repo_file = repo.write_mtree(mtree, None)?.downcast().unwrap();
    let checksum = repo.write_commit(None, "Test Commit", None, None, &repo_file, None)?;
    repo.transaction_set_ref(None, "test", checksum.as_str());
    repo.commit_transaction(None)?;
    Ok(checksum)
}

fn open_repo(repodir: &tempfile::TempDir) -> Result<ostree::Repo, glib::Error> {
    let repo = ostree::Repo::new_for_path(repodir.path());
    repo.open(None)?;
    Ok(repo)
}

#[test]
fn should_commit_content_to_repo_and_list_refs_again() {
    let repodir = tempfile::tempdir().unwrap();
    let treedir = tempfile::tempdir().unwrap();

    let repo = create_repo(&repodir).expect("failed to create repo");
    create_test_file(&treedir).expect("failed to create test file");
    let mtree = create_mtree(&treedir, &repo).expect("failed to build mtree");
    let checksum = commit_mtree(&repo, &mtree).expect("failed to commit mtree");

    let repo = open_repo(&repodir).expect("failed to open repo");
    let refs = repo.list_refs(None, None).expect("failed to list refs");
    assert_eq!(refs.len(), 1);
    assert_eq!(refs["test"], checksum);
}
