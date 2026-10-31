/* mml/water.h: water for a mod's graphics pass (C++, header only, Direct3D 11), as mods/examples/river_water draws the
 * game's river: what is known about the game's water, its surfaces sorted into planes, the wind's ripples (their
 * spectrum on the CPU, their slopes on the GPU each frame) and a reflection's copy with mips. The shaders' side is
 * mml/gfx/water.hlsli (the shading) and mml/gfx/water_waves.hlsli (the ripples' kernels, compiled by the mod's own
 * SHADERS). knowledge/notes/water.md has the findings behind it. */
#pragma once
#include <d3d11.h>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <random>
#include <vector>

namespace mml {
namespace water {

/* ---- the game's water (both builds; knowledge/notes/water.md)
 * The city's river and the other water of the city's model `watercity` are drawn with the animated texture
 * 00TEST01..20.BMP: the river one plane of 54 triangles at y -9.99 under the whole city, a fountain at 2.41. */
constexpr const char *kGameWater = "00test*";
/* The shade the game lays on its water under the bridges: the same model's flat frames watercity.stin01..18, 0.3 m over
 * the river, blended to darken the game's water, with no texture (geom_hide by their frames' names). Over a water drawn
 * anew they show as black and white bands: hide them with the water (the shadow map gives the real shade). */
constexpr const char *kGameShade = "frame:water*.stin*";

/* ---- the water's surfaces, sorted into planes by their level (the river and a fountain share the texture: one mean
 * level put the river's reflection 1.4 m off). Feed it each capture's groups (geom_capture; a capture holds only what
 * the frame drew: ask every few seconds and keep the groups by their key); `tris` is then sorted by plane. */
struct Plane {
    float level;
    unsigned first, count;          /* its triangles in Surfaces::tris */
};
struct Surfaces {
    struct Group { const void *visual, *group; std::vector<float> tris; };
    std::vector<Group> groups;
    std::vector<float> tris;        /* every group's triangles (three points of three floats), sorted by plane */
    std::vector<Plane> planes;
    unsigned version = 0;           /* changes whenever tris does */
    unsigned maxPlanes = 16;

