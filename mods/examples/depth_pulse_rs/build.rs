// The pass's pixel shader, compiled by the Windows SDK's fxc at build time (mml-build): src/lib.rs takes its bytecode
// with mml::include_shader!("PsPulse").
fn main() {
    mml_build::hlsl("pulse.hlsl", "PsPulse", "ps_5_0");
}
