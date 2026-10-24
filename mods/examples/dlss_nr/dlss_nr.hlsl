// dlss_nr's drawing (dlss_nr.cpp's graphics pass, Direct3D 11, shader model 5). DLSS (5 Neural Rendering, Super
// Resolution) runs in the 64-bit half over the frame it is given: PsPrepare gives it the frame at the render size (a
// slot's shared textures "color_in", "depth_in", "mvec_in", "mask_in"), PsComposite puts what came back ("color_out")
// on the screen, PsDebug shows the inputs.
#include "mml/gfx/frame.hlsli"

cbuffer DlssNr : register(b1) {
    float4 g_prevEye;     // the camera of the frame before (as g_eye...): the motion vectors reproject into it
    float4 g_prevRight;
    float4 g_prevUp;
    float4 g_prevFwd;
    float4 g_prevProj;    // its projection's _11, _22, _31, _32
    float4 g_opts;        // x: the mask on the sky (0..1), y: 1 when there is a frame before, z: the split's place
                          // (0..1 of the width, 0: none), w: the debug view (1 motion, 2 depth, 3 mask)
    float4 g_opts2;       // x: 1 when the bridge's motion vectors are bound (frame.hlsli g_motion, t3), y: their scale (a test)
    float4 g_render;      // xy: the frame's pixels a render pixel covers (the frame's size / the render size), zw: where
                          // in its render pixel this frame's inputs are sampled (the jitter, -0.5..0.5)
    float4 g_opts3;       // xy: the frame's size (the picture DLSS draws), z: 1 when the world was drawn at the render size
                          // (gfx_upscale: g_size is the render size, the inputs its pixels at the top-left of the
                          // frame's textures, read one to one; g_render.xy 1, zw 0: the jitter is the game's projection's)
};

Texture2D<float4> g_nrOut : register(t5);   // "color_out": DLSS-NR's picture, in the display's gamma (as it was given)
Texture2D<float4> g_prevColor : register(t6);   // the frame before (the debug views' reprojection check)

// Where a world direction or point was on the screen the frame before (pixels). A point at infinity (the sky): its
// direction alone, the camera's turn without its move.
float2 PrevScreen(float3 d) {
    float z = dot(d, g_prevFwd.xyz);
    float2 ndc = float2(dot(d, g_prevRight.xyz) * g_prevProj.x / z + g_prevProj.z, dot(d, g_prevUp.xyz) * g_prevProj.y / z + g_prevProj.w);
    return float2((ndc.x + 1) * 0.5 * g_size.x, (1 - ndc.y) * 0.5 * g_size.y);
}

// This pixel's motion, in pixels, from where it is now to where it was the frame before (DLSS's convention: the
// current place plus the vector is the last place). The bridge's when it gives them (the camera's and the moving
// cars' and people's own: gfx_motion); else the camera's alone: the depth reprojected through the last frame's camera.
float2 Motion(float2 pixel) {
    if (g_opts.y < 0.5) return 0;
    if (g_opts2.x > 0.5) return g_motion.Load(int3(pixel, 0)) * g_opts2.y;
    float z = SceneDepth(pixel);
    float3 ray = ViewRay(pixel);
    float3 d = IsSky(z) ? ray : g_eye.xyz + ray * z - g_prevEye.xyz;
    if (dot(d, g_prevFwd.xyz) <= 1e-4) return 0;   // (behind the last camera: no place there)
    return (PrevScreen(d) - pixel) * g_opts2.y;
}

float Mask(float2 pixel) { return IsSky(SceneDepth(pixel)) ? g_opts.x : 1; }

struct Prepared {
    float4 color : SV_Target0;   // the frame as the game drew it: an SDR picture in the display's gamma (0..1), what
                                 // DLSS-NR takes (RTX Remix gives it pow(x, 1 / 2.2) of its tone-mapped colour and
                                 // reads its picture back with pow(x, 2.2): DLSS-NR's model works on gamma values)
    float depth : SV_Target1;    // its depth as the game's projection makes it: _33 + _43 / z (the sky 1)
    float4 motion : SV_Target2;  // rg: the motion vector in pixels
    float4 mask : SV_Target3;    // DLSS-NR's control mask: how much it may change each pixel
};

// One render pixel: the frame at its jittered sample point (the game drew the frame at its full size; a render pixel
// sampled at one point of it is what a render at the smaller size would have drawn there, its jitter the sub-pixel
// sample DLSS gathers over the frames). The colour is filtered at that very point: the frame's pixel under it would
// move the sample to that pixel's centre, by another amount in each pixel where the frame's size is no whole multiple
// of the render size (1280 / 427 in Ultra Performance), against the jitter DLSS is told: the picture shimmered when
// still (tools/still_test.py). The depth and the motion are the nearest pixel's (not blended across an edge). The
// motion vector in render pixels.
// With the world drawn at the render size (g_opts3.z) a render pixel is the picture's own pixel, read as it is.
Prepared PsPrepare(float4 pos : SV_Position) {
    Prepared o;
    float2 at = clamp((floor(pos.xy) + 0.5 + g_render.zw) * g_render.xy, 0.5, g_size.xy - 0.5);
    float2 pixel = floor(at) + 0.5;
    o.color = float4(saturate(g_opts3.z > 0.5 ? g_sceneColor.Load(int3(pixel, 0)).rgb :
                                                g_sceneColor.SampleLevel(g_linear, at * g_size.zw, 0).rgb), 1);
    float z = SceneDepth(pixel);
    o.depth = IsSky(z) ? 1 : saturate(g_depthProj.x + g_depthProj.y / max(z, 1e-3));
    o.motion = float4(Motion(pixel) / g_render.xy, 0, 0);
    o.mask = Mask(pixel).xxxx;
    return o;
}

