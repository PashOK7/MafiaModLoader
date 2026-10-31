// The wind's ripples on water (mml/water.h makes their spectrum; mml/gfx/water.hlsli reads what this makes): a
// spectrum made into the surface's slopes, heights and horizontal displacement by an inverse FFT on the GPU each
// frame. A mod's .hlsl includes this file and names its entry points in its SHADERS (mods/examples/river_water:
// river_waves.hlsl); its C++ side is mml/water.h's mml::water::Waves (the textures, the dispatches, the mips). Three
// cascades (patches of different sizes, each carrying its own band of wavelengths) tile the water without a visible
// repeat; what they leave out (the capillaries) is a roughness.
//
// The displacement is Tessendorf's choppy waves: each point moved towards the crests by D = -i k/|k| h (a Gerstner
// wave's), so the crests come out sharper and steeper and the troughs flatter. Its value and derivatives are kept:
// the displacement moves tessellated vertices; with the slopes its derivatives give the surface's normal and its
// Jacobian (below 1 the surface is squeezed at a crest, below 0 it folds over: a breaking crest).
//
// CsSpectrum: each wavenumber's amplitude at this time (the initial spectrum h0, made on the CPU from JONSWAP for the
// wind and its fetch, turned by the waves' dispersion), as four complex fields, each holding two real fields (x + i y):
// the slopes (dh/dx + i dh/dz), the height and dDx/dx, dDz/dz and dDx/dz (the fourth unused).
// CsFftRows, CsFftCols: the inverse FFT of two complex fields, 256 a line (Stockham, radix 2, in groupshared memory):
// run on each of the two textures.
// CsResolve: the slopes (x, z) and their squares; the height and the displacement's derivatives. Their mipmaps give
// every pixel its mean slope and shape and the variance of the slopes it averages (the roughness it cannot draw).

#define N 256

cbuffer Waves : register(b0) {
    float4 g_patch;   // xyz: the cascades' sizes (metres), w: the time (seconds)
};

Texture2DArray<float4> g_h0 : register(t0);   // h0(k) (xy) and conj(h0(-k)) (zw), the wavenumber index centred
Texture2DArray<float4> g_in : register(t1);   // the FFT's input; CsResolve: xy the slopes, zw the height and dDx/dx
Texture2DArray<float4> g_in2 : register(t2);  // CsResolve: x dDz/dz, y dDx/dz
RWTexture2DArray<float4> g_out : register(u0);
RWTexture2DArray<float4> g_slopes : register(u1);
RWTexture2DArray<float4> g_shape : register(u2);   // the height, dDx/dx, dDz/dz, dDx/dz
RWTexture2DArray<float4> g_out2 : register(u3);
RWTexture2DArray<float4> g_displacement : register(u4); // height, Dx, Dz, unused; actual vertex displacement

float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
float2 TimesI(float2 a) { return float2(-a.y, a.x); }

[numthreads(8, 8, 1)]
void CsSpectrum(uint3 id : SV_DispatchThreadID) {
    float2 k = (float2(id.xy) - N / 2) * (6.2831853 / g_patch[id.z]);
    float kl = length(k);
    float w = sqrt(9.81 * kl + 7.4e-5 * kl * kl * kl);   // gravity and capillary waves
    float4 h0 = g_h0[id];
    float s, c;
    sincos(w * g_patch.w, s, c);
    float2 h = CMul(h0.xy, float2(c, s)) + CMul(h0.zw, float2(c, -s));
    float2 kn = kl > 0 ? k / kl : 0;
    // the slopes: i kx h + i (i kz h); the height h + i dDx/dx; dDz/dz + i dDx/dz (D = -i k/|k| h: dDx/dx = kx^2/|k| h)
    g_out[id] = float4(TimesI(k.x * h) - k.y * h, h + TimesI(k.x * kn.x * h));
    g_out2[id] = float4(k.y * kn.y * h + TimesI(k.x * kn.y * h), -TimesI(kn.x * h) + kn.y * h);
}

groupshared float4 g_buf[2][N];

void Fft(uint t, uint row, uint cascade, bool rows) {
    uint3 a0 = rows ? uint3(t, row, cascade) : uint3(row, t, cascade);
    uint3 a1 = rows ? uint3(t + N / 2, row, cascade) : uint3(row, t + N / 2, cascade);
    g_buf[0][t] = g_in[a0];
    g_buf[0][t + N / 2] = g_in[a1];
    GroupMemoryBarrierWithGroupSync();
    uint src = 0;
    [unroll] for (uint ns = 1; ns < N; ns <<= 1) {
        float4 v0 = g_buf[src][t], v1 = g_buf[src][t + N / 2];
        uint k = t & (ns - 1);
        float s, c;
        sincos(3.14159265 * k / ns, s, c);   // the inverse transform's twiddle, e^(i 2 pi k / 2ns)
        v1 = float4(CMul(v1.xy, float2(c, s)), CMul(v1.zw, float2(c, s)));
        uint d = (t - k) * 2 + k;
        g_buf[1 - src][d] = v0 + v1;
        g_buf[1 - src][d + ns] = v0 - v1;
        src = 1 - src;
        GroupMemoryBarrierWithGroupSync();
    }
    g_out[a0] = g_buf[src][t];
    g_out[a1] = g_buf[src][t + N / 2];
}

[numthreads(N / 2, 1, 1)]
void CsFftRows(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) { Fft(t.x, g.x, g.y, true); }

[numthreads(N / 2, 1, 1)]
void CsFftCols(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) { Fft(t.x, g.x, g.y, false); }

[numthreads(8, 8, 1)]
void CsResolve(uint3 id : SV_DispatchThreadID) {
    // (the spectrum's index was centred: every other point's sign turned)
    float sign = ((id.x + id.y) & 1) ? -1.0 : 1.0;
    float4 a = g_in[id] * sign, b = g_in2[id] * sign;
    g_slopes[id] = float4(a.xy, a.xy * a.xy);
    g_shape[id] = float4(a.zw, b.xy);
    g_displacement[id] = float4(a.z, b.zw, 0);
}
