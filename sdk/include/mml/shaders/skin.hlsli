// MafiaModLoader: people skinned on the GPU (mml.h skin_gpu; docs/MODDING.md "People's skinning", "on the GPU").
// Vertex shader side, shader model 3 (vs_3_0), with game.hlsli's constants (c0..c116, set as for any draw).
//
// While a mod asks for it (skin_gpu), every person's draw (LS3D's single meshes) goes through a vertex shader that
// skins the mesh's rest vertices with its bones' matrices and lights them as LS3D's CPU did (its own choice of the 8
// lights, its ambient, its 128 scale): the host's (the reference: the same picture), or a rule's whose flags have
// MML_SHADE_SKINNED (it reads these inputs; mml_Skin and mml_SkinLight do the work). A rule without that flag keeps
// its pixel shader on a person's draw, with the host's vertex shader: its outputs are reference.hlsl's MmlVsOut
// (TEXCOORD0, TEXCOORD1) and the world's normal (TEXCOORD2) and tangent (TEXCOORD3, w the bitangent's sign).
//
// The vertices come in two streams: the rest pose (the morph applied: a face's expression) and what does not move.
#ifndef MML_SKIN_HLSLI
#define MML_SKIN_HLSLI
#include "game.hlsli"

struct MmlSkinIn {
    float3 pos : POSITION0;          // the mesh's space at rest
    float3 normal : NORMAL0;
    float2 uv : TEXCOORD0;
    float4 tangent : TANGENT0;       // MikkTSpace (Blender's, the bakers'): xyz, w the bitangent's sign
    float4 bones0 : BLENDINDICES0;   // the palette's bones, 8 a vertex
    float4 bones1 : BLENDINDICES1;
    float4 weights0 : BLENDWEIGHT0;  // their weights (0: none), summing to 1
    float4 weights1 : BLENDWEIGHT1;
};

float4 mml_SkinAmbient : register(c120);   // rgb the ambient light (the material's ambient and emissive in), w the lights
float4 mml_SkinInfo : register(c121);      // x the vertices' alpha (material and fading), y the environment map's mode
                                           // (-1 none, 0, 1), z the palette's width in texels, w 1 / z
float4 mml_SkinEnv : register(c122);       // the environment map: x its scale, y and z its offsets
struct MmlSkinLight {   // LS3D's lights for this mesh, in the mesh's space (LightVertices's own choice)
    float4 pos;     // xyz, w the type (1 point, 2 spot, 3 directional)
    float4 dir;     // xyz (a spot's and a directional's, towards the light), w the near range
    float4 color;   // rgb (times the material's diffuse), w the far range
    float4 range;   // x 1 / (far - near), y a spot's outer cone (cos), z its inner (cos), w 1 / (inner - outer)
};
MmlSkinLight mml_SkinLights[8] : register(c124);

// the bones: 6 texels a bone (the position's matrix's three columns, then the normal's), R32G32B32A32F
sampler2D mml_Palette : register(s0);   // (a vertex sampler: D3DVERTEXTEXTURESAMPLER0)

float4 mml_PaletteTexel(float i) { return tex2Dlod(mml_Palette, float4((i + 0.5) * mml_SkinInfo.w, 0.5, 0, 0)); }

struct MmlSkinned { float3 pos; float3 normal; float3 tangent; };

// The vertex skinned: its place, normal and tangent in the mesh's space (the world's: through mml_World).
MmlSkinned mml_Skin(MmlSkinIn i) {
    MmlSkinned o;
    o.pos = 0;
    o.normal = 0;
    o.tangent = 0;
    float b[8] = {i.bones0.x, i.bones0.y, i.bones0.z, i.bones0.w, i.bones1.x, i.bones1.y, i.bones1.z, i.bones1.w};
    float w[8] = {i.weights0.x, i.weights0.y, i.weights0.z, i.weights0.w, i.weights1.x, i.weights1.y, i.weights1.z, i.weights1.w};
    float4 p = float4(i.pos, 1);
    [unroll] for (int k = 0; k < 8; k++) {
        if (w[k] > 0) {
            float t = b[k] * 6;
            float4 c0 = mml_PaletteTexel(t), c1 = mml_PaletteTexel(t + 1), c2 = mml_PaletteTexel(t + 2);
            float4 n0 = mml_PaletteTexel(t + 3), n1 = mml_PaletteTexel(t + 4), n2 = mml_PaletteTexel(t + 5);
            o.pos += w[k] * float3(dot(p, c0), dot(p, c1), dot(p, c2));
            o.normal += w[k] * float3(dot(i.normal, n0.xyz), dot(i.normal, n1.xyz), dot(i.normal, n2.xyz));
            o.tangent += w[k] * float3(dot(i.tangent.xyz, n0.xyz), dot(i.tangent.xyz, n1.xyz), dot(i.tangent.xyz, n2.xyz));
        }
    }
    return o;
}

// LS3D's colour of a vertex (LightVertices): the ambient plus each light's N.L (the normal as skinned, not made unit
// length: LS3D's), a point or spot light's falling off past its near range to nothing at its far one, a spot's within
// its cone; times 128 / 255 (LS3D's scale: the texture stage doubles it), alpha the material's.
float4 mml_SkinLight(float3 pos, float3 normal) {
    float3 c = mml_SkinAmbient.rgb;
    for (int k = 0; k < 8; k++) {
        if (k >= (int)mml_SkinAmbient.w) break;
        MmlSkinLight l = mml_SkinLights[k];
        float f = 0;
        if (l.pos.w > 2.5) {
            f = dot(l.dir.xyz, normal);
        } else {
            float3 d = l.pos.xyz - pos;
            float ndl = dot(d, normal), d2 = dot(d, d);
            if (ndl >= 0 && d2 <= l.color.w * l.color.w) {
                float dist = sqrt(d2);
                if (dist > l.dir.w) ndl *= (l.color.w - dist) * l.range.x;
                if (l.pos.w > 1.5) {
                    float cosA = dot(l.dir.xyz, d) / dist;
                    if (cosA > l.range.y) {
                        if (cosA < l.range.z) ndl *= (cosA - l.range.y) * l.range.w;
                        f = ndl / dist;
                    }
                } else {
                    f = ndl / dist;
                }
            }
        }
        if (f > 1e-8) c += l.color.rgb * f;
    }
    return float4(saturate(c * (128.0 / 255.0)), mml_SkinInfo.x);
}

// The environment map's uv (EnvMapCoords: modes 0 and 1), the world's place and normal of the vertex.
float2 mml_SkinEnvUv(float3 worldPos, float3 worldNormal) {
    float3 d = mml_Camera.xyz - worldPos;
    float3 r = worldNormal * (dot(worldNormal, d) * 2) - d;
    float inv = 1 / length(r);
    float t = (mml_SkinInfo.y < 0.5 ? r.z : r.y) * inv + 1;
    if (t < 1e-8) return float2(0.5 + mml_SkinEnv.y, 0.5 + mml_SkinEnv.z);
    float k = 1 / (sqrt(t) * mml_SkinEnv.x);
    return mml_SkinInfo.y < 0.5 ? float2(r.x * inv * k + 0.5 + mml_SkinEnv.y, -k * r.y * inv + 0.5 + mml_SkinEnv.z)
                                : float2(r.x * inv * k + 0.5 + mml_SkinEnv.y, k * inv * r.z + 0.5 + mml_SkinEnv.z);
}

#endif