// DLSS-NR's picture as the frame (alpha 1), in the gamma it came in. With the split on, the game's own frame left
// of it and a thin line where it is: the difference seen at once.
// (The game's own frame: under gfx_upscale the world drawn at the render size, brought up to the frame's filtered.)
float4 PsComposite(float4 pos : SV_Position) : SV_Target {
    float split = g_opts.z * g_opts3.x;
    if (g_opts.z > 0 && abs(pos.x - split) < 1) return float4(1, 1, 1, 1);
    if (g_opts.z > 0 && pos.x < split) {
        float2 uv = g_opts3.z > 0.5 ? pos.xy * g_size.xy / (g_opts3.xy * g_opts3.xy) : pos.xy * g_size.zw;
        return float4(g_sceneColor.SampleLevel(g_linear, uv, 0).rgb, 1);
    }
    return float4(saturate(g_nrOut.Load(int3(pos.xy, 0)).rgb), 1);
}

// The showing (frame generation): the back buffer (t1, as the bridge gives it at the showing) into DLSS-FG's input,
// DLSS-FG's pictures (t5) into the bridge's targets, and the stand-in for a GPU without DLSS-FG: the frame before (t6)
// and this one blended at the picture's time (g_render.x: 0 the frame before .. 1 this one).
// The motion vectors as DLSS is given them (the debug view 7: the slot's "mvec_in" at t5, at the render size), in the
// colours of the debug view 1 (back in the frame's pixels): the same picture when they reach DLSS whole.
float4 PsMvecIn(float4 pos : SV_Position) : SV_Target {
    float2 toRender = g_opts3.z > 0.5 ? g_size.xy / g_opts3.xy : 1 / g_render.xy;   // (a frame pixel in render pixels)
    float2 m = g_nrOut.Load(int3(pos.xy * toRender, 0)).rg / toRender;
    return float4(saturate(0.5 + m / 32), 0.5, 1);
}
float4 PsCopyScene(float4 pos : SV_Position) : SV_Target { return float4(g_sceneColor.Load(int3(pos.xy, 0)).rgb, 1); }
float4 PsCopyIn(float4 pos : SV_Position) : SV_Target { return float4(g_nrOut.Load(int3(pos.xy, 0)).rgb, 1); }
float4 PsBlend(float4 pos : SV_Position) : SV_Target {
    float3 before = g_prevColor.Load(int3(pos.xy, 0)).rgb, now = g_sceneColor.Load(int3(pos.xy, 0)).rgb;
    return float4(lerp(before, now, g_render.x), 1);
}

// The inputs as DLSS-NR gets them (the settings' debug view): the motion vectors as colours (grey: still; red right,
// green down: grey plus a half for 16 pixels), the depth (white near, darker on a log scale to 500 m, the sky black),
// the mask; the check of the motion vectors (5): the frame before, fetched where each pixel's vector says it was,
// against this frame (four times the difference: black where the vectors are right, the moving actors aside), and
// (6) the same with no vectors, to compare.
// (Under gfx_upscale the inputs are the render size's: each frame pixel shows the render pixel under it.)
float2 InPixel(float2 pos) { return g_opts3.z > 0.5 ? floor(pos * g_size.xy / g_opts3.xy) + 0.5 : pos; }
float2 InUv(float2 at) { return at / (g_opts3.z > 0.5 ? g_opts3.xy : g_size.xy); }   // (the textures are the frame's size)
float4 PsDebug(float4 pos : SV_Position) : SV_Target {
    float2 p = InPixel(pos.xy);
    if (g_opts.w > 4.5) {
        float2 at = p + (g_opts.w < 5.5 ? Motion(p) : 0);
        float3 now = g_sceneColor.Load(int3(p, 0)).rgb;
        float3 before = g_prevColor.SampleLevel(g_linear, InUv(at), 0).rgb;
        return float4(saturate(abs(now - before) * 4), 1);
    }
    if (g_opts.w < 1.5) {
        float2 m = Motion(p);
        return float4(saturate(0.5 + m / 32), 0.5, 1);
    }
    if (g_opts.w < 2.5) {
        float z = SceneDepth(p);
        return float4((IsSky(z) ? 0 : 1 - saturate(log2(1 + z) / log2(501))).xxx, 1);
    }
    return float4(Mask(p).xxx, 1);
}
