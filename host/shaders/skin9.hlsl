// People skinned on the GPU (host/skin.cpp, host/shaders.cpp), shader model 3; the contract is
// sdk/include/mml/shaders/skin.hlsli (a mod's own vertex shader reads the same):
//   VsSkin        vs_3_0  a person's draw: skinned, lit as LS3D lit it (the reference: the same picture); its outputs
//                         reference.hlsl's MmlVsOut and the world's normal and tangent (a rule's pixel shader reads them)
//   PsSkin        ps_3_0  the game's fixed stages and fog (reference.hlsl's mml_ps), for a draw no rule takes
//   VsSkinDepth   vs_3_0  the drawing from the sun (the shadow map): the shadow pass's own pixel shader after it
//   VsSkinMotion  vs_3_0  the motion vectors (host/motion.cpp's PsMotion after it): each vertex through this frame's
//                         palette and transforms (VsSkin's, as it drew) and the last frame's palette and transform
#include "mml/shaders/skin.hlsli"
#include "mml/shaders/reference.hlsl"

struct SkinOut {
    float4 pos : POSITION;
    float4 color : COLOR0;
    float4 uv : TEXCOORD0;      // xy the first uv set, zw the second (the environment map's when there is one)
    float4 world : TEXCOORD1;   // xyz the world's place, w the fog's distance
    float3 normal : TEXCOORD2;  // the world's
    float4 tangent : TEXCOORD3; // the world's, w the bitangent's sign
};

SkinOut VsSkin(MmlSkinIn i) {
    MmlSkinned s = mml_Skin(i);
    SkinOut o;
    float4 w = mul(float4(s.pos, 1), mml_World);
    o.pos = mul(mul(w, mml_View), mml_Proj);
    o.color = mml_SkinLight(s.pos, s.normal);
    o.normal = mul(s.normal, (float3x3)mml_World);
    o.tangent = float4(mul(s.tangent, (float3x3)mml_World), i.tangent.w);
    float2 second = mml_SkinInfo.y > -0.5 ? mml_SkinEnvUv(w.xyz, o.normal) : i.uv;
    o.uv.xy = mml_Stage0.z > 0.5 ? second : i.uv;
    o.uv.zw = mml_Stage1.z > 0.5 ? second : i.uv;
    o.world.xyz = w.xyz;
    o.world.w = mml_FogDistance(s.pos);
    return o;
}

float4 PsSkin(MmlVsOut i) : COLOR0 { return mml_ps(i); }

// the shadow map's drawing (host/shaders.cpp kShadowHlsl's vs: the same outputs)
struct DepthOut { float4 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float z : TEXCOORD1; };
DepthOut VsSkinDepth(MmlSkinIn i) {
    MmlSkinned s = mml_Skin(i);
    DepthOut o;
    o.pos = mul(mul(mul(float4(s.pos, 1), mml_World), mml_View), mml_Proj);
    o.color = float4(1, 1, 1, mml_SkinInfo.x);
    o.uv = i.uv;
    o.z = o.pos.z / o.pos.w;
    return o;
}

// the motion vectors: the last frame's palette at vertex sampler 1, the last frame's world * view * projection at c164
sampler2D g_paletteBefore : register(s1);
row_major float4x4 g_before : register(c164);
struct MotionOut { float4 pos : POSITION; float4 now : TEXCOORD0; float4 before : TEXCOORD1; };
float3 SkinBefore(MmlSkinIn i) {
    float b[8] = {i.bones0.x, i.bones0.y, i.bones0.z, i.bones0.w, i.bones1.x, i.bones1.y, i.bones1.z, i.bones1.w};
    float w[8] = {i.weights0.x, i.weights0.y, i.weights0.z, i.weights0.w, i.weights1.x, i.weights1.y, i.weights1.z, i.weights1.w};
    float4 p = float4(i.pos, 1);
    float3 o = 0;
    [unroll] for (int k = 0; k < 8; k++) {
        if (w[k] > 0) {
            float t = b[k] * 6;
            float4 c0 = tex2Dlod(g_paletteBefore, float4((t + 0.5) * mml_SkinInfo.w, mml_SkinEnv.w, 0, 0));
            float4 c1 = tex2Dlod(g_paletteBefore, float4((t + 1.5) * mml_SkinInfo.w, mml_SkinEnv.w, 0, 0));
            float4 c2 = tex2Dlod(g_paletteBefore, float4((t + 2.5) * mml_SkinInfo.w, mml_SkinEnv.w, 0, 0));
            o += w[k] * float3(dot(p, c0), dot(p, c1), dot(p, c2));
        }
    }
    return o;
}
MotionOut VsSkinMotion(MmlSkinIn i) {
    MotionOut o;
    // the place as VsSkin put it (the same transforms, c0..c11, left by the draw just made, in the same order): the
    // depth test against that draw's own depth (motion.cpp's LESSEQUAL) passes on every pixel it drew; through
    // g_now, one matrix, the last bits differed and parts of a swinging arm kept the camera's motion
    o.pos = mul(mul(mul(float4(mml_Skin(i).pos, 1), mml_World), mml_View), mml_Proj);
    o.now = o.pos;
    o.before = mul(float4(SkinBefore(i), 1), g_before);
    return o;
}