    void Clear() { groups.clear(); tris.clear(); planes.clear(); version++; }
    /* one group of a capture (MmlGeomGroup's visual, group, and its triangles); true when it changed anything */
    bool Add(const void *visual, const void *group, const float *t, unsigned count) {
        Group *g = nullptr;
        for (auto &e : groups)
            if (e.visual == visual && e.group == group) g = &e;
        if (!g) { groups.push_back({visual, group, {}}); g = &groups.back(); }
        if (g->tris.size() == count * 9u && !std::memcmp(g->tris.data(), t, count * 9 * sizeof(float))) return false;
        g->tris.assign(t, t + count * 9);
        return true;
    }
    /* after the groups changed: the planes again (within 5 cm of a level) */
    void Sort() {
        struct Tri { float level; const float *p; };
        std::vector<Plane> ps;
        std::vector<std::vector<const float *>> members;
        for (auto &g : groups)
            for (size_t i = 0; i + 9 <= g.tris.size(); i += 9) {
                float level = (g.tris[i + 1] + g.tris[i + 4] + g.tris[i + 7]) / 3;
                size_t p = 0;
                while (p < ps.size() && std::fabs(ps[p].level - level) > 0.05f) p++;
                if (p == ps.size()) {
                    if (ps.size() == maxPlanes) continue;
                    ps.push_back({level, 0, 0});
                    members.emplace_back();
                }
                members[p].push_back(&g.tris[i]);
            }
        tris.clear();
        for (size_t p = 0; p < ps.size(); p++) {
            ps[p].first = static_cast<unsigned>(tris.size() / 9);
            ps[p].count = static_cast<unsigned>(members[p].size());
            for (const float *t : members[p]) tris.insert(tris.end(), t, t + 9);
        }
        planes = ps;
        version++;
    }
};

/* ---- the wind's ripples: three cascades of kN x kN, each its band of wavenumbers (mml/gfx/water_waves.hlsli) */
constexpr int kN = 256;
constexpr int kCascades = 3;
constexpr float kPatch[kCascades] = {37.3f, 9.17f, 2.31f};   /* metres (no two in a whole ratio: no common repeat) */

struct Spectrum {
    std::vector<float> h0;          /* kCascades x kN x kN x 4: h0(k), conj(h0(-k)) (CsSpectrum's t0) */
    float drawn = 0;                /* the slope variance the cascades draw (both axes) */
    float rest = 0;                 /* the slope variance no cascade draws, an axis (water.hlsli WaterRipples' rest) */
    float peak = 0;                 /* the peak's wavelength (m) */
    float heightVar = 0;            /* the height's variance the cascades draw (m^2): its deviation is a crest's scale */
};

/* JONSWAP for a wind of `wind` m/s over `fetch` m of open water (a river is narrow: its waves are short), spread about
 * the direction the wind comes `from` (degrees from +z towards +x; cos^8 of half the angle). The whole slope variance
 * (both axes) is `variancePerMs` a m/s of wind, 85 % drawn by the cascades and the rest the capillaries' roughness.
 * Cox and Munk's grows by 0.00084 a m/s on a sea covered by slicks, 0.00512 on a clean one, from 0.003 that a sea's
 * swell gives at no wind; a city's river has no swell and a filmed surface: 0.0012 (JONSWAP's open-sea slopes, 0.0275
 * at 3 m/s, turned a river's far bank's reflection into the sky). A wind over `filmBreaks` m/s tears the film up (slicks
 * are seen to go between 4 and 7 m/s): past it the variance grows by `cleanPerMs` more a m/s, a clean sea's growth less
 * the slick's (0.0043), so a strong wind's river is rough and its crests steep (8 m/s: 0.025, against 0.0096). h0 in Tessendorf's convention (its variance half the
 * spectrum's share of the cell: the field's variance is the spectrum's integral). Seeded: the same water each time. */
inline Spectrum MakeSpectrum(float wind, float from, float fetch, float variancePerMs = 0.0012f, float filmBreaks = 4,
                             float cleanPerMs = 0.0043f) {
    Spectrum out;
    out.h0.assign(static_cast<size_t>(kCascades) * kN * kN * 4, 0.0f);
    const double U = wind, F = fetch > 10 ? fetch : 10, g = 9.81, T = 7.4e-5, pi = 3.14159265358979;
    if (U < 0.1) return out;   // no wind: a mirror
    const double wp = 22 * std::pow(g * g / (U * F), 1 / 3.0);   // the peak's frequency
    const double alpha = 0.076 * std::pow(U * U / (F * g), 0.22);
    const double dir = from * pi / 180;
    out.peak = static_cast<float>(2 * pi * g / (wp * wp));
    const int s = 4;
    double norm = 0;
    for (int i = 0; i < 3600; i++) norm += std::pow(std::cos((-pi + (i + 0.5) * 2 * pi / 3600) / 2), 2 * s) * 2 * pi / 3600;
    // (a wave with wavenumber k runs towards -k here: CsSpectrum's phase is k.x + w t)
    auto psi = [&](double kx, double kz) {
        double k = std::sqrt(kx * kx + kz * kz);
        double w = std::sqrt(g * k + T * k * k * k);
        double dwdk = (g + 3 * T * k * k) / (2 * w);
        double sigma = w <= wp ? 0.07 : 0.09;
        double r = std::exp(-(w - wp) * (w - wp) / (2 * sigma * sigma * wp * wp));
        double sw = alpha * g * g / std::pow(w, 5) * std::exp(-1.25 * std::pow(wp / w, 4)) * std::pow(3.3, r);
        double d = std::pow(std::fabs(std::cos((std::atan2(kx, kz) - dir) / 2)), 2 * s) / norm;
        return sw * dwdk * d / k;
    };
    std::mt19937 rng(20261028);
    std::normal_distribution<float> gauss;
    double resolved = 0, heights = 0;
    for (int c = 0; c < kCascades; c++) {
        const double dk = 2 * pi / kPatch[c];
        const double lo = c ? pi * kN / (2 * kPatch[c - 1]) : 0, hi = pi * kN / (2 * kPatch[c]);   // 4 texels a wave
        float *h = &out.h0[static_cast<size_t>(c) * kN * kN * 4];
        for (int y = 0; y < kN; y++)
            for (int x = 0; x < kN; x++) {
                double kx = (x - kN / 2) * dk, kz = (y - kN / 2) * dk, k = std::sqrt(kx * kx + kz * kz);
                float *o = h + (static_cast<size_t>(y) * kN + x) * 4;
                float a = gauss(rng), b = gauss(rng);
                if (k <= 0 || k < lo || k >= hi) continue;
                double p = psi(kx, kz) * dk * dk;
                resolved += k * k * p;
                heights += p;
                double amp = std::sqrt(p / 2) / std::sqrt(2.0);
                o[0] = static_cast<float>(a * amp);
                o[1] = static_cast<float>(b * amp);
            }
        for (int y = 0; y < kN; y++)   // zw: conj(h0(-k)), the index mirrored about the centre
            for (int x = 0; x < kN; x++) {
                const float *m = h + (static_cast<size_t>((kN - y) % kN) * kN + (kN - x) % kN) * 4;
                float *o = h + (static_cast<size_t>(y) * kN + x) * 4;
                o[2] = m[0];
                o[3] = -m[1];
            }
    }
    const double total = variancePerMs * U + cleanPerMs * (U > filmBreaks ? U - filmBreaks : 0);
    const double scale = resolved > 0 ? std::sqrt(0.85 * total / resolved) : 0;
    for (float &a : out.h0) a = static_cast<float>(a * scale);
    out.drawn = static_cast<float>(0.85 * total);
    out.rest = static_cast<float>(0.15 * total / 2);
    out.heightVar = static_cast<float>(heights * scale * scale);
    return out;
}

template <class T> inline void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

/* The ripples on the GPU: Make with the four kernels' bytecode (the mod's SHADERS of water_waves.hlsli), then Update
 * each frame (the spectrum's h0 uploaded when `spectrumVersion` changes): Slopes() and Shape() are the texture arrays
 * mml/gfx/water.hlsli's WaterRipples reads (x, z slope and their squares; the height and the choppy displacement's
 * derivatives dDx/dx, dDz/dz, dDx/dz; both mipmapped). Displacement() holds height/Dx/Dz for tessellated geometry.
 * Two FFTs of two complex fields each; the last pair packs Dx+iDz. */
struct Bytecode { const void *code; size_t size; };
class Waves {
public:
    bool Make(ID3D11Device *d, Bytecode spectrum, Bytecode rows, Bytecode cols, Bytecode resolve) {
        Release();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        made_ = SUCCEEDED(d->CreateComputeShader(spectrum.code, spectrum.size, nullptr, &spectrum_)) &&
                SUCCEEDED(d->CreateComputeShader(rows.code, rows.size, nullptr, &rows_)) &&
                SUCCEEDED(d->CreateComputeShader(cols.code, cols.size, nullptr, &cols_)) &&
                SUCCEEDED(d->CreateComputeShader(resolve.code, resolve.size, nullptr, &resolve_)) &&
                SUCCEEDED(d->CreateBuffer(&bd, nullptr, &cb_));
        for (int i = 0; i < 4 && made_; i++)
            made_ = Grid(d, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, &field_[i].tex, &field_[i].srv, &field_[i].uav, nullptr);
        made_ = made_ && Grid(d, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, &slopes_.tex, &slopes_.srv, &slopes_.uav, nullptr) &&
                Grid(d, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, &shape_.tex, &shape_.srv, &shape_.uav, nullptr) &&
                Grid(d, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, &displacement_.tex, &displacement_.srv, &displacement_.uav, nullptr) &&
                Grid(d, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, &previous_.tex, &previous_.srv, &previous_.uav, nullptr);
        return made_;
    }
    void Update(ID3D11Device *d, ID3D11DeviceContext *ctx, const Spectrum &s, unsigned spectrumVersion, float time) {
        if (!made_) return;
        if (h0Version_ != spectrumVersion || !h0_.srv) {
            h0_.Release();
            if (!Grid(d, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, &h0_.tex, &h0_.srv, nullptr, s.h0.data())) return;
            h0Version_ = spectrumVersion;
        }
        float k[4] = {kPatch[0], kPatch[1], kPatch[2], time};
        ctx->UpdateSubresource(cb_, 0, nullptr, k, 0, 0);
        ctx->CSSetConstantBuffers(0, 1, &cb_);
        // field 0 (the slopes, the height and dDx/dx) and field 2 (dDz/dz, dDx/dz), each through its pair's other
        Run(ctx, spectrum_, {h0_.srv, nullptr, nullptr}, {field_[0].uav, nullptr, nullptr, field_[2].uav}, kN / 8, kN / 8, kCascades);
        for (int f = 0; f < 4; f += 2) {
            Run(ctx, rows_, {nullptr, field_[f].srv, nullptr}, {field_[f + 1].uav, nullptr, nullptr, nullptr}, kN, kCascades, 1);
            Run(ctx, cols_, {nullptr, field_[f + 1].srv, nullptr}, {field_[f].uav, nullptr, nullptr, nullptr}, kN, kCascades, 1);
        }
        std::swap(displacement_, previous_);   // preserve exactly the last rendered wave field for temporal reprojection
        Run(ctx, resolve_, {nullptr, field_[0].srv, field_[2].srv}, {nullptr, slopes_.uav, shape_.uav, nullptr, displacement_.uav}, kN / 8, kN / 8,
            kCascades);
        ctx->CSSetShader(nullptr, nullptr, 0);
        ctx->GenerateMips(slopes_.srv);
        ctx->GenerateMips(shape_.srv);
        ctx->GenerateMips(displacement_.srv);
    }
    ID3D11ShaderResourceView *Slopes() const { return slopes_.srv; }
    ID3D11ShaderResourceView *Shape() const { return shape_.srv; }
    ID3D11ShaderResourceView *Displacement() const { return displacement_.srv; }
    ID3D11ShaderResourceView *PreviousDisplacement() const { return previous_.srv; }
    bool Made() const { return made_; }
    void Release() {
        SafeRelease(spectrum_); SafeRelease(rows_); SafeRelease(cols_); SafeRelease(resolve_); SafeRelease(cb_);
        h0_.Release();
        for (auto &f : field_) f.Release();
        slopes_.Release();
        shape_.Release();
        displacement_.Release();
        previous_.Release();
        made_ = false;
        h0Version_ = 0;
    }
    ~Waves() { Release(); }

private:
    struct Grid3 {
        ID3D11Texture2D *tex = nullptr;
        ID3D11ShaderResourceView *srv = nullptr;
        ID3D11UnorderedAccessView *uav = nullptr;
        void Release() { SafeRelease(uav); SafeRelease(srv); SafeRelease(tex); }
    };
    struct Srvs { ID3D11ShaderResourceView *v[3]; };
    struct Uavs { ID3D11UnorderedAccessView *v[5]; };
    static bool Grid(ID3D11Device *d, DXGI_FORMAT f, unsigned mips, ID3D11Texture2D **tex, ID3D11ShaderResourceView **srv,
                     ID3D11UnorderedAccessView **uav, const float *init) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = kN;
        td.MipLevels = mips;
        td.ArraySize = kCascades;
        td.Format = f;
        td.SampleDesc.Count = 1;
        td.Usage = init ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0) | (mips != 1 ? D3D11_BIND_RENDER_TARGET : 0);
        td.MiscFlags = mips != 1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        D3D11_SUBRESOURCE_DATA sd[kCascades]{};
        for (int c = 0; c < kCascades && init; c++) {
            sd[c].pSysMem = init + static_cast<size_t>(c) * kN * kN * 4;
            sd[c].SysMemPitch = kN * 16;
        }
        if (FAILED(d->CreateTexture2D(&td, init ? sd : nullptr, tex)) || FAILED(d->CreateShaderResourceView(*tex, nullptr, srv))) return false;
        if (!uav) return true;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = f;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray.ArraySize = kCascades;
        return SUCCEEDED(d->CreateUnorderedAccessView(*tex, &ud, uav));
    }
    static void Run(ID3D11DeviceContext *ctx, ID3D11ComputeShader *cs, Srvs in, Uavs out, UINT x, UINT y, UINT z) {
        ID3D11ShaderResourceView *none[3] = {};
        ID3D11UnorderedAccessView *noUav[5] = {};
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetShaderResources(0, 3, in.v);
        ctx->CSSetUnorderedAccessViews(0, 5, out.v, nullptr);
        ctx->Dispatch(x, y, z);
        ctx->CSSetUnorderedAccessViews(0, 5, noUav, nullptr);
        ctx->CSSetShaderResources(0, 3, none);
    }
    ID3D11ComputeShader *spectrum_ = nullptr, *rows_ = nullptr, *cols_ = nullptr, *resolve_ = nullptr;
    ID3D11Buffer *cb_ = nullptr;
    Grid3 h0_, field_[4], slopes_, shape_, displacement_, previous_;
    unsigned h0Version_ = 0;
    bool made_ = false;
};

