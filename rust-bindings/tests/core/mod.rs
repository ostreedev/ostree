use crate::util::*;
use std::error::Error;

#[test]
fn variant_types() -> Result<(), Box<dyn Error>> {
    let tr = TestRepo::new();
    let commit_checksum = tr.test_commit("test");
    let repo = &tr.repo;
    let commit_v = repo.load_variant(ostree::ObjectType::Commit, commit_checksum.as_str())?;
    let commit = commit_v.get::<ostree::CommitVariantType>().unwrap();
    assert_eq!(commit.3, "Test Commit");
    Ok(())
}
