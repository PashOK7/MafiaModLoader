// Current posed LS3D materials written as depth; compiled with the host, never during a game frame.
#include "mml/shaders/game.hlsli"
struct ShadowIn { float3 pos : POSITION; float4 color : COLOR0; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };
struct ShadowOut { float4 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float z : TEXCOORD1; };
ShadowOut VsSunShadow(ShadowIn i) {
    ShadowOut o;
    o.pos = mul(mul(mul(float4(i.pos, 1), mml_World), mml_View), mml_Proj);
    o.color = i.color;
    o.uv = mml_Stage0.z > 0.5 ? i.uv1 : i.uv0;
    o.z = o.pos.z / o.pos.w;
    return o;
}
float4 PsSunShadow(ShadowOut i) : COLOR0 {
    float a = mml_FixedStage(mml_Stage0, tex2D(mml_Tex0, i.uv), i.color, i.color).a;
    float slope = max(abs(ddx(i.z)), abs(ddy(i.z)));
    return float4(i.z + slope * 1.5, 0, 0, a);
}
