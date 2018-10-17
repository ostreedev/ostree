#![allow(dead_code)]

#[cfg(feature = "lgpl-docs")]
extern crate libgir;

#[cfg(feature = "lgpl-docs")]
extern crate stripper_lib;

fn main() {
    #[cfg(feature = "lgpl-docs")] {
        extract_api_docs().expect("failed to extract API docs");
        merge_api_docs();
    }
}

fn out_dir() -> String {
    std::env::var("OUT_DIR").expect("missing var OUT_DIR")
}

fn docs_file() -> String {
    format!("{}/vendor.md", out_dir())
}

#[cfg(feature = "lgpl-docs")]
fn extract_api_docs() -> Result<(), String> {
    let mut config = libgir::Config::new(
        Some("../conf/libostree.toml"),
        libgir::WorkMode::Doc,
        None,
        None,
        None,
        None,
        Some(&docs_file()),
        false,
        false,
    )?;

    let mut library = libgir::Library::new(&config.library_name);
    library.read_file(&config.girs_dir, &config.library_full_name())?;
    library.preprocessing(config.work_mode);
    libgir::update_version::apply_config(&mut library, &config);
    library.postprocessing();
    config.resolve_type_ids(&library);
    libgir::update_version::check_function_real_version(&mut library);

    let namespaces = libgir::namespaces_run(&library);
    let symbols = libgir::symbols_run(&library, &namespaces);
    let class_hierarchy = libgir::class_hierarchy_run(&library);

    let mut env = libgir::Env {
        library,
        config,
        namespaces,
        symbols: std::cell::RefCell::new(symbols),
        class_hierarchy,
        analysis: Default::default(),
    };

    libgir::analysis_run(&mut env);
    libgir::codegen_generate(&env);

    Ok(())
}

#[cfg(feature = "lgpl-docs")]
fn merge_api_docs() {
    stripper_lib::regenerate_doc_comments(".", false, &docs_file(), false, false);
}
