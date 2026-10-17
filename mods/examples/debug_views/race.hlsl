// debug_views, race: the paint (race.cpp, Pass): the race's gates, the player's gate zone and the car's probe point painted on
// the world's surfaces, from the frame's depth. A gate's line is infinite in the ground plane (x, z): it is painted
// wherever a surface of the world meets its vertical plane, so it lies on the ground where there is ground and on the
// walls it crosses, and what stands in front of it hides it. The part between the gate's two nodes is painted
// brighter (the real gate); the rest of its line dimmer. The zone is the strip between the infinite lines of the
// previous gate and the current one, shaded where it lies on the world. The probe is a disc on the ground under it.
#include "mml/gfx/frame.hlsli"

cbuffer RacePaint : register(b1) {
    float4 g_seg[32];      // a gate's track node and its outer marker, in the ground plane: (x, z, x, z)
    float4 g_kind[32];     // x: 0 another gate, 1 the lap line (gate 0), 2 the player's current gate, 3 a neighbour
    float4 g_colour[5];    // rgb and a (the strength) of the kinds 0..3, then the probe (4)
    float4 g_zone;         // x the previous gate's index, y the current gate's, z 1 when the zone is shaded
    float4 g_probe;        // xyz the car's probe point, w 1 when there is one
    float4 g_style;        // x the line's width in pixels, y the zone's strength, z the probe disc's radius (metres), w the gates
};

// The gate's infinite line in the ground plane: the signed distance (metres) of q from it, and along: where q lies on
// the gate's own segment (0 at its track node, 1 at its outer marker; outside [0, 1] beyond the gate's ends).
float LineSide(float2 q, int i, out float along) {
    float2 a = g_seg[i].xy;
    float2 d = g_seg[i].zw - a;
    float len = max(length(d), 1e-4);
    along = dot(q - a, d) / (len * len);
    return (d.x * (q.y - a.y) - d.y * (q.x - a.x)) / len;
}

float4 PsRace(float4 pos : SV_Position) : SV_Target {
    float z = SceneDepth(pos.xy);
    if (IsSky(z)) return 0;
    float3 p = ScenePoint(pos.xy);
    float2 q = p.xz;
    // the world's size of one pixel at this depth (vertically: the projection's y scale and the frame's height)
    float fp = max(z * 2.0 / (g_proj.y * g_size.y), 1e-4);
    float fade = 1 - FogAmount(z);
    float3 rgb = 0;   // premultiplied colour and alpha, the layers laid one over another
    float a = 0;
    int n = (int)g_style.w;
    for (int i = 0; i < n; i++) {
        float along;
        float s = LineSide(q, i, along);
        // a line one pixel wide, antialiased over the pixel's edge
        float cover = saturate(0.5 + (0.5 * g_style.x * fp - abs(s)) / fp);
        if (cover <= 0) continue;
        int kind = (int)g_kind[i].x;
        float4 col = g_colour[kind];
        // every gate line is as strong as the others: the kinds differ by colour only, never by opacity or width
        float alpha = cover * col.a * fade;
        rgb = col.rgb * alpha + rgb * (1 - alpha);
        a = alpha + a * (1 - alpha);
    }
    if (g_zone.z > 0.5) {
        float ap, ac;
        float sp = LineSide(q, (int)g_zone.x, ap);
        float sc = LineSide(q, (int)g_zone.y, ac);
        if (sp * sc < 0) {   // between the two infinite lines: inside the zone (no limit at the gates' ends)
            float alpha = g_style.y * fade;
            rgb = g_colour[2].rgb * alpha + rgb * (1 - alpha);
            a = alpha + a * (1 - alpha);
        }
    }
    if (g_probe.w > 0.5 && abs(p.y - g_probe.y) < 1.5) {
        float dist = length(q - g_probe.xz);
        float cover = saturate(0.5 + (g_style.z - dist) / fp);
        if (cover > 0) {
            float alpha = cover * g_colour[4].a * fade;
            rgb = g_colour[4].rgb * alpha + rgb * (1 - alpha);
            a = alpha + a * (1 - alpha);
        }
    }
    return float4(rgb, a);
}
