extern crate gio;
extern crate libostree;

use gio::prelude::*;
use libostree::prelude::*;

fn main() {
    let repo = libostree::Repo::new(&gio::File::new_for_path("test-repo"));

    let result = repo.create(libostree::RepoMode::Archive, Option::None);

    result.expect("we did not expect this to fail :O");

    let path = repo.get_path();
    println!("path: {}", path.unwrap().get_path().unwrap().to_str().unwrap());
}
