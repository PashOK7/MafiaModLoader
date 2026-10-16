// What every Direct3D 11 pass of the graphics bridge gets (mml.h gfx_pass, MmlGfxFrame; docs/MODDING.md "Effects
// over the frame"), shader model 5. A mod's shader: #include "mml/gfx/frame.hlsli" (mml_add_mod's SHADERS have the
// SDK on their include path).
//
// World space is the game's (metres, y up). Linear view depth is the distance along the camera's forward axis. A pass
// draws into the effects target: premultiplied RGBA (rgb already times alpha; alpha: how much of the frame it covers),
// composited over the frame as frame * (1 - alpha) + rgb, where the world is drawn and the HUD is not yet. The passes
// are laid over each other in their order the same way (the bridge's blend state: ONE, INV_SRC_ALPHA), so a pass that
// returns 0 leaves what the passes before it drew.

cbuffer MmlFrame : register(b0) {
    float4 g_eye;         // the camera's position, w: the time in seconds
    float4 g_right;       // the camera's axes in the world (unit), w unused
    float4 g_up;
    float4 g_fwd;
    float4 g_proj;        // the projection's _11, _22, _31, _32 (row vectors)
    float4 g_size;        // width, height, 1/width, 1/height (pixels)
    float4 g_sun;         // the direction towards the sun (unit), w: 1 the game's (its Direct3D light, else the scene's sun), 0 a default
    float4 g_depthProj;   // the projection's _33, _43; the linear depth of the sky (where nothing was drawn); unused
    float4 g_sunColor;    // the sun's colour, w: 1 the game's
    float4 g_fog;         // the world's fog: start, end (linear), density (exp), mode (0 none, 1 exp, 2 exp2, 3 linear)
    float4 g_fogColor;    // its colour, w: 1 when on
    float4 g_ambient;     // the ambient light, w: 1 the scene's (else a default grey)
};

Texture2D<float> g_sceneDepth : register(t0);    // linear view depth, metres (the sky: g_depthProj.z)
Texture2D<float4> g_sceneColor : register(t1);   // the frame as drawn so far (the world, no HUD)
SamplerState g_point : register(s0);
SamplerState g_linear : register(s1);

// A full-screen triangle: Draw(3), no buffers (the bridge binds it as the vertex shader before each pass).
float4 VsFullScreen(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// The world direction through a pixel, scaled so that its forward component is 1: the point at linear view depth z
// is g_eye.xyz + ViewRay(pixel) * z.
float3 ViewRay(float2 pixel) {
    float2 ndc = float2(pixel.x * g_size.z * 2 - 1, 1 - pixel.y * g_size.w * 2);
    float vx = (ndc.x - g_proj.z) / g_proj.x;
    float vy = (ndc.y - g_proj.w) / g_proj.y;
    return g_right.xyz * vx + g_up.xyz * vy + g_fwd.xyz;
}

float SceneDepth(float2 pixel) { return g_sceneDepth.Load(int3(pixel, 0)); }
bool IsSky(float z) { return z >= g_depthProj.z * 0.5; }

// The world point a pixel shows (where the scene was drawn).
float3 ScenePoint(float2 pixel) { return g_eye.xyz + ViewRay(pixel) * SceneDepth(pixel); }

// How much of the world's fog lies in front of a point at linear view depth z (0 none, 1 all), as Direct3D's
// fixed-function fog has it: effects fade into the distance as the world does.
float FogAmount(float z) {
    if (g_fogColor.w < 0.5) return 0;
    if (g_fog.w > 2.5) return saturate((z - g_fog.x) / max(g_fog.y - g_fog.x, 1e-3));
    float d = g_fog.z * z;
    return g_fog.w > 1.5 ? 1 - exp(-d * d) : 1 - exp(-d);
}
float3 ApplyFog(float3 color, float z) { return lerp(color, g_fogColor.rgb, FogAmount(z)); }

// The game draws in display space (gamma about 2.2): add and mix light in linear terms (Lin), back for the frame (Enc).
float3 Lin(float3 c) { return pow(max(c, 0), 2.2); }
float3 Enc(float3 c) { return pow(max(c, 0), 1 / 2.2); }

// A world point on the screen (pixels); z: its linear view depth.
float3 ToScreen(float3 p) {
    float3 d = p - g_eye.xyz;
    float z = dot(d, g_fwd.xyz);
    float2 ndc = float2(dot(d, g_right.xyz) * g_proj.x / z + g_proj.z, dot(d, g_up.xyz) * g_proj.y / z + g_proj.w);
    return float3((ndc.x + 1) * 0.5 * g_size.x, (1 - ndc.y) * 0.5 * g_size.y, z);
}