/* A scene view's picture (MmlGfxView::color_texture) copied with mips each frame it is fresh, read through an sRGB view
 * where its format has one (so mips and the blur of rough water mix light, not display values): water.hlsli's
 * WaterStreakedReflection reads it. Encoded() true: no sRGB view of that format, the shader takes Lin() itself. */
class ReflectionCopy {
public:
    bool Update(ID3D11Device *d, ID3D11DeviceContext *ctx, ID3D11Texture2D *src) {
        if (!src) return false;
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        if (!tex_ || w_ != sd.Width || h_ != sd.Height || from_ != sd.Format) {
            Release();
            DXGI_FORMAT typeless = sd.Format, view = sd.Format;
            if (sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM) typeless = DXGI_FORMAT_B8G8R8A8_TYPELESS, view = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            else if (sd.Format == DXGI_FORMAT_R8G8B8A8_UNORM) typeless = DXGI_FORMAT_R8G8B8A8_TYPELESS, view = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = sd.Width;
            td.Height = sd.Height;
            td.MipLevels = 0;
            td.ArraySize = 1;
            td.Format = typeless;
            td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = view;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = static_cast<UINT>(-1);
            if (FAILED(d->CreateTexture2D(&td, nullptr, &tex_)) || FAILED(d->CreateShaderResourceView(tex_, &vd, &srv_))) { Release(); return false; }
            tex_->GetDesc(&td);
            w_ = sd.Width, h_ = sd.Height, from_ = sd.Format, mips_ = td.MipLevels;
            encoded_ = view == sd.Format;
        }
        ctx->CopySubresourceRegion(tex_, 0, 0, 0, 0, src, 0, nullptr);
        ctx->GenerateMips(srv_);
        return true;
    }
    ID3D11ShaderResourceView *View() const { return srv_; }
    unsigned Width() const { return w_; }
    unsigned Height() const { return h_; }
    unsigned Mips() const { return mips_; }
    bool Encoded() const { return encoded_; }
    void Release() { SafeRelease(srv_); SafeRelease(tex_); w_ = h_ = mips_ = 0; }
    ~ReflectionCopy() { Release(); }

private:
    ID3D11Texture2D *tex_ = nullptr;
    ID3D11ShaderResourceView *srv_ = nullptr;
    unsigned w_ = 0, h_ = 0, mips_ = 0;
    DXGI_FORMAT from_ = DXGI_FORMAT_UNKNOWN;
    bool encoded_ = false;
};

