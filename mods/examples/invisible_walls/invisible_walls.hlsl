// invisible_walls' pass (invisible_walls.cpp): the walls of the static world that nothing draws, shown where the frame's
// depth says they are in front of what is drawn (or over the sky). Each pixel's ray (from the camera, through the
// pixel: ViewRay's forward component is 1, so the ray parameter is the linear depth) is tested against every wall
// triangle; the nearest hit nearer than the scene is shaded.
#include "mml/gfx/frame.hlsli"

cbuffer Walls : register(b1) {
    float4 g_meta;          // x: the walls (triangles), y: the grid's cell (m), z: the edge's width (m), w: the band's period (s)
    float4 g_colour;        // rgb, a: the strength
    float4 g_tri[128 * 3];  // each wall's three corners (xyz)
};

float4 PsWalls(float4 pos : SV_Position) : SV_Target {
    float z = SceneDepth(pos.xy);
    if (IsSky(z)) z = 1e9;              // nothing drawn here: a wall over the sky is still a wall
    const float3 o = g_eye.xyz;
    const float3 d = ViewRay(pos.xy);   // the world point at linear depth t is o + d * t

    // the nearest wall the ray meets, nearer than the scene
    float bestT = z;
    int best = -1;
    const int n = (int)g_meta.x;
    for (int i = 0; i < n; i++) {
        const float3 a = g_tri[i * 3].xyz;
        const float3 b = g_tri[i * 3 + 1].xyz;
        const float3 c = g_tri[i * 3 + 2].xyz;
        const float3 e1 = b - a;
        const float3 e2 = c - a;
        const float3 h = cross(d, e2);
        const float det = dot(e1, h);
        if (abs(det) < 1e-10) continue;
        const float f = 1 / det;
        const float3 s = o - a;
        const float u = f * dot(s, h);
        if (u < 0 || u > 1) continue;
        const float3 q = cross(s, e1);
        const float v = f * dot(d, q);
        if (v < 0 || u + v > 1) continue;
        const float t = f * dot(e2, q);
        if (t > 0 && t < bestT) { bestT = t; best = i; }
    }
    if (best < 0) return 0;

    const float3 a = g_tri[best * 3].xyz;
    const float3 b = g_tri[best * 3 + 1].xyz;
    const float3 c = g_tri[best * 3 + 2].xyz;
    const float3 p = o + d * bestT;
    const float3 n3 = normalize(cross(b - a, c - a));
    const float3 dn = normalize(d);
    // one pixel's size on the wall, in metres
    const float fp = max(bestT * 2.0 / (g_proj.y * g_size.y), 1e-4);

    // a glowing edge: the distance from the point to the nearest of the triangle's three edges
    const float dAB = length(cross(p - a, normalize(b - a)));
    const float dBC = length(cross(p - b, normalize(c - b)));
    const float dCA = length(cross(p - c, normalize(a - c)));
    const float edgeDist = min(dAB, min(dBC, dCA));
    const float edge = saturate(1 - max(edgeDist - g_meta.z, 0) / max(fp, 0.02));

    // a grid over the face, in its own plane: metres to the nearest line on each of its two axes
    const float3 U = normalize(b - a);
    const float3 V = cross(n3, U);
    const float2 uv = float2(dot(p - a, U), dot(p - a, V));
    const float cell = max(g_meta.y, 0.1);
    const float2 gf = abs(frac(uv / cell + 0.5) - 0.5) * cell;
    const float gridLine = saturate(1 - min(gf.x, gf.y) / (1.5 * fp));

    // a rim that brightens at grazing angles, and a band that rises through the wall every period
    const float fres = pow(1 - abs(dot(n3, dn)), 3);
    const float y0 = frac(g_eye.w / max(g_meta.w, 0.5)) * 90.0;
    const float band = exp(-pow((p.y - y0) / 0.7, 2));

    const float3 base = g_colour.rgb;
    const float strength = g_colour.a;
    const float fill = 0.10 * strength;
    const float edgeA = edge * 0.95 * strength;
    const float gridA = gridLine * 0.35 * strength;
    const float fresA = fres * 0.35 * strength;
    const float bandA = band * 0.45 * strength;
    const float a_out = saturate(fill + edgeA + gridA + fresA + bandA);
    float3 rgb = base * (fill + gridA + fresA + bandA) + lerp(base, float3(1, 1, 1), 0.6) * edgeA;
    rgb = min(rgb, a_out);   // premultiplied

    const float fade = 1 - FogAmount(bestT);
    return float4(rgb * fade, a_out * fade);
}
