use gio::NONE_CANCELLABLE;
use ostree::{checksum_file_at, ChecksumFlags, ObjectType, RepoMode};
use std::path::PathBuf;
use util::TestRepo;

#[test]
fn should_checksum_file_at() {
    let repo = TestRepo::new_with_mode(RepoMode::BareUser);
    repo.test_commit("test");

    let result = checksum_file_at(
        repo.repo.get_dfd(),
        &PathBuf::from(
            "objects/89/f84ca9854a80e85b583e46a115ba4985254437027bad34f0b113219323d3f8.file",
        ),
        None,
        ObjectType::File,
        ChecksumFlags::IGNORE_XATTRS,
        NONE_CANCELLABLE,
    )
    .expect("checksum file at");

    assert_eq!(
        result.as_str(),
        "89f84ca9854a80e85b583e46a115ba4985254437027bad34f0b113219323d3f8",
    );
}
