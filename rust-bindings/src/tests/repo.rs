use crate::Repo;
use crate::RepoMode;

#[test]
fn should_get_repo_mode_from_string() {
    let mode = Repo::mode_from_string("archive").unwrap();
    assert_eq!(RepoMode::Archive, mode);
}

#[test]
fn should_return_error_for_invalid_repo_mode_string() {
    let result = Repo::mode_from_string("invalid-repo-mode");
    assert!(result.is_err());
}
