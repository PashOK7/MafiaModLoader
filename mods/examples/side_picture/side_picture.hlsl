// side_picture's pass (side_picture.cpp): the 64-bit half's picture (a shared texture, t3) in a rectangle of the frame.
#include "mml/gfx/frame.hlsli"

Texture2D<float4> g_picture : register(t3);

cbuffer Picture : register(b1) {
    float4 g_rect;   // x, y, width, height (pixels of the frame)
};

float4 PsPicture(float4 pos : SV_Position) : SV_Target {
    float2 uv = (pos.xy - g_rect.xy) / g_rect.zw;
    float3 c = g_picture.Sample(g_linear, uv).rgb;
    float2 edge = min(pos.xy - g_rect.xy, g_rect.xy + g_rect.zw - pos.xy);
    if (min(edge.x, edge.y) < 2) c = 0;   // a thin black frame round it
    return float4(c, 1);                  // opaque (premultiplied)
}
