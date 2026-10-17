//! A Rust mod's shaders compiled at build time, as the C++ mods' `mml_shaders` does: the Windows SDK's fxc (shader
//! model 5 and lower: Direct3D 9 to 11) or dxc (6 and up), optimised, warnings as errors, the SDK's `sdk/include`
//! on the include path (`#include "mml/gfx/frame.hlsli"`, `"mml/shaders/game.hlsli"`).
//!
//! In the mod's `build.rs` (with `[build-dependencies] mml-build = { path = ".../sdk/rust/mml-build" }`):
//!
//! ```ignore
//! fn main() {
//!     mml_build::hlsl("pulse.hlsl", "PsPulse", "ps_5_0");
//! }
//! ```
//!
//! and in the mod: `const PS_PULSE: &[u8] = mml::include_shader!("PsPulse");` (the bytecode, from OUT_DIR).
//! A change to the file, or to any .hlsli beside it or in the SDK, builds it again. The compiler: the environment's
//! `MML_FXC` / `MML_DXC` (the CMake build passes its own), else the newest Windows SDK's (its registry root, then
//! the usual folder), else the PATH.
use std::path::{Path, PathBuf};
use std::process::Command;

/// Compiles `entry` of `file` (relative to the crate) for `profile` ("ps_5_0", "vs_3_0", "cs_5_0", "ps_6_0"...)
/// into OUT_DIR/shaders/<entry>.cso. A compiler error stops the build with its message.
pub fn hlsl(file: &str, entry: &str, profile: &str) {
    hlsl_named(file, entry, profile, entry)
}

/// The same, written as `name` (two profiles of one entry point).
pub fn hlsl_named(file: &str, entry: &str, profile: &str, name: &str) {
    let crate_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("mml_build: run from a build.rs"));
    let src = crate_dir.join(file);
    let src_dir = src.parent().unwrap_or(&crate_dir).to_path_buf();
    let sdk_include = Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join("..").join("include");
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").expect("mml_build: no OUT_DIR")).join("shaders");
    std::fs::create_dir_all(&out_dir).expect("mml_build: OUT_DIR/shaders not made");
    let out = out_dir.join(format!("{name}.cso"));

    println!("cargo:rerun-if-changed={}", src.display());
    for dir in [src_dir.clone(), sdk_include.join("mml").join("gfx"), sdk_include.join("mml").join("shaders")] {
        for e in std::fs::read_dir(&dir).into_iter().flatten().flatten() {
            if e.path().extension().is_some_and(|x| x == "hlsli") {
                println!("cargo:rerun-if-changed={}", e.path().display());
            }
        }
    }
    println!("cargo:rerun-if-env-changed=MML_FXC");
    println!("cargo:rerun-if-env-changed=MML_DXC");

    let dxc = profile.contains("_6_");
    let tool = compiler(if dxc { "dxc" } else { "fxc" });
    let mut cmd = Command::new(&tool);
    if dxc {
        cmd.args(["-nologo", "-O3", "-WX", "-T", profile, "-E", entry, "-I"]).arg(&src_dir).arg("-I").arg(&sdk_include).arg("-Fo").arg(&out).arg(&src);
    } else {
        cmd.args(["/nologo", "/O3", "/WX", "/T", profile, "/E", entry, "/I"]).arg(&src_dir).arg("/I").arg(&sdk_include).arg("/Fo").arg(&out).arg(&src);
    }
    let r = cmd.output().unwrap_or_else(|e| panic!("mml_build: {} could not be run: {e}", tool.display()));
    if !r.status.success() {
        panic!(
            "mml_build: {file} {entry} ({profile}) did not compile:\n{}{}",
            String::from_utf8_lossy(&r.stdout),
            String::from_utf8_lossy(&r.stderr)
        );
    }
}

/// fxc or dxc: the environment's, the newest Windows SDK's, the PATH's
fn compiler(name: &str) -> PathBuf {
    let var = if name == "fxc" { "MML_FXC" } else { "MML_DXC" };
    if let Some(p) = std::env::var_os(var).filter(|p| !p.is_empty()) {
        return PathBuf::from(p);
    }
    let exe = format!("{name}.exe");
    for root in kits_roots() {
        let bin = root.join("bin");
        let mut versions: Vec<PathBuf> = std::fs::read_dir(&bin)
            .into_iter()
            .flatten()
            .flatten()
            .map(|e| e.path())
            .filter(|p| p.file_name().and_then(|n| n.to_str()).is_some_and(|n| n.starts_with("10.")))
            .collect();
        versions.sort_by_key(|p| version_key(p));
        for v in versions.iter().rev() {
            let p = v.join("x64").join(&exe);
            if p.exists() {
                return p;
            }
        }
    }
    PathBuf::from(exe)
}

fn kits_roots() -> Vec<PathBuf> {
    let mut roots = Vec::new();
    // the registry's root, as the C++ build reads it
    if let Ok(o) = Command::new("reg").args(["query", r"HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots", "/v", "KitsRoot10"]).output() {
        for line in String::from_utf8_lossy(&o.stdout).lines() {
            if let Some((_, path)) = line.split_once("REG_SZ") {
                roots.push(PathBuf::from(path.trim()));
            }
        }
    }
    roots.push(PathBuf::from(r"C:\Program Files (x86)\Windows Kits\10"));
    roots
}

fn version_key(p: &Path) -> Vec<u32> {
    p.file_name().and_then(|n| n.to_str()).unwrap_or("").split('.').map(|s| s.parse().unwrap_or(0)).collect()
}
