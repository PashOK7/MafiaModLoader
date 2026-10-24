// The graphics bridge's Direct3D 9 side (host/gfx.cpp), shader model 3:
//   VsQuad       vs_3_0  a full-screen quad, its positions already in clip space
//   PsDepth      ps_3_0  the scene's depth (the INTZ texture, 0..1) as linear view depth, metres (R32F)
//   PsComposite  ps_3_0  the effects target (premultiplied RGBA16F) over the frame (blend ONE, INVSRCALPHA)
//   VsMotion     vs_3_0  a moving object's draw again (host/motion.cpp): its vertices through this frame's and the
//   PsMotion     ps_3_0  last frame's world, view and projection; each pixel's motion (pixels), its view depth, 1
//   VsMotionSkin vs_3_0  the same for a skinned mesh (a person): each vertex where it is now and where it was
//                        drawn the frame before (motion.cpp's copy of LS3D's skinned positions)
//   PsMotionAll  ps_3_0  the frame's motion vectors (G16R16F): the objects' where they are what is seen, else the
//                        camera's (the linear depth reprojected through the last frame's camera)
// Pixels come from VPOS (a pixel's top-left corner): texels and pixels match one to one, without Direct3D 9's
// half-pixel offset.

float4 g_size : register(c0);    // width, height, 1/width, 1/height
float4 g_depth : register(c1);   // the projection's _33, _43; the value written where nothing was drawn (d >= 1)
float4 g_drawn : register(c3);   // the size the world is drawn at (the frame's, or an upscaler's render size at the
                                 // targets' top-left: gfx_upscale): width, height, 1/width, 1/height

sampler2D g_tex : register(s0);

float4 VsQuad(float4 pos : POSITION) : POSITION { return pos; }

float4 PsDepth(float2 vpos : VPOS) : COLOR0 {
    float d = tex2D(g_tex, (vpos + 0.5) * g_size.zw).r;
    // a row-vector perspective projection: z_ndc = _33 + _43 / z_view, so z_view = _43 / (z_ndc - _33)
    float den = d - g_depth.x;
    float z = d >= 1.0 || abs(den) < 1e-9 ? g_depth.z : g_depth.y / den;
    return float4(z, 0, 0, 0);
}

float4 PsComposite(float2 vpos : VPOS) : COLOR0 { return tex2D(g_tex, (vpos + 0.5) * g_size.zw); }

// ---- motion vectors (host/motion.cpp, gfx.cpp). Motion is in pixels, DLSS's and TAA's convention: this pixel plus
// its motion is where the surface it shows was on the screen the frame before.

row_major float4x4 g_now : register(c4);    // VsMotion: world * view * projection, this frame (row vectors)
row_major float4x4 g_before : register(c8); // VsMotion: the last frame's; PsMotionAll: this frame's view space to the
                                            // last frame's clip space (inverse(view) * view before * projection before)
float4 g_proj : register(c12);              // PsMotionAll: this frame's projection _11, _22, _31, _32

struct MotionV { float4 pos : POSITION; float4 now : TEXCOORD0; float4 before : TEXCOORD1; float2 uv : TEXCOORD2; };

// (the draw's own vertex layout, its FVF: the texture's place for an alpha-tested draw)
MotionV VsMotion(float3 pos : POSITION, float2 uv : TEXCOORD0) {
    MotionV o;
    o.pos = mul(float4(pos, 1), g_now);
    o.now = o.pos;
    o.before = mul(float4(pos, 1), g_before);
    o.uv = uv;
    return o;
}

// a skinned mesh: LS3D skins on the CPU, so each vertex has its own last position (POSITION1), drawn the frame before
MotionV VsMotionSkin(float3 pos : POSITION0, float3 was : POSITION1) {
    MotionV o;
    o.pos = mul(float4(pos, 1), g_now);
    o.now = o.pos;
    o.before = mul(float4(was, 1), g_before);
    o.uv = 0;
    return o;
}

// a clip-space point where it is drawn (pixels of the picture as drawn: g_drawn)
float2 ToScreen(float4 clip) { return float2((clip.x / clip.w + 1) * 0.5 * g_drawn.x, (1 - clip.y / clip.w) * 0.5 * g_drawn.y); }

// PsMotion: x the draw's alpha test (its reference, 0..1; < 0 none: the texture at s0 is the draw's own, its alpha
// what the test compares)
float4 g_motionDraw : register(c2);

float4 PsMotion(float2 vpos : VPOS, float4 now : TEXCOORD0, float4 before : TEXCOORD1, float2 uv : TEXCOORD2) : COLOR0 {
    if (g_motionDraw.x >= 0) clip(tex2D(g_tex, uv).a - g_motionDraw.x);   // (where the draw's own test left it out)
    // (where this pixel is, through the same mapping as where it was: Direct3D 9 puts a pixel's centre at the whole
    // coordinate, ToScreen at its + 0.5, and vpos + 0.5 here left every moving object's vector half a pixel off on
    // both axes at any speed: its edges shook under DLSS)
    float2 motion = before.w > 1e-4 && now.w > 1e-4 ? ToScreen(before) - ToScreen(now) : 0;
    return float4(motion, now.w, 1);   // (w of a perspective projection: the view depth, compared in PsMotionAll)
}

// s0 the objects' target (PsMotion's, cleared to 0), s1 the linear view depth (PsDepth's)
sampler2D g_objects : register(s0);
sampler2D g_viewDepth : register(s1);

float4 PsMotionAll(float2 vpos : VPOS) : COLOR0 {
    float2 here = vpos + 0.5;
    float2 uv = here * g_size.zw;
    float z = tex2D(g_viewDepth, uv).r;
    float4 o = tex2D(g_objects, uv);
    // an object's, where the object is what is seen (its depth is the frame's: a nearer thing drawn after it has
    // another; a shadow on the road has the road's, glass over the street far behind it has not)
    if (o.w > 0.5 && abs(o.z - z) <= 0.01 * z + 0.05) return float4(o.xy, 0, 0);
    // the camera's: this pixel's place in view space, through the last frame's camera; the sky (g_depth.z) as a
    // direction alone (its turn without its move)
    float2 drawn = here * g_drawn.zw;   // (where in the picture as drawn: the projection's own range)
    float2 ndc = float2(drawn.x * 2 - 1, 1 - drawn.y * 2);
    bool sky = z >= g_depth.z * 0.999;
    float d = sky ? 1 : z;
    float3 v = float3((ndc.x - g_proj.z) / g_proj.x * d, (ndc.y - g_proj.w) / g_proj.y * d, d);
    float4 before = mul(float4(v, sky ? 0 : 1), g_before);
    return float4(before.w > 1e-4 ? ToScreen(before) - here : 0, 0, 0);
}

// The game's brightness in a window (host/gamma.cpp, on DXVK): the frame's copy (s0) through the game's gamma ramp,
// 256 texels at s1 (each channel its own row of the ramp: red, green, blue), filtered between the ramp's entries.
sampler2D g_ramp : register(s1);
float4 PsGamma(float2 vpos : VPOS) : COLOR0 {
    float3 c = tex2D(g_tex, (vpos + 0.5) * g_size.zw).rgb;
    float3 u = c * (255.0 / 256.0) + 0.5 / 256.0;
    return float4(tex2D(g_ramp, float2(u.r, 0.5)).r, tex2D(g_ramp, float2(u.g, 0.5)).g, tex2D(g_ramp, float2(u.b, 0.5)).b, 1);
}
