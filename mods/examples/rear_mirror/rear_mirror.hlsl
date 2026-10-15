// rear_mirror's pass (rear_mirror.cpp): a scene view's colour copied into a rectangle of the frame.
#include "mml/gfx/frame.hlsli"

Texture2D<float4> g_view : register(t2);   // the view's colour (MmlGfxView::color)

cbuffer Mirror : register(b1) {
    float4 g_rect;   // the rectangle, pixels: left, top, right, bottom
    float4 g_opt;    // x: 1 flipped left to right (a mirror); y: a dark border's width, pixels; z: 1 the difference
                     // from the frame under it, 8 times (the check: black where they are the same); 2 a puddle;
                     // 3 the turning mirror
    float4 g_puddle; // the puddle: x the plane's height (world y), y its radius (metres), zw its centre (world x, z)
    float4 g_mc;     // the turning mirror's centre (world), w: 1 its front is seen and the view is this frame's
    float4 g_mn;     // its front's normal (unit), w: half its width
    float4 g_mu;     // its up (unit), w: half its height
    float4 g_mr;     // its right (unit), w: its frame's width (metres)
    row_major float4x4 g_vp;   // the view's view x projection (row vectors): a world point to the view's picture
};

// The turning mirror: where this pixel's ray meets it, if nothing the frame drew stands in front of it there
// (alpha -1: it is not there)
float4 Spin(float2 p) {
    float3 d = ViewRay(p);                           // its forward component is 1: t is a linear view depth
    float den = dot(d, g_mn.xyz);
    if (abs(den) < 1e-5) return -1;
    float t = dot(g_mc.xyz - g_eye.xyz, g_mn.xyz) / den;
    if (t <= 0 || t >= SceneDepth(p)) return -1;     // behind the camera, or behind what the frame shows there
    float3 h = g_eye.xyz + d * t, q = h - g_mc.xyz;
    float2 e = abs(float2(dot(q, g_mr.xyz), dot(q, g_mu.xyz))) - float2(g_mn.w, g_mu.w);
    if (any(e > g_mr.w)) return -1;                  // beyond its frame
    float3 c;
    if (any(e > 0)) {                                // the frame: dark wood, lighter on its outer edge
        c = float3(0.32, 0.19, 0.09) * (0.7 + 0.3 * saturate(max(e.x, e.y) / g_mr.w));
    } else if (dot(g_eye.xyz - g_mc.xyz, g_mn.xyz) <= 0 || g_mc.w < 0.5) {
        c = float3(0.33, 0.33, 0.35);                // its back (and no picture this frame)
    } else {
        float4 v = mul(float4(h, 1), g_vp);          // the same point in the mirrored camera's picture
        float2 uv = v.xy / v.w * float2(0.5, -0.5) + 0.5;
        c = g_view.SampleLevel(g_linear, uv, 0).rgb * float3(0.93, 0.95, 0.96);   // a glass's slight tint
        return float4(c, 1);                         // (the picture is fogged already, along its whole path)
    }
    return float4(ApplyFog(c, t), 1);
}

float4 PsMirror(float4 pos : SV_Position) : SV_Target {
    float2 p = pos.xy;
    if (g_opt.z > 2.5) {
        float4 s = Spin(p);
        if (s.a < 0) discard;
        return s;
    }
    if (g_opt.z > 1.5) {   // the puddle: the reflected view, the frame's size, flipped left to right, on the ground
        float z = SceneDepth(p);
        if (IsSky(z)) discard;
        float3 w = g_eye.xyz + ViewRay(p) * z;
        float r = distance(w.xz, g_puddle.zw) / g_puddle.y;
        if (abs(w.y - g_puddle.x) > 0.15 || r > 1) discard;   // only the ground at the plane's height, near the centre
        float3 c = g_view.Load(int3(g_size.x - 1 - p.x, p.y, 0)).rgb;
        float a = 0.75 * saturate((1 - r) * 4);   // its edge fades
        return float4(c * a, a);
    }
    float b = g_opt.y;
    if (any(p < g_rect.xy - b) || any(p > g_rect.zw + b)) discard;   // outside: left as it is (another view's)
    if (any(p < g_rect.xy) || any(p > g_rect.zw)) return float4(0.03, 0.03, 0.03, 1);   // the border
    if (g_opt.z > 0.5) {   // pixel for pixel: the view is the rectangle's size
        float3 v = g_view.Load(int3(p - g_rect.xy, 0)).rgb, f = g_sceneColor.Load(int3(p, 0)).rgb;
        return float4(saturate(abs(v - f) * 8), 1);
    }
    float2 uv = (p - g_rect.xy) / (g_rect.zw - g_rect.xy);
    if (g_opt.x > 0.5) uv.x = 1 - uv.x;
    return float4(g_view.SampleLevel(g_linear, uv, 0).rgb, 1);   // opaque (the view's alpha is not coverage)
}
