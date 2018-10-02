extern crate gio;
extern crate libostree;

use std::io::Write;
use std::fs::File;

use gio::prelude::*;
use libostree::prelude::*;

fn main() {
    let repo = libostree::Repo::new_for_str("../../../repo-bare");

    //let result = repo.create(libostree::RepoMode::Archive, Option::None);
    //result.expect("we did not expect this to fail :O");

    repo.open(None).expect("should have opened");

    let (file, checksum) = repo.read_commit("test", None).unwrap();

    println!("path: {:?}", file.get_path());

    println!("sha256: {}", checksum);

    let objs = repo.traverse_commit_manual(checksum.as_str(), -1, None).unwrap();

    for obj in objs {
        let (name, obj_type) = libostree::object_name_deserialize(&obj);
        println!("  {}", libostree::object_to_string(name.as_str(), obj_type).unwrap());

        let (stream, size) = repo.load_object_stream(obj_type, name.as_str(), None).unwrap();
        println!("  bytes: {}", size);

        if obj_type == libostree::ObjectType::File {
            let mut file = File::create("./object.file").unwrap();
            let mut read = 1;
            let mut buffer = [0 as u8; 4096];
            while read != 0 {
                read = stream.read(buffer.as_mut(), None).unwrap();
                file.write(&buffer[0 .. read]);
            }
        }

        //println!("{:?}", obj.type_());
    }
}