/* A planar reflection's camera (MmlViewFn's MmlViewCamera rows, mml.h): the camera mirrored in the plane y = level,
 * only what is above it drawn: the clip plane the water's own. Not over it: one 2 cm over the water cut the foot of
 * every wall, rock and hull out, and close to the water, seen at a grazing angle, the reflection camera looked through
 * that cut at what lay behind (a band of houses and sky between a thing and its reflection). Not under it either: the
 * game's models end at the water (the city's rocks and bridges at -10.00 against the river's -9.99), and under them
 * the reflection camera saw the sky's backdrop, which the plane does not cut (a dashed light line at the waterline).
 * The pixels at a thing's cut foot are covered in part, the backdrop showing through where their centres pass under
 * the foot: water.hlsli's WaterFollowReflection fills those holes with the foot beside them. The image comes out
 * mirrored; read it by its own view x projection (MmlGfxView::view, proj: water.hlsli WaterReflectionUv). */
inline void MirrorCamera(float world[16], float clip[4], float level) {
    world[13] = 2 * level - world[13];
    world[1] = -world[1];
    world[5] = -world[5];
    world[9] = -world[9];
    clip[0] = 0;
    clip[1] = 1;
    clip[2] = 0;
    clip[3] = -level;
}

} // namespace water
} // namespace mml
