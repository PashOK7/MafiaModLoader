// MafiaModLoader: the constants the host gives a mod's shaders on the game's own drawing (mml.h shader_add;
// docs/MODDING.md "Shaders on the game's own drawing"). Direct3D 9, shader model 3 (vs_3_0 with ps_3_0); the same
// registers in the vertex and the pixel shader, set before each draw a rule takes. `#include "mml/game.hlsli"` in a
// shader the host compiles (shader_add's source) is this file; sdk/include/mml/shaders/reference.hlsl draws as the
// game does.
//
// What the game hands its draws (measured with shader.survey, both builds): LS3D lights the vertices itself, on the
// CPU, so a draw's vertices carry their lit colour and no normal (FVF 0x142: position, colour, uv; 0x242 also a
// second uv: a lightmap's or a car's reflection's); Direct3D's lighting is off and no light is set. The texture
// stages do the rest (mml_FixedStage below), then the vertex fog. A pixel shader that wants a normal takes the
// face's from the world position's derivatives (mml_FaceNormal).
//
// Matrices are Direct3D's (row vectors: a point times the matrix), as the game set them for this draw.
#ifndef MML_GAME_HLSLI
#define MML_GAME_HLSLI

row_major float4x4 mml_World     : register(c0);    // the object's space to the world
row_major float4x4 mml_View      : register(c4);
row_major float4x4 mml_Proj      : register(c8);
row_major float4x4 mml_WorldViewProj : register(c12);
float4 mml_Camera   : register(c16);   // xyz the eye in the world, w the time in seconds (wraps at an hour)
float4 mml_Fog      : register(c17);   // start, end, density, mode (0 none, 1 exp, 2 exp2, 3 linear)
float4 mml_FogColor : register(c18);   // rgb, w 1 when the game's fog is on for this draw
float4 mml_MatDiffuse  : register(c19);   // the material the game set (D3DMATERIAL: unused while its lighting is off)
float4 mml_MatAmbient  : register(c20);
float4 mml_MatEmissive : register(c21);
float4 mml_Ambient     : register(c22);   // the scene's ambient (D3DRS_AMBIENT)
float4 mml_Draw     : register(c23);   // x the kind (1 an object, 2 lightmapped, 3 a billboard), y 1 when Direct3D lights it,
                                       // z the alpha test's reference 0..1 (-1: no test; the device still tests after the
                                       // pixel shader), w the pass in its face group
float4 mml_Stage0   : register(c24);   // the texture stages: x COLOROP, y ALPHAOP, z TEXCOORDINDEX, w 1 when a texture is set
float4 mml_Stage1   : register(c25);   // (COLOROP 1 the stage is off)
float4 mml_Info     : register(c26);   // x the Direct3D lights on, y 1 for range fog (else the view's depth), zw the viewport
struct MmlLight {   // Direct3D's lights (D3DLIGHT9); none in the game's own draws
    float4 pos;       // xyz the place (world), w the type (1 point, 2 spot, 3 directional)
    float4 dir;       // xyz the direction (world), w the range
    float4 diffuse;
    float4 ambient;
    float4 att;       // attenuation 0, 1, 2; w the spot's falloff
    float4 cone;      // x cos(theta / 2), y cos(phi / 2)
};
MmlLight mml_Lights[8] : register(c27);
float4 mml_TFactor  : register(c75);   // D3DRS_TEXTUREFACTOR (a car's reflection's amount is its alpha)

// What per-object shading needs beyond the draw (mml.h shader_shadows): the shadow map from the sun (when a mod asked
// for it), the sun, the game's own lights near this draw.
row_major float4x4 mml_ShadowViewProj : register(c76);   // the world to the map's clip space (x, y -1..1, z 0..1 its depth)
float4 mml_ShadowInfo : register(c80);   // x 1 when the map was drawn this frame, y one texel (1 / size), z the depth's bias, w size
float4 mml_SunDir     : register(c81);   // xyz towards the sun (the scene's brightest directional light), w 1 when there is one
float4 mml_SunColor   : register(c82);   // rgb its colour times its power
float4 mml_SunAmbient : register(c83);   // rgb the scene's ambient light
float4 mml_GameLightInfo : register(c84);   // x the lights below (0..8), y the scene's lights that are on
struct MmlGameLight {   // LS3D's point and spot lights (lights.cpp), the nearest to the draw's object first
    float4 pos;       // xyz the place (world), w the type (1 point, 2 spot)
    float4 color;     // rgb its colour times its power, w its far range (nothing past it)
    float4 dir;       // xyz where a spot shines, w cos(its outer cone / 2)
    float4 range;     // x its near range (full light to it), y cos(its inner cone / 2)
};
MmlGameLight mml_GameLights[8] : register(c85);

sampler2D mml_Tex0 : register(s0);   // the textures the game set: stage 0 the surface's, stage 1 a lightmap or a reflection
sampler2D mml_Tex1 : register(s1);
sampler2D mml_ShadowMap : register(s2);   // R32F: the sun's depth (1 nothing); point sampling, white past its edges

// How lit a point of the world is by the sun: 1 lit, 0 in a shadow (2x2 texels, a 3x3 average). 1 when there is no
// map this frame.
float mml_Shadow(float3 worldPos) {
    if (mml_ShadowInfo.x < 0.5) return 1;
    float4 p = mul(float4(worldPos, 1), mml_ShadowViewProj);
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    float depth = p.z - mml_ShadowInfo.z;
    float lit = 0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            lit += tex2D(mml_ShadowMap, uv + float2(x, y) * mml_ShadowInfo.y).r >= depth ? 1 : 0;
    return lit / 9;
}

