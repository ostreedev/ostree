extern crate gio;
extern crate libostree;

use libostree::prelude::*;

fn main() {
    let repo = libostree::Repo::new_for_str("test-repo");

    let result = repo.create(libostree::RepoMode::Archive, Option::None);
    result.expect("we did not expect this to fail :O");
}
