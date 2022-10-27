use crate::util::*;
use ostree::glib::prelude::*;
use ostree::glib::Variant;
use ostree::*;

use std::collections::HashMap;

#[test]
fn should_generate_static_delta_at() {
    let mut options: HashMap<String, Variant> = HashMap::<String, Variant>::new();

    let delta_dir = tempfile::tempdir().expect("static delta dir");
    let delta_path = delta_dir.path().join("static_delta.file");
    let path_var = delta_path
        .to_str()
        .map(std::ffi::CString::new)
        .expect("no valid path")
        .unwrap()
        .as_bytes_with_nul()
        .to_variant();

    let test_repo = TestRepo::new();
    let from = test_repo.test_commit("commit1");
    let to = test_repo.test_commit("commit2");

    options.insert(String::from("filename"), path_var);

    let varopts = &options.to_variant();

    let _result = test_repo
        .repo
        .static_delta_generate(
            ostree::StaticDeltaGenerateOpt::Major,
            Some(&from),
            &to,
            None,
            Some(varopts),
            gio::Cancellable::NONE,
        )
        .expect("static delta generate");

    assert!(delta_path.try_exists().unwrap());
}
