// boxes: what its shaders share (an .hlsli beside a mod's shaders: a change to it compiles them again).
#ifndef BOXES_HLSLI
#define BOXES_HLSLI

cbuffer Frame : register(b0) {
    row_major float4x4 view_proj;   // a row vector times this: clip space (the mod's own camera on its world)
    float4 light_dir;               // xyz: the light's direction, towards the scene
};

float3 HashColor(uint i) {          // a steady colour for a box by its index
    uint h = i * 2654435761u;
    return float3((h >> 24) & 255, (h >> 16) & 255, (h >> 8) & 255) / 255.0 * 0.7 + 0.3;
}

#endif
