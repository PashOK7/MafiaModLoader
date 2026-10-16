// MafiaModLoader: the reference shaders. They draw every kind of the game's draw as its fixed pipeline does (the
// same picture), from the device states the host hands over (game.hlsli): copy them and change what you want.
//
//   shader_add: source = this file, vs_entry "mml_vs", ps_entry "mml_ps" (vs_3_0, ps_3_0); or `call shader.add
//   {"source": <this file>, "vs": "mml_vs", "ps": "mml_ps"}`.
//
// The vertex stream the game draws: position, the colour LS3D lit it with, one or two uv sets.
#include "game.hlsli"

struct MmlVsIn {
    float3 pos : POSITION;
    float4 color : COLOR0;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};
struct MmlVsOut {
    float4 pos : POSITION;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;      // xy the first uv set, zw the second
    float4 world : TEXCOORD1;   // xyz the world position, w the fog's distance
};

MmlVsOut mml_vs(MmlVsIn i) {
    MmlVsOut o;
    // (the fixed pipeline's order: a decal lying on a wall loses to it or wins by the depth's last bits, and the game's
    // own signs on walls lose this way; mml_WorldViewProj in one step would show them)
    o.pos = mul(mul(mul(float4(i.pos, 1), mml_World), mml_View), mml_Proj);
    o.color = i.color;
    // (each stage's TEXCOORDINDEX picks the uv set)
    o.uv.xy = mml_Stage0.z > 0.5 ? i.uv1 : i.uv0;
    o.uv.zw = mml_Stage1.z > 0.5 ? i.uv1 : i.uv0;
    o.world.xyz = mul(float4(i.pos, 1), mml_World).xyz;
    o.world.w = mml_FogDistance(i.pos);
    return o;
}

float4 mml_ps(MmlVsOut i) : COLOR0 {
    float4 c = mml_FixedStage(mml_Stage0, tex2D(mml_Tex0, i.uv.xy), i.color, i.color);
    if (mml_Stage1.x > 1.5) c = mml_FixedStage(mml_Stage1, tex2D(mml_Tex1, i.uv.zw), c, i.color);
    c.rgb = lerp(mml_FogColor.rgb, c.rgb, mml_FogFactor(i.world.w));
    return c;
}
