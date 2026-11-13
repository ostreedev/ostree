use std::env;
use std::path::PathBuf;

fn main() {
    let crate_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
    let out_dir = PathBuf::from(env::var("OUT_DIR").unwrap());

    // Generate C header with cbindgen
    let config = cbindgen::Config {
        language: cbindgen::Language::C,
        include_guard: Some("OSTREE_SEQUOIA_H".to_string()),
        ..Default::default()
    };

    cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(config)
        .generate()
        .expect("Unable to generate C bindings")
        .write_to_file(out_dir.join("ostree-sequoia.h"));

    // Also write to src dir for development convenience
    let _ = cbindgen::Builder::new()
        .with_crate(&crate_dir)
        .with_config(cbindgen::Config {
            language: cbindgen::Language::C,
            include_guard: Some("OSTREE_SEQUOIA_H".to_string()),
            ..Default::default()
        })
        .generate()
        .expect("Unable to generate C bindings")
        .write_to_file(PathBuf::from(&crate_dir).join("ostree-sequoia.h"));

    // Generate pkg-config file
    let prefix = env::var("PREFIX").unwrap_or_else(|_| "/usr/local".to_string());
    let libdir = env::var("LIBDIR").unwrap_or_else(|_| "${prefix}/lib".to_string());

    let pc_content = format!(
        r#"prefix={prefix}
libdir={libdir}
includedir=${{prefix}}/include

Name: ostree-sequoia
Description: OpenPGP backend for ostree using Sequoia PGP
Version: {version}
Libs: -L${{libdir}} -lostree_sequoia
Cflags: -I${{includedir}}
"#,
        prefix = prefix,
        libdir = libdir,
        version = env::var("CARGO_PKG_VERSION").unwrap(),
    );

    std::fs::write(
        PathBuf::from(env::var("CARGO_TARGET_DIR").unwrap_or_else(|_| "target".to_string()))
            .join(env::var("PROFILE").unwrap_or_else(|_| "debug".to_string()))
            .join("ostree-sequoia.pc"),
        pc_content,
    )
    .ok();
}
