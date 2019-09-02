use gio::NONE_CANCELLABLE;
use glib::prelude::*;
use ostree::prelude::*;
use ostree::{checksum_file, checksum_file_from_input, ObjectType, RepoFile};
use util::TestRepo;

#[cfg(feature = "v2017_13")]
mod checksum_file_at;

#[test]
fn should_checksum_file() {
    let repo = TestRepo::new();
    repo.test_commit("test");

    let file = repo
        .repo
        .read_commit("test", NONE_CANCELLABLE)
        .expect("read commit")
        .0
        .downcast::<RepoFile>()
        .expect("downcast");
    let result = checksum_file(&file, ObjectType::File, NONE_CANCELLABLE).expect("checksum file");

    assert_eq!(file.get_checksum().unwrap(), result.to_string());
}

#[test]
fn should_checksum_file_from_input() {
    let repo = TestRepo::new();
    let commit_checksum = repo.test_commit("test");

    let objects = repo
        .repo
        .traverse_commit(&commit_checksum, -1, NONE_CANCELLABLE)
        .expect("traverse commit");
    for obj in objects {
        if obj.object_type() != ObjectType::File {
            continue;
        }
        let (stream, file_info, xattrs) = repo
            .repo
            .load_file(obj.checksum(), NONE_CANCELLABLE)
            .expect("load file");
        let result = checksum_file_from_input(
            file_info.as_ref().unwrap(),
            xattrs.as_ref(),
            stream.as_ref(),
            ObjectType::File,
            NONE_CANCELLABLE,
        )
        .expect("checksum file from input");
        assert_eq!(obj.checksum(), result.to_string());
    }
}
