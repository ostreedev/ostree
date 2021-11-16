use crate::util::TestRepo;
use gio::NONE_CANCELLABLE;
use ostree::{checksum_file_from_input, ObjectType};

#[test]
fn list_repo_objects() {
    let repo = TestRepo::new();
    let commit_checksum = repo.test_commit("test");
    let mut dirtree_cnt = 0;
    let mut dirmeta_cnt = 0;
    let mut file_cnt = 0;
    let mut commit_cnt = 0;

    let objects = repo.repo.list_objects( ffi::OSTREE_REPO_LIST_OBJECTS_ALL, NONE_CANCELLABLE).expect("List Objects");
    for (object, _items) in objects {
        match object.object_type()  {
            ObjectType::DirTree => { dirtree_cnt += 1; },
            ObjectType::DirMeta => { dirmeta_cnt += 1; },
            ObjectType::File => { file_cnt += 1; },
            ObjectType::Commit => {
                assert_eq!(commit_checksum.to_string(), object.checksum());
                commit_cnt += 1;
            },
            x => { panic!("unexpected object type {}", x ); }
        }
    }
    assert_eq!(dirtree_cnt, 2);
    assert_eq!(dirmeta_cnt, 1);
    assert_eq!(file_cnt, 1);
    assert_eq!(commit_cnt, 1);
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
        assert_eq!(result.to_string(), obj.checksum());
    }
}
