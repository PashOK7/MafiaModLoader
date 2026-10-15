// The graphics bridge's Direct3D 9 side (host/gfx.cpp), shader model 3:
//   VsQuad       vs_3_0  a full-screen quad, its positions already in clip space
//   PsDepth      ps_3_0  the scene's depth (the INTZ texture, 0..1) as linear view depth, metres (R32F)
//   PsComposite  ps_3_0  the effects target (premultiplied RGBA16F) over the frame (blend ONE, INVSRCALPHA)
// Pixels come from VPOS (a pixel's top-left corner): texels and pixels match one to one, without Direct3D 9's
// half-pixel offset.

float4 g_size : register(c0);    // width, height, 1/width, 1/height
float4 g_depth : register(c1);   // the projection's _33, _43; the value written where nothing was drawn (d >= 1)

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
