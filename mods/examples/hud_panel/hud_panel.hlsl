// hud_panel's pass (hud_panel.cpp): the frame's 2D pass (the HUD, the menus) on a panel in the 3D world.
#include "mml/gfx/frame.hlsli"

Texture2D<float4> g_ui : register(t2);   // the 2D pass, premultiplied (MmlGfxFrame::ui)

cbuffer Panel : register(b1) {
    float4 g_center;   // xyz: the panel's centre (world), w: its backing's opacity
    float4 g_u;        // xyz: its right edge from the centre (half its width along its right axis)
    float4 g_v;        // xyz: its top edge from the centre
    float4 g_n;        // xyz: its normal (towards the camera)
};

float4 PsPanel(float4 pos : SV_Position) : SV_Target {
    float3 dir = ViewRay(pos.xy);   // its forward component 1: the ray's t is the point's linear view depth
    float dn = dot(dir, g_n.xyz);
    if (abs(dn) < 1e-5) return 0;
    float t = dot(g_center.xyz - g_eye.xyz, g_n.xyz) / dn;
    if (t <= 0.05 || t > SceneDepth(pos.xy)) return 0;   // behind the camera, or behind what the world shows there
    float3 p = g_eye.xyz + dir * t - g_center.xyz;
    float2 q = float2(dot(p, g_u.xyz) / dot(g_u.xyz, g_u.xyz), dot(p, g_v.xyz) / dot(g_v.xyz, g_v.xyz));   // -1..1
    if (any(abs(q) > 1)) return 0;
    float4 ui = g_ui.Sample(g_linear, float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5));
    float edge = max(abs(q.x), abs(q.y)) > 0.985 ? 1 : 0;
    float4 back = lerp(float4(0.02, 0.03, 0.05, 1) * g_center.w, float4(0.25, 0.7, 0.9, 0.9), edge);
    return ui + back * (1 - ui.a);   // the 2D over the panel's backing, premultiplied
}
