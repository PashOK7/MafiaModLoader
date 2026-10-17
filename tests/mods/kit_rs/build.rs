// kit_rs's shader through mml-build (the SDK's include path, fxc found as the CMake build or the Windows SDK has it)
fn main() {
    mml_build::hlsl("tiny.hlsl", "PsTiny", "ps_5_0");
}
