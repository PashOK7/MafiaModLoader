//! rust_sys: sdk/rust/mml/src/sys.rs, the Rust form of the C ABI, made from sdk/include/mml/mml.h by bindgen.
//!
//!     cargo run --release --manifest-path tools/rust_sys/Cargo.toml           write sys.rs
//!     cargo run --release --manifest-path tools/rust_sys/Cargo.toml -- --check   exit 1 when sys.rs is not what
//!                                                                                mml.h gives (tests/run_tests.py)
//!
//! sys.rs is committed: a mod's author needs no libclang. It is made for the loader's one target,
//! i686-pc-windows-msvc (the game is 32-bit), and its layout checks are compile-time asserts for that target.
//! The named enums become newtypes over their C type (an unknown value from a newer loader is no undefined
//! behaviour), their variants without the prefix (MmlEventType::FRAME); the anonymous ones (MML_BONE_*,
//! MML_SLOT_*...) plain `c_int` constants.
use std::path::PathBuf;
use std::process::ExitCode;

#[derive(Debug)]
struct Names;

impl bindgen::callbacks::ParseCallbacks for Names {
    fn enum_variant_name(
        &self,
        enum_name: Option<&str>,
        original: &str,
        _value: bindgen::callbacks::EnumVariantValue,
    ) -> Option<String> {
        let name = enum_name?.trim_start_matches("enum ");
        let prefix = match name {
            "MmlEventType" => "MML_EVENT_",
            "MmlLogLevel" => "MML_LOG_",
            "MmlFont" => "MML_FONT_",
            "MmlMissionState" => "MML_MISSION_",
            _ => return None,
        };
        original.strip_prefix(prefix).map(str::to_owned)
    }
}

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..").join("..")
}

fn generate() -> String {
    let include = root().join("sdk").join("include");
    let header = include.join("mml").join("mml.h");
    let text = bindgen::Builder::default()
        .header(header.to_string_lossy())
        .clang_args(["--target=i686-pc-windows-msvc", "-x", "c"])
        .clang_arg(format!("-I{}", include.display()))
        .allowlist_type("Mml.*")
        .allowlist_var("MML_.*")
        .newtype_enum("Mml.*")
        .prepend_enum_name(false)
        .ctypes_prefix("::core::ffi")
        .use_core()
        .generate_comments(false) // mml.h's comments follow their members: clang would give them to the next one
        .derive_default(true)
        .derive_debug(true)
        .layout_tests(true)
        .parse_callbacks(Box::new(Names))
        .generate()
        .expect("bindgen could not read mml.h")
        .to_string();
    tidy(&text)
}

/// The anonymous enums' constants typed `c_int` instead of bindgen's `_bindgen_ty_N` aliases.
fn tidy(text: &str) -> String {
    let mut aliases = Vec::new();
    let mut out = String::new();
    for line in text.lines() {
        let t = line.trim();
        if let Some(rest) = t.strip_prefix("pub type _bindgen_ty_") {
            if let Some((n, ty)) = rest.split_once(" = ") {
                aliases.push((format!("_bindgen_ty_{n}"), ty.trim_end_matches(';').to_owned()));
                continue;
            }
        }
        out.push_str(line);
        out.push('\n');
    }
    // the longest first: _bindgen_ty_12 before _bindgen_ty_1
    aliases.sort_by(|a, b| b.0.len().cmp(&a.0.len()));
    for (alias, ty) in &aliases {
        out = out.replace(alias.as_str(), ty);
    }
    let head = "// Made by tools/rust_sys from sdk/include/mml/mml.h: do not edit (run it again after a change there).\n\
                // The contract, every member's meaning: mml.h. The safe layer over it: lib.rs.\n";
    format!("{head}{out}")
}

fn main() -> ExitCode {
    let out = root().join("sdk").join("rust").join("mml").join("src").join("sys.rs");
    let text = generate();
    if std::env::args().any(|a| a == "--check") {
        let now = std::fs::read_to_string(&out).unwrap_or_default().replace("\r\n", "\n");
        if now != text {
            eprintln!("{} is not what mml.h gives: run cargo run --release --manifest-path tools/rust_sys/Cargo.toml", out.display());
            return ExitCode::FAILURE;
        }
        println!("sys.rs: up to date");
        return ExitCode::SUCCESS;
    }
    std::fs::write(&out, text).expect("sys.rs not written");
    println!("written {}", out.display());
    ExitCode::SUCCESS
}
