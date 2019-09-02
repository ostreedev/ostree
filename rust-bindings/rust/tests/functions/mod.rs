use gio::NONE_CANCELLABLE;
use glib::Cast;
use ostree::{checksum_file, ObjectType, RepoFile, RepoFileExt};
use util::TestRepo;

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
