// kit_rs's shader: compiled at build time by mml-build (build.rs), its bytecode checked by kit_rs.offline.
#include "mml/gfx/frame.hlsli"

float4 PsTiny(float4 pos : SV_Position) : SV_Target {
    return float4(SceneDepth(pos.xy).xxx * 0.01, 1);
}