// One of the game's lights at a point: its colour times how much reaches it (LS3D's: full to the near range, none past
// the far one, linear between; a spot's cone between its inner and outer angles), times the surface's facing it.
float3 mml_GameLight(int i, float3 worldPos, float3 worldNormal) {
    MmlGameLight l = mml_GameLights[i];
    float3 to = l.pos.xyz - worldPos;
    float dist = length(to);
    float3 L = to / max(dist, 1e-4);
    float att = saturate((l.color.w - dist) / max(l.color.w - l.range.x, 1e-4));
    if (l.pos.w > 1.5) {
        float rho = dot(-L, normalize(l.dir.xyz));
        att *= saturate((rho - l.dir.w) / max(l.range.y - l.dir.w, 1e-4));
    }
    return l.color.rgb * att * saturate(dot(normalize(worldNormal), L));
}

// The game's fog factor (1: no fog) for a distance from the eye, as the fixed pipeline works it out.
float mml_FogFactor(float dist) {
    if (mml_FogColor.w < 0.5 || mml_Fog.w < 0.5) return 1;
    if (mml_Fog.w > 2.5) return saturate((mml_Fog.y - dist) / max(mml_Fog.y - mml_Fog.x, 1e-4));
    float d = dist * mml_Fog.z;
    return saturate(mml_Fog.w > 1.5 ? exp(-d * d) : exp(-d));
}
// The distance the fixed pipeline's vertex fog takes for a point of the object's space: the view's depth, or the
// distance to the eye with range fog.
float mml_FogDistance(float3 objectPos) {
    float4 v = mul(float4(objectPos, 1), mul(mml_World, mml_View));
    return mml_Info.y > 0.5 ? length(v.xyz) : v.z;
}

// One fixed texture stage as Direct3D does it with the arguments the game uses (ARG1 the texture, ARG2 what came
// before): op is the stage's COLOROP (or ALPHAOP), the stage's texture sample, what came before (the vertex colour at
// stage 0), the alphas BLEND*ALPHA read.
float4 mml_FixedOp(float op, float4 tex, float4 cur, float4 diffuse) {
    if (op < 1.5) return cur;                          // disable
    if (op < 2.5) return tex;                          // selectarg1
    if (op < 3.5) return cur;                          // selectarg2
    if (op < 4.5) return tex * cur;                    // modulate
    if (op < 5.5) return saturate(tex * cur * 2);      // modulate2x
    if (op < 6.5) return saturate(tex * cur * 4);      // modulate4x
    if (op < 7.5) return saturate(tex + cur);          // add
    if (op < 8.5) return saturate(tex + cur - 0.5);    // addsigned
    if (op < 9.5) return saturate((tex + cur - 0.5) * 2);   // addsigned2x
    if (op < 10.5) return saturate(tex - cur);         // subtract
    if (op < 11.5) return saturate(tex + cur - tex * cur);  // addsmooth
    if (op < 12.5) return lerp(cur, tex, diffuse.a);   // blenddiffusealpha
    if (op < 13.5) return lerp(cur, tex, tex.a);       // blendtexturealpha
    if (op < 14.5) return lerp(cur, tex, mml_TFactor.a);    // blendfactoralpha
    return tex * cur;
}
float4 mml_FixedStage(float4 stage, float4 tex, float4 cur, float4 diffuse) {
    float4 t = stage.w > 0.5 ? tex : float4(1, 1, 1, 1);   // (a stage with no texture: white)
    float3 rgb = mml_FixedOp(stage.x, t, cur, diffuse).rgb;
    float a = mml_FixedOp(stage.y, t, cur, diffuse).a;
    return float4(rgb, a);
}

// The face's normal in the world from a pixel's world position (flat: the triangle's), towards the eye's side
// (measured: the street's is y up; the other order of the cross product gave y down).
float3 mml_FaceNormal(float3 worldPos) {
    return normalize(cross(ddx(worldPos), ddy(worldPos)));
}

// Direct3D's fixed lighting of a point (for draws Direct3D lights: mml_Draw.y 1; the game's own draws are lit by LS3D).
float4 mml_Light(float3 worldPos, float3 worldNormal) {
    float3 c = mml_MatEmissive.rgb + mml_Ambient.rgb * mml_MatAmbient.rgb;
    float3 n = normalize(worldNormal);
    for (int i = 0; i < 8; i++) {
        if (i >= (int)mml_Info.x) break;
        MmlLight l = mml_Lights[i];
        float3 L;
        float att = 1;
        if (l.pos.w > 2.5) {
            L = -normalize(l.dir.xyz);
        } else {
            float3 to = l.pos.xyz - worldPos;
            float dist = length(to);
            L = to / max(dist, 1e-4);
            att = dist > l.dir.w ? 0 : 1 / max(l.att.x + l.att.y * dist + l.att.z * dist * dist, 1e-4);
            if (l.pos.w > 1.5) {
                float rho = dot(-L, normalize(l.dir.xyz));
                att *= rho > l.cone.x ? 1 : rho <= l.cone.y ? 0 : pow(saturate((rho - l.cone.y) / max(l.cone.x - l.cone.y, 1e-4)), l.att.w);
            }
        }
        c += att * (l.ambient.rgb * mml_MatAmbient.rgb + saturate(dot(n, L)) * l.diffuse.rgb * mml_MatDiffuse.rgb);
    }
    return float4(saturate(c), mml_MatDiffuse.a);
}

#endif
