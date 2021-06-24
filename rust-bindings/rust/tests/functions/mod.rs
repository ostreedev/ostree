use crate::util::TestRepo;
use gio::NONE_CANCELLABLE;
use ostree::{checksum_file_from_input, ObjectType};

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
        assert_eq!(result.to_string(), obj.checksum());
    }
}
