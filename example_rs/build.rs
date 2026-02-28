use std::env;
use std::fs;
use std::path::PathBuf;

fn main() {
    let static_root = env::var("ZRTDB_STATIC_ROOT").unwrap_or_else(|_| "/usr/local/ZRTDB".to_string());
    let app = env::var("ZRTDB_APP").unwrap_or_else(|_| "CONTROLER".to_string());

    let src = PathBuf::from(&static_root)
        .join("header")
        .join("rust")
        .join(format!("{}.rs", app));

    println!("cargo:rerun-if-env-changed=ZRTDB_STATIC_ROOT");
    println!("cargo:rerun-if-env-changed=ZRTDB_APP");
    println!("cargo:rerun-if-changed={}", src.display());

    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR not set"))
        .join("zrtdb_generated.rs");

    println!("cargo:rustc-link-search=native={}", static_root);
    println!("cargo:rustc-link-lib=static=zrtdb");
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rustc-link-lib=dylib=pthread");

    if !src.exists() {
        panic!(
            "Rust bindings not found: {}. Run zrtdb_model first to generate DAT-based Rust interfaces.",
            src.display()
        );
    }

    fs::copy(&src, &out).expect("failed to copy generated Rust bindings");
}
