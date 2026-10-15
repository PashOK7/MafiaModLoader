// boxes: the mod's shaders, compiled in its build (CMakeLists.txt: mml_add_mod(... SHADERS boxes.hlsl:VsBox:vs_5_0
// ...)) into headers the mod includes (shaders/VsBox.h: kVsBox). Shader model 5: Direct3D 11.
//
// VsBox, PsBox: the bodies of the Jolt world, drawn instanced (one draw call): a cube of half extent 1 scaled by each
// body's half extents and placed by its world matrix. CsCoverage: counts the pixels drawn, so the mod reads back a
// number that proves the device drew this frame.
#include "boxes.hlsli"

struct Box {
    row_major float4x4 world;   // rows: the box's x, y, z axes and its position (Jolt's columns)
    float4 half_awake;          // xyz: half extents; w: 1 awake, 0 asleep, -1 static (the floor, the plank)
};
StructuredBuffer<Box> boxes : register(t0);

struct VsIn { float3 pos : POSITION; float3 normal : NORMAL; uint id : SV_InstanceID; };
struct VsOut { float4 pos : SV_Position; float3 normal : NORMAL; float3 color : COLOR; };

VsOut VsBox(VsIn v) {
    Box b = boxes[v.id];
    float4 world = mul(float4(v.pos * b.half_awake.xyz, 1), b.world);
    VsOut o;
    o.pos = mul(world, view_proj);
    o.normal = mul(float4(v.normal, 0), b.world).xyz;
    float state = b.half_awake.w;
    o.color = state > 0.5 ? HashColor(v.id) : state < -0.5 ? float3(0.22, 0.24, 0.27) : float3(0.45, 0.45, 0.5);   // asleep: grey
    return o;
}

float4 PsBox(VsOut i) : SV_Target {
    float lit = 0.35 + 0.65 * saturate(dot(normalize(i.normal), -light_dir.xyz));
    return float4(i.color * lit, 1);
}

Texture2D<float4> frame : register(t0);
RWByteAddressBuffer covered : register(u0);

[numthreads(8, 8, 1)]
void CsCoverage(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    frame.GetDimensions(w, h);
    if (id.x < w && id.y < h && frame[id.xy].a > 0.5)
        covered.InterlockedAdd(0, 1);
}
