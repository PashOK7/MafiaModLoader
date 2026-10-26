// Shaders on the game's own drawing (mml.h shader_add, shader_remove; docs/MODDING.md "Shaders on the game's own
// drawing"; sdk/include/mml/shaders/game.hlsli the contract).
//
// The game draws with Direct3D 8's fixed pipeline (no shaders of its own), through a wrapper's Direct3D 9 in the
// labs. LS3D draws every mesh face group by face group through three functions (host/geom.cpp hooks them: objects,
// lightmapped objects, billboards), each setting its device states (textures, stages, lights, the world transform)
// and calling DrawIndexedPrimitive. A rule of a mod's names the draws it takes (its textures' names, the kind, the
// draw's class: opaque, alpha-tested, translucent, additive); at such a draw the host sets the rule's Direct3D 9
// vertex and pixel shaders and the frame's constants (game.hlsli's registers, the same in both stages) on the game's
// device, lets the draw go, and puts the game's (none) back. A shader given as HLSL is compiled by d3dcompiler_47
// (Windows' own), `#include "mml/game.hlsli"` answered from the host's copy of the header.
//
// shader.survey records the device states of one frame's draws by kind (what the reference shaders must do).
//
// Bridge methods: shader.add, shader.remove, shader.state, shader.survey.
#include "host.h"
#include "d3dmirror.h"
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>
#include "json.hpp"
#include "shaders_hlsli.inc"   // (kGameHlsli: sdk/include/mml/shaders/game.hlsli, made by the build)

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kGroupMaterial = 0x04, kGroupFaces = 0x08, kMaterialFlags = 0x08, kMaterialTexture = 0x0C, kTextureName = 0x1C;
constexpr int kEntryGroup = 0x10;

template <class T> T At(const void *p, int off) { return *reinterpret_cast<const T *>(static_cast<const uint8_t *>(p) + off); }

std::string Lower(const char *s) {
    std::string o;
    for (; s && *s; s++) o += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
    return o;
}
bool Glob(const char *p, const char *s) {
    const char *star = nullptr, *after = nullptr;
    while (*s) {
        if (*p == '?' || *p == *s) { p++; s++; continue; }
        if (*p == '*') { star = p++; after = s; continue; }
        if (star) { p = star + 1; s = ++after; continue; }
        return false;
    }
    while (*p == '*') p++;
    return !*p;
}

// ---- the group being drawn (geom.cpp tells it)
struct Group {
    bool active = false;
    int kind = 0, where = 0, pass = 0;
    const void *visual = nullptr, *group = nullptr, *material = nullptr;
    const char *texture = nullptr;
    uint32_t matFlags = 0;
    unsigned faces = 0;
};
Group g_group;

// ---- rules
struct Rule {
    MmlMod *mod = nullptr;
    int handle = 0;
    std::string pattern;            // lower case
    uint32_t kinds = 0, classes = 0, where = 0, flags = 0;
    std::vector<uint8_t> vsCode, psCode;
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DDevice9 *device = nullptr;   // what vs and ps were made on
    std::string error;
    unsigned drawsNow = 0, drawsLast = 0;
    float consts[64 * 4] = {};       // the mod's own registers c160..c223 (shader_constants), set at each draw it takes
    int constLo = 0, constHi = 0;    // (the registers given: [lo, hi) of 0..64)
    // the float registers each of its shaders reads (a bit a register, c0..c255; from the code's constant table):
    // only those are written at its draws; a stage it leaves to the game reads none
    uint32_t vsReads[8], psReads[8];
    bool readsLights = true, readsShadows = true, readsNear = true;   // (their union's groups: Constants)
};

// The float registers a Direct3D 9 shader's code reads, from the constant table the compiler leaves in it (a comment
// token 'CTAB': D3DXSHADER_CONSTANTTABLE, its D3DXSHADER_CONSTANTINFO records: RegisterSet 2 is float4, then the index
// and the count). False (every register taken as read) when the code has no such table.
bool ReadsOf(const std::vector<uint8_t> &code, uint32_t out[8]) {
    memset(out, 0xFF, 32);
    if (code.size() < 8) return false;
    const auto *t = reinterpret_cast<const uint32_t *>(code.data());
    const size_t n = code.size() / 4;
    for (size_t i = 1; i < n;) {
        const uint32_t tok = t[i];
        if ((tok & 0xFFFF) != 0xFFFE) break;   // (the comments come first, after the version)
        const size_t len = tok >> 16;
        if (i + 1 + len > n) break;
        if (len >= 8 && t[i + 1] == 0x42415443u) {   // 'CTAB'
            const auto *base = reinterpret_cast<const uint8_t *>(&t[i + 2]);
            const size_t bytes = (len - 1) * 4;
            const uint32_t count = *reinterpret_cast<const uint32_t *>(base + 12), info = *reinterpret_cast<const uint32_t *>(base + 16);
            if (info + size_t(count) * 20 > bytes) return false;
            memset(out, 0, 32);
            for (uint32_t k = 0; k < count; k++) {
                const uint8_t *ci = base + info + k * 20;
                const uint16_t set = *reinterpret_cast<const uint16_t *>(ci + 4), reg = *reinterpret_cast<const uint16_t *>(ci + 6),
                               cnt = *reinterpret_cast<const uint16_t *>(ci + 8);
                if (set != 2) continue;   // (D3DXRS_FLOAT4)
                for (uint32_t r = reg; r < uint32_t(reg) + cnt && r < 256; r++) out[r >> 5] |= 1u << (r & 31);
            }
            return true;
        }
        i += 1 + len;
    }
    return false;
}
// Whether a shader reads any register of [from, to) (`reads`: a bit a register).
bool Reads(const uint32_t *reads, int from, int to) {
    for (int r = from; r < to; r++) if (reads[r >> 5] >> (r & 31) & 1) return true;
    return false;
}
std::vector<std::unique_ptr<Rule>> g_rules;
int g_nextHandle = 1;
unsigned g_faults;
double g_t0;

// ---- the survey
int g_surveyWanted, g_nextSurvey = 1;
struct Combo { unsigned draws = 0, faces = 0; std::vector<std::string> textures; json states; };
std::map<std::string, Combo> g_combos;
struct SurveyDone { int id; json result; };
std::vector<SurveyDone> g_surveys;

const char *TextureName(const void *mat) {
    const void *tex = mat ? At<const void *>(mat, kMaterialTexture) : nullptr;
    const char *n = tex ? At<const char *>(tex, kTextureName) : nullptr;
    return n && *n ? n : nullptr;
}

// The class of a draw by its blending: 1 opaque, 2 alpha-tested, 4 translucent, 8 additive.
uint32_t ClassOf(const D3dMirror &m) {
    if (m.rs[D3DRS_ALPHABLENDENABLE]) return m.rs[D3DRS_DESTBLEND] == D3DBLEND_ONE ? MML_SHADE_ADDITIVE : MML_SHADE_TRANSLUCENT;
    return m.rs[D3DRS_ALPHATESTENABLE] ? MML_SHADE_ALPHATEST : MML_SHADE_OPAQUE;
}

D3dMirror g_scratch;   // (StateOf's without the mirror: game thread)
float g_frameSeconds;   // the time the shaders get (game.hlsli mml_Camera.w), taken once a frame

// The state a draw's work reads: the mirror's (d3dmirror.cpp), or, without it, what is read asked of the device into
// `scratch` (the transforms, the render states, stages 0 and 1, their textures, the material, the lights, the viewport,
// the shaders, sampler 2).
const D3dMirror &StateOf(IDirect3DDevice9 *d, D3dMirror *scratch) {
    if (const D3dMirror *m = MirrorOf(d)) return *m;
    D3dMirror &m = *scratch;
    d->GetTransform(D3DTS_WORLD, &m.world);
    d->GetTransform(D3DTS_VIEW, &m.view);
    d->GetTransform(D3DTS_PROJECTION, &m.proj);
    for (D3DRENDERSTATETYPE s : {D3DRS_FOGENABLE, D3DRS_FOGTABLEMODE, D3DRS_FOGVERTEXMODE, D3DRS_FOGSTART, D3DRS_FOGEND,
                                 D3DRS_FOGDENSITY, D3DRS_FOGCOLOR, D3DRS_AMBIENT, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF,
                                 D3DRS_LIGHTING, D3DRS_RANGEFOGENABLE, D3DRS_TEXTUREFACTOR, D3DRS_ALPHABLENDENABLE,
                                 D3DRS_SRCBLEND, D3DRS_DESTBLEND})
        d->GetRenderState(s, &m.rs[s]);
    for (DWORD s = 0; s < 3; s++) {
        d->GetTextureStageState(s, D3DTSS_COLOROP, &m.tss[s][D3DTSS_COLOROP]);
        d->GetTextureStageState(s, D3DTSS_ALPHAOP, &m.tss[s][D3DTSS_ALPHAOP]);
        d->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &m.tss[s][D3DTSS_TEXCOORDINDEX]);
        IDirect3DBaseTexture9 *t = nullptr;
        d->GetTexture(s, &t);
        m.tex[s] = t;
        if (t) t->Release();   // (the device keeps it: compared and set back only)
    }
    for (DWORD s = 1; s < 14; s++) d->GetSamplerState(2, static_cast<D3DSAMPLERSTATETYPE>(s), &m.samp[2][s]);
    d->GetMaterial(&m.material);
    for (DWORD i = 0; i < 8; i++) {
        BOOL on = FALSE;
        m.lightOn[i] = SUCCEEDED(d->GetLightEnable(i, &on)) && on;
        if (m.lightOn[i]) d->GetLight(i, &m.light[i]);
    }
    d->GetViewport(&m.viewport);
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    d->GetVertexShader(&vs);
    d->GetPixelShader(&ps);
    m.vs = vs;
    m.ps = ps;
    if (vs) vs->Release();
    if (ps) ps->Release();
    return m;
}

const char *OpName(DWORD op) {
    static const char *n[] = {"?", "disable", "selectarg1", "selectarg2", "modulate", "modulate2x", "modulate4x", "add", "addsigned",
                              "addsigned2x", "subtract", "addsmooth", "blenddiffusealpha", "blendtexturealpha", "blendfactoralpha",
                              "blendtexturealphapm", "blendcurrentalpha", "premodulate", "modulatealpha_addcolor", "modulatecolor_addalpha",
                              "modulateinvalpha_addcolor", "modulateinvcolor_addalpha", "bumpenvmap", "bumpenvmapluminance", "dotproduct3",
                              "multiplyadd", "lerp"};
    return op < sizeof n / sizeof *n ? n[op] : "?";
}
std::string ArgName(DWORD a) {
    static const char *n[] = {"diffuse", "current", "texture", "tfactor", "specular", "temp", "constant"};
    std::string s = (a & 0xF) < 7 ? n[a & 0xF] : "?";
    if (a & D3DTA_COMPLEMENT) s += "|complement";
    if (a & D3DTA_ALPHAREPLICATE) s += "|alpha";
    return s;
}

void Survey(IDirect3DDevice9 *d, UINT prims) {
    json st;
    DWORD v = 0;
    d->GetFVF(&v);
    char fvf[16];
    snprintf(fvf, sizeof fvf, "0x%X", static_cast<unsigned>(v));
    st["fvf"] = fvf;
    struct { D3DRENDERSTATETYPE s; const char *n; } rs[] = {
        {D3DRS_LIGHTING, "lighting"}, {D3DRS_FOGENABLE, "fog"}, {D3DRS_FOGTABLEMODE, "fog_table"}, {D3DRS_FOGVERTEXMODE, "fog_vertex"},
        {D3DRS_ALPHATESTENABLE, "alpha_test"}, {D3DRS_ALPHAREF, "alpha_ref"}, {D3DRS_ALPHAFUNC, "alpha_func"},
        {D3DRS_ALPHABLENDENABLE, "blend"}, {D3DRS_SRCBLEND, "src"}, {D3DRS_DESTBLEND, "dst"}, {D3DRS_ZWRITEENABLE, "zwrite"},
        {D3DRS_CULLMODE, "cull"}, {D3DRS_COLORVERTEX, "color_vertex"}, {D3DRS_DIFFUSEMATERIALSOURCE, "diffuse_source"},
        {D3DRS_AMBIENTMATERIALSOURCE, "ambient_source"}, {D3DRS_SPECULARENABLE, "specular"}, {D3DRS_NORMALIZENORMALS, "normalize"},
        {D3DRS_VERTEXBLEND, "vertex_blend"}, {D3DRS_INDEXEDVERTEXBLENDENABLE, "indexed_blend"}, {D3DRS_TEXTUREFACTOR, "tfactor"},
    };
    for (auto &r : rs) {
        d->GetRenderState(r.s, &v);
        st[r.n] = v;
    }
    json stages = json::array();
    for (DWORD s = 0; s < 3; s++) {
        DWORD cop = 0, a1 = 0, a2 = 0, aop = 0, aa1 = 0, aa2 = 0, tci = 0, ttf = 0;
        d->GetTextureStageState(s, D3DTSS_COLOROP, &cop);
        if (cop == D3DTOP_DISABLE) break;
        d->GetTextureStageState(s, D3DTSS_COLORARG1, &a1);
        d->GetTextureStageState(s, D3DTSS_COLORARG2, &a2);
        d->GetTextureStageState(s, D3DTSS_ALPHAOP, &aop);
        d->GetTextureStageState(s, D3DTSS_ALPHAARG1, &aa1);
        d->GetTextureStageState(s, D3DTSS_ALPHAARG2, &aa2);
        d->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &tci);
        d->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf);
        IDirect3DBaseTexture9 *t = nullptr;
        d->GetTexture(s, &t);
        stages.push_back({{"color", std::string(OpName(cop)) + "(" + ArgName(a1) + ", " + ArgName(a2) + ")"},
                          {"alpha", std::string(OpName(aop)) + "(" + ArgName(aa1) + ", " + ArgName(aa2) + ")"},
                          {"texcoord", tci}, {"transform", ttf}, {"texture", t != nullptr}});
        if (t) t->Release();
    }
    st["stages"] = stages;
    json lights = json::array();
    for (DWORD i = 0; i < 8; i++) {
        BOOL on = FALSE;
        if (FAILED(d->GetLightEnable(i, &on)) || !on) continue;
        D3DLIGHT9 l{};
        d->GetLight(i, &l);
        lights.push_back(l.Type == D3DLIGHT_POINT ? "point" : l.Type == D3DLIGHT_SPOT ? "spot" : "directional");
    }
    st["lights"] = lights;
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    d->GetVertexShader(&vs);
    d->GetPixelShader(&ps);
    st["shaders_set"] = vs != nullptr || ps != nullptr;
    if (vs) vs->Release();
    if (ps) ps->Release();
    st["kind"] = g_group.kind == 1 ? "object" : g_group.kind == 2 ? "lightmapped" : "billboard";
    st["pass"] = g_group.pass;
    std::string key = st.dump(-1, ' ', false, json::error_handler_t::replace);
    Combo &c = g_combos[key];
    if (!c.draws) c.states = st;
    c.draws++;
    c.faces += prims;
    if (c.textures.size() < 4 && g_group.texture) {
        std::string t = g_group.texture;
        if (std::find(c.textures.begin(), c.textures.end(), t) == c.textures.end()) c.textures.push_back(t);
    }
}

// ---- the constants (game.hlsli)
constexpr int kRegs = 117;   // (c0..c75 here, c76..c116 shadows.cpp's)
void Mul(const D3DMATRIX &a, const D3DMATRIX &b, D3DMATRIX *o) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) o->m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
}
void Rgba(DWORD x, float *o) {
    o[0] = ((x >> 16) & 0xFF) / 255.0f; o[1] = ((x >> 8) & 0xFF) / 255.0f; o[2] = (x & 0xFF) / 255.0f; o[3] = ((x >> 24) & 0xFF) / 255.0f;
}
float AsFloat(DWORD v) { float f; memcpy(&f, &v, 4); return f; }
// c0..c75 both kinds of constants share: the transforms, the eye, the time, the fog
void CommonConstants(const D3dMirror &s, float *c) {
    const D3DMATRIX &w = s.world, &v = s.view, &p = s.proj;
    D3DMATRIX vp, wvp;
    Mul(v, p, &vp);
    Mul(w, vp, &wvp);
    memcpy(c + 0, &w, 64);
    memcpy(c + 16, &v, 64);
    memcpy(c + 32, &p, 64);
    memcpy(c + 48, &wvp, 64);
    // the eye: -t R^T of the view (its rows' rotation is orthonormal)
    float tx = v._41, ty = v._42, tz = v._43;
    c[64] = -(tx * v._11 + ty * v._12 + tz * v._13);
    c[65] = -(tx * v._21 + ty * v._22 + tz * v._23);
    c[66] = -(tx * v._31 + ty * v._32 + tz * v._33);
    c[67] = g_frameSeconds;
    const DWORD fog = s.rs[D3DRS_FOGENABLE], table = s.rs[D3DRS_FOGTABLEMODE], vert = s.rs[D3DRS_FOGVERTEXMODE];
    const DWORD mode = table != D3DFOG_NONE ? table : vert;
    c[68] = AsFloat(s.rs[D3DRS_FOGSTART]); c[69] = AsFloat(s.rs[D3DRS_FOGEND]); c[70] = AsFloat(s.rs[D3DRS_FOGDENSITY]);
    c[71] = fog ? static_cast<float>(mode) : 0.0f;
    Rgba(s.rs[D3DRS_FOGCOLOR], c + 72);
    c[75] = fog ? 1.0f : 0.0f;
}
// Every register of game.hlsli for a rule's draw; the groups its shaders do not read left out (`lights`: the
// Direct3D lights c27..c74; `shadows`: the shadow map's c76..c83, `nearLights` the game's lights c84..c116). What is
// not written there is not read.
// What the last full making was made from (Constants reuses its registers while it holds): the mirror's serial, the
// groups, the draw's kind and pass.
struct ConstKey { const D3dMirror *state; uint32_t serial; bool lights, shadows, nearLights; int kind, pass; };
ConstKey g_constKey;
D3DMATRIX g_viewProj;
float g_c[kRegs * 4 + 4];   // the registers of the draws (kept: Constants reuses them while its key holds)
// True when only the world, the textures' flags and (nearLights) the lights near were made anew: the rest is the last
// draw's.
bool Constants(const D3dMirror &s, float *c, bool lights, bool shadows, bool nearLights) {
    const ConstKey key{&s, s.serial, lights, shadows, nearLights, g_group.kind, g_group.pass};
    const ConstKey &k = g_constKey;
    if (&s != &g_scratch && s.serial && k.state == key.state && k.serial == key.serial && k.lights == lights &&
        k.shadows == shadows && k.nearLights == nearLights && k.kind == key.kind && k.pass == key.pass) {
        // the same state but the world and the textures: those registers alone (c is the last draw's)
        const D3DMATRIX &w = s.world;
        D3DMATRIX wvp;
        Mul(w, g_viewProj, &wvp);
        memcpy(c + 0, &w, 64);
        memcpy(c + 48, &wvp, 64);
        c[99] = s.tex[0] ? 1.0f : 0.0f;
        c[103] = s.tex[1] ? 1.0f : 0.0f;
        if (nearLights) ShadowsConstants(g_group.visual, c, true);
        return true;
    }
    g_constKey = key;
    CommonConstants(s, c);
    Mul(s.view, s.proj, &g_viewProj);   // (kept for the next draws)
    memcpy(c + 76, &s.material.Diffuse, 16);
    memcpy(c + 80, &s.material.Ambient, 16);
    memcpy(c + 84, &s.material.Emissive, 16);
    Rgba(s.rs[D3DRS_AMBIENT], c + 88);
    c[92] = static_cast<float>(g_group.kind);
    c[93] = s.rs[D3DRS_LIGHTING] ? 1.0f : 0.0f;
    c[94] = s.rs[D3DRS_ALPHATESTENABLE] ? (s.rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f : -1.0f;
    c[95] = static_cast<float>(g_group.pass);
    for (DWORD i = 0; i < 2; i++) {
        float *o = c + 96 + i * 4;
        o[0] = static_cast<float>(s.tss[i][D3DTSS_COLOROP]); o[1] = static_cast<float>(s.tss[i][D3DTSS_ALPHAOP]);
        o[2] = static_cast<float>(s.tss[i][D3DTSS_TEXCOORDINDEX] & 0xFFFF); o[3] = s.tex[i] ? 1.0f : 0.0f;
    }
    const D3DVIEWPORT9 &vpo = s.viewport;
    int n = 0;
    if (lights) memset(c + 108, 0, 8 * 24 * 4);   // (the slots past the lights on: zeros, as they are compared)
    for (DWORD i = 0; i < 8 && n < 8 && lights; i++) {   // (the lights only for a shader that reads them)
        if (!s.lightOn[i]) continue;
        const D3DLIGHT9 &l = s.light[i];
        float *o = c + 108 + n * 24;
        o[0] = l.Position.x; o[1] = l.Position.y; o[2] = l.Position.z; o[3] = static_cast<float>(l.Type);
        o[4] = l.Direction.x; o[5] = l.Direction.y; o[6] = l.Direction.z; o[7] = l.Range;
        memcpy(o + 8, &l.Diffuse, 16);
        memcpy(o + 12, &l.Ambient, 16);
        o[16] = l.Attenuation0; o[17] = l.Attenuation1; o[18] = l.Attenuation2; o[19] = l.Falloff;
        o[20] = std::cos(l.Theta / 2); o[21] = std::cos(l.Phi / 2);
        n++;
    }
    Rgba(s.rs[D3DRS_TEXTUREFACTOR], c + 300);
    c[104] = static_cast<float>(n);
    c[105] = s.rs[D3DRS_RANGEFOGENABLE] ? 1.0f : 0.0f;
    c[106] = static_cast<float>(vpo.Width);
    c[107] = static_cast<float>(vpo.Height);
    if (shadows || nearLights) ShadowsConstants(g_group.visual, c, nearLights);
    return false;
}

// What the host's own skinned shaders read of game.hlsli (VsSkin, PsSkin: the transforms, the eye, the fog, the
// draw, the stages, the viewport, the texture factor), c0..c26 and c75: a person's draw no rule takes writes these
// alone (there are hundreds of such draws a frame).
void CoreConstants(const D3dMirror &s, float *c) {
    g_constKey = {};   // (the shared registers written otherwise: Constants makes them whole next)
    memset(c, 0, 27 * 16);
    CommonConstants(s, c);
    c[92] = static_cast<float>(g_group.kind);
    c[94] = s.rs[D3DRS_ALPHATESTENABLE] ? (s.rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f : -1.0f;
    c[95] = static_cast<float>(g_group.pass);
    for (DWORD i = 0; i < 2; i++) {
        float *o = c + 96 + i * 4;
        const DWORD cop = s.tss[i][D3DTSS_COLOROP];
        o[0] = static_cast<float>(cop);
        if (cop == D3DTOP_DISABLE) continue;   // (a stage off: nothing more of it is read)
        o[1] = static_cast<float>(s.tss[i][D3DTSS_ALPHAOP]); o[2] = static_cast<float>(s.tss[i][D3DTSS_TEXCOORDINDEX] & 0xFFFF);
        o[3] = s.tex[i] ? 1.0f : 0.0f;
    }
    c[105] = s.rs[D3DRS_FOGENABLE] && s.rs[D3DRS_RANGEFOGENABLE] ? 1.0f : 0.0f;
    c[106] = static_cast<float>(s.viewport.Width);
    c[107] = static_cast<float>(s.viewport.Height);
    Rgba(s.rs[D3DRS_TEXTUREFACTOR], c + 300);
}

bool Make(Rule &r, IDirect3DDevice9 *d) {
    if (r.device == d && (r.vs || r.vsCode.empty()) && (r.ps || r.psCode.empty())) return true;
    if (r.vs) { r.vs->Release(); r.vs = nullptr; }
    if (r.ps) { r.ps->Release(); r.ps = nullptr; }
    r.device = d;
    if (!r.vsCode.empty() && FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(r.vsCode.data()), &r.vs))) {
        r.error = "the device refused its vertex shader";
        return false;
    }
    if (!r.psCode.empty() && FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(r.psCode.data()), &r.ps))) {
        r.error = "the device refused its pixel shader";
        return false;
    }
    return true;
}

Rule *RuleFor(const D3dMirror &st) {
    uint32_t cls = 0;
    std::string name;
    for (auto it = g_rules.rbegin(); it != g_rules.rend(); ++it) {   // (the last added first)
        Rule &r = **it;
        if (!r.error.empty()) continue;
        if (r.kinds && !(r.kinds & (1u << (g_group.kind - 1)))) continue;
        if (r.where && !(r.where & static_cast<uint32_t>(g_group.where))) continue;
        if (g_group.pass > 0 && !(r.flags & MML_SHADE_ALL_PASSES)) continue;
        if (!cls) cls = ClassOf(st);
        if (r.classes && !(r.classes & cls)) continue;
        if (r.pattern != "*") {
            if (name.empty()) name = Lower(g_group.texture ? g_group.texture : "");
            if (!Glob(r.pattern.c_str(), name.c_str())) continue;
        }
        return &r;
    }
    return nullptr;
}

using DipFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
bool Compile(const std::string &src, const char *entry, const char *target, std::vector<uint8_t> *out, std::string *err);

// ---- the scene drawn from the sun (shadows.cpp, view.cpp ViewDrawShadow): every face group into the map as its
// depth, cut out where the game's alpha test cuts it; what lets light through (blended without a test, added) and the
// backdrop (the sky: drawn with the camera's view without its position) left out
const char kShadowHlsl[] = R"(
#include "mml/game.hlsli"
struct I { float3 pos : POSITION; float4 color : COLOR0; float2 uv0 : TEXCOORD0; float2 uv1 : TEXCOORD1; };
struct O { float4 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float z : TEXCOORD1; };
O vs(I i) {
    O o;
    o.pos = mul(mul(mul(float4(i.pos, 1), mml_World), mml_View), mml_Proj);
    o.color = i.color;
    o.uv = mml_Stage0.z > 0.5 ? i.uv1 : i.uv0;
    o.z = o.pos.z / o.pos.w;
    return o;
}
float4 ps(O i) : COLOR0 {
    float a = mml_FixedStage(mml_Stage0, tex2D(mml_Tex0, i.uv), i.color, i.color).a;   // (the game's alpha test cuts on it)
    // a slope-scaled bias: the depth's own change across a texel added, so the receivers' constant bias stays small
    float slope = max(abs(ddx(i.z)), abs(ddy(i.z)));
    return float4(i.z + slope * 1.5, 0, 0, a);
}
)";
IDirect3DVertexShader9 *g_shadowVs;
IDirect3DPixelShader9 *g_shadowPs;
IDirect3DDevice9 *g_shadowDev;
bool g_shadowFailed;
std::string g_shadowWhy;
float g_shadowBack[16];       // the backdrop's view for this drawing
unsigned g_shadowDraws, g_shadowFaces, g_shadowSkipped, g_shadowCut;
float g_shadowVP[16];          // the world to the map as the drawing set it (its first world draw: View x Projection)
bool g_shadowVPSet;

bool MakeShadowShaders(IDirect3DDevice9 *d) {
    if (g_shadowDev == d && g_shadowVs && g_shadowPs) return true;
    if (g_shadowFailed && g_shadowDev == d) return false;
    if (g_shadowDev != d) { g_shadowVs = nullptr; g_shadowPs = nullptr; }   // (another device: the old one's went with it)
    g_shadowDev = d;
    std::vector<uint8_t> vs, ps;
    std::string err;
    if (!Compile(kShadowHlsl, "vs", "vs_3_0", &vs, &err) || !Compile(kShadowHlsl, "ps", "ps_3_0", &ps, &err) ||
        FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD *>(vs.data()), &g_shadowVs)) ||
        FAILED(d->CreatePixelShader(reinterpret_cast<const DWORD *>(ps.data()), &g_shadowPs))) {
        g_shadowWhy = err.empty() ? "the device refused the depth shaders" : err;
        g_shadowFailed = true;
        return false;
    }
    return true;
}

// a person skinned on the GPU (skin.cpp): its vertex shader in place of the draw's vertices; the pixel shader as given
HRESULT SkinnedDip(IDirect3DDevice9 *d, const SkinGpuDraw &s, IDirect3DVertexShader9 *vs, IDirect3DPixelShader9 *ps, D3DPRIMITIVETYPE t,
                   UINT minIndex, UINT verts, UINT start, UINT n, DipFn dip) {
    SkinGpuSaved k{};
    SkinGpuBind(d, s, &k);
    if (!MirrorLend(d, vs, ps)) {   // (lent: they stay bound; else set here, put back by the caller)
        d->SetVertexShader(vs);
        d->SetPixelShader(ps);
    }
    HRESULT hr = dip(d, t, 0, minIndex, verts, start, n);   // (the LOD's vertices from 0 in the skin's buffers)
    SkinGpuUnbind(d, k);
    return hr;
}

bool ShadowDraw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts, UINT start, UINT n, DipFn dip, HRESULT *hr) {
    const D3dMirror &st = StateOf(d, &g_scratch);
    const DWORD blend = st.rs[D3DRS_ALPHABLENDENABLE], test = st.rs[D3DRS_ALPHATESTENABLE], dst = st.rs[D3DRS_DESTBLEND];
    const D3DMATRIX v = st.view;
    bool sky = !memcmp(&v, g_shadowBack, sizeof g_shadowBack);
    if (sky || (blend && (!test || dst == D3DBLEND_ONE))) { g_shadowSkipped++; *hr = S_OK; return true; }   // (left out: no light stopped)
    if (!MakeShadowShaders(d)) { *hr = S_OK; return true; }
    if (!g_shadowVPSet) {   // (what the map is read with is what it was drawn with)
        D3DMATRIX p{};
        d->GetTransform(D3DTS_PROJECTION, &p);
        Mul(v, p, reinterpret_cast<D3DMATRIX *>(g_shadowVP));
        g_shadowVPSet = true;
    }
    float *const c = g_c;
    Constants(st, c, true, true, true);
    IDirect3DVertexShader9 *const oldVs = st.vs;   // (the device holds them: set back, not released)
    IDirect3DPixelShader9 *const oldPs = st.ps;
    MirrorConstants(d, false, 0, c, kRegs);
    MirrorConstants(d, true, 0, c, kRegs);
    SkinGpuDraw skin;
    const bool skinned = SkinGpuFor(d, g_group.visual, base, &skin);
    const bool lent = !skinned && MirrorLend(d, g_shadowVs, g_shadowPs);
    if (!lent && !skinned) {
        d->SetVertexShader(g_shadowVs);
        d->SetPixelShader(g_shadowPs);
    }
    if (blend) { d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); g_shadowCut++; }   // (a fence's cut-outs: tested, not blended)
    if (skinned)
        *hr = SkinnedDip(d, skin, static_cast<IDirect3DVertexShader9 *>(SkinGpuShader(2)), g_shadowPs, t, minIndex, verts, start, n, dip);
    else
        *hr = dip(d, t, base, minIndex, verts, start, n);
    if (blend) d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);   // (as LS3D left it: its own cache of the states holds)
    if (!lent && !MirrorOf(d)) {   // (no mirror: the game's put back at once)
        d->SetVertexShader(oldVs);
        d->SetPixelShader(oldPs);
    }
    g_shadowDraws++;
    g_shadowFaces += n;
    return true;
}

bool Draw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts, UINT start, UINT n, DipFn dip, HRESULT *hr) {
    if (ShadowViewNow()) return ShadowDraw(d, t, base, minIndex, verts, start, n, dip, hr);
    if (g_surveyWanted && g_group.where == MML_GEOM_FRAME) Survey(d, n);
    SkinGpuDraw skin;
    const bool skinned = g_group.kind == 1 && SkinGpuFor(d, g_group.visual, base, &skin);
    if (g_rules.empty() && !skinned) return false;
    MirrorCheckDraw(d);   // (d3d.mirror's check, when asked)
    const D3dMirror &st = StateOf(d, &g_scratch);
    Rule *r = g_rules.empty() || !g_group.where ? nullptr : RuleFor(st);   // (the rules: the frame's and the views' draws)
    if (r && !Make(*r, d)) r = nullptr;
    if (!r && !skinned) return false;
    float *const c = g_c;
    // the registers to compare: the shaders' reads; only the ones a draw changes when the rule, the state and the
    // device's constants are as at the last draw (what was written then is there)
    static const Rule *lastRule;
    static uint32_t lastConstSerial;
    bool sameAsLast = false;
    bool all = false;
    if (r) {
        all = (skinned && !(r->vs && (r->flags & MML_SHADE_SKINNED))) || !r->vs || !r->ps;   // (a shader not the rule's: all)
        const bool fast = Constants(st, c, all || r->readsLights, all || r->readsShadows, all || r->readsNear);
        sameAsLast = fast && !all && r == lastRule && lastConstSerial == MirrorConstSerial();
    }
    else CoreConstants(st, c);   // (a person no rule takes: the host's shaders read these alone)
    IDirect3DVertexShader9 *const oldVs = st.vs;   // (the device holds them: set back, not released)
    IDirect3DPixelShader9 *const oldPs = st.ps;
    // only the registers that differ from what the device holds (MirrorConstants)
    if (r) {
        static const uint32_t kAll[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
        // (what a draw on the same state changes: the world c0..c3, its product c12..c15, the stages' textures c24, c25,
        // the game's lights near it c84..c116)
        static const uint32_t kDraw[8] = {0x0300F00Fu, 0, 0xFFF00000u, 0x001FFFFFu, 0, 0, 0, 0};
        uint32_t vsMask[8], psMask[8];
        const bool hostVs = skinned && !(r->vs && (r->flags & MML_SHADE_SKINNED));   // (the host's skinning shader: reads them all)
        for (int i = 0; i < 8; i++) {
            vsMask[i] = (hostVs ? kAll[i] : r->vsReads[i]) & (sameAsLast ? kDraw[i] : ~0u);
            psMask[i] = (r->ps ? r->psReads[i] : kAll[i]) & (sameAsLast ? kDraw[i] : ~0u);
        }
        MirrorConstants(d, false, 0, c, kRegs, vsMask);
        MirrorConstants(d, true, 0, c, kRegs, psMask);
    } else {
        MirrorConstants(d, false, 0, c, 27);
        MirrorConstants(d, true, 0, c, 27);
        MirrorConstants(d, true, 75, c + 300, 1);
    }
    if (r && r->constHi > r->constLo) {   // (the mod's own registers)
        MirrorConstants(d, false, 160 + r->constLo, r->consts + r->constLo * 4, r->constHi - r->constLo, r->vsReads);
        MirrorConstants(d, true, 160 + r->constLo, r->consts + r->constLo * 4, r->constHi - r->constLo, r->psReads);
    }
    lastRule = r;
    lastConstSerial = MirrorConstSerial();
    // the rule's shaders lent (d3dmirror.cpp: left bound for the next draw, the game's given back when something else
    // draws or asks); without the mirror set here and put back below
    const bool lent = !skinned && MirrorLend(d, r->vs, r->ps);
    if (!skinned && !lent) {
        if (r->vs) d->SetVertexShader(r->vs);
        if (r->ps) d->SetPixelShader(r->ps);
    }
    // the shadow map at sampler 2 (shadows.cpp), the sampler's state put back after
    IDirect3DBaseTexture9 *map = r ? ShadowsTexture() : nullptr, *oldTex = nullptr;
    DWORD ss[5] = {};
    const D3DSAMPLERSTATETYPE kSs[5] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV};
    if (map) {
        oldTex = st.tex[2];
        for (int i = 0; i < 5; i++) ss[i] = st.samp[2][kSs[i]];
        d->SetTexture(2, map);
        d->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        d->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        d->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_BORDER);
        d->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER);
        d->SetSamplerState(2, D3DSAMP_BORDERCOLOR, 0xFFFFFFFF);
    }
    if (skinned) {   // (a rule's vertex shader only when it reads the skin's inputs: MML_SHADE_SKINNED)
        auto *vs = r && r->vs && (r->flags & MML_SHADE_SKINNED) ? r->vs : static_cast<IDirect3DVertexShader9 *>(SkinGpuShader(0));
        auto *ps = r && r->ps ? r->ps : static_cast<IDirect3DPixelShader9 *>(SkinGpuShader(1));
        *hr = SkinnedDip(d, skin, vs, ps, t, minIndex, verts, start, n, dip);
    } else {
        VdataSaved vs{};   // (the rule's second stream: the normals, tangents and the mod's attributes)
        const bool vdata = (r->flags & MML_SHADE_VERTEX_DATA) && VdataBind(d, g_group.visual, base, verts, &vs);
        *hr = dip(d, t, base, minIndex, verts, start, n);
        if (vdata) VdataUnbind(d, vs);
    }
    if (map) {
        d->SetTexture(2, oldTex);
        for (int i = 0; i < 5; i++) d->SetSamplerState(2, kSs[i], ss[i]);
    }
    if (!MirrorOf(d)) {   // (no mirror: the game's put back at once)
        d->SetVertexShader(oldVs);
        d->SetPixelShader(oldPs);
    }
    if (r) r->drawsNow++;
    return true;
}

// ---- HLSL through d3dcompiler_47
using CompileFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
struct Includes : ID3DInclude {
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID *data, UINT *bytes) override {
        std::string n = Lower(name);
        for (char &ch : n) if (ch == '\\') ch = '/';
        if (n == "mml/game.hlsli" || n == "game.hlsli" || n == "mml/shaders/game.hlsli") {
            *data = kGameHlsli;
            *bytes = static_cast<UINT>(strlen(kGameHlsli));
            return S_OK;
        }
        if (n == "mml/skin.hlsli" || n == "skin.hlsli" || n == "mml/shaders/skin.hlsli") {
            *data = kSkinHlsli;
            *bytes = static_cast<UINT>(strlen(kSkinHlsli));
            return S_OK;
        }
        return E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }
};
bool Compile(const std::string &src, const char *entry, const char *target, std::vector<uint8_t> *out, std::string *err) {
    static CompileFn compile = [] {
        HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
        return m ? reinterpret_cast<CompileFn>(GetProcAddress(m, "D3DCompile")) : nullptr;
    }();
    if (!compile) { *err = "d3dcompiler_47.dll is not there (Windows 8.1 and later ship it)"; return false; }
    Includes inc;
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = compile(src.data(), src.size(), "shader", nullptr, &inc, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors) {
        if (FAILED(hr)) err->assign(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize());
        errors->Release();
    }
    if (FAILED(hr) || !code) { if (err->empty()) *err = "the compiler failed"; if (code) code->Release(); return false; }
    out->assign(static_cast<const uint8_t *>(code->GetBufferPointer()), static_cast<const uint8_t *>(code->GetBufferPointer()) + code->GetBufferSize());
    code->Release();
    while (!err->empty() && (err->back() == '\n' || err->back() == '\r' || err->back() == 0)) err->pop_back();
    return true;
}

void Release(Rule &r) {
    if (r.vs) r.vs->Release();
    if (r.ps) r.ps->Release();
    r.vs = nullptr;
    r.ps = nullptr;
}

} // namespace

bool ShadersWanted() { return !g_rules.empty() || g_surveyWanted || ShadowViewNow() || SkinGpuWanted(); }

void ShadersShadowBegin(const float backView[16]) {
    memcpy(g_shadowBack, backView, sizeof g_shadowBack);
    g_shadowDraws = g_shadowFaces = g_shadowSkipped = g_shadowCut = 0;
    g_shadowVPSet = false;
}
void ShadersShadowStats(unsigned *draws, unsigned *faces, unsigned *skipped, unsigned *cut, float viewProj[16], std::string *why) {
    if (g_shadowVPSet) memcpy(viewProj, g_shadowVP, sizeof g_shadowVP);
    *draws = g_shadowDraws;
    *faces = g_shadowFaces;
    *skipped = g_shadowSkipped;
    *cut = g_shadowCut;
    *why = g_shadowWhy;
}

void ShadersGroupBegin(int kind, void *visual, uint8_t *entry, int where) {
    g_group = Group{};
    // (the drawing from the sun is neither the frame nor a mod's view; a person the GPU skins is drawn by it anywhere)
    if (!where && !ShadowViewNow() && !(kind == 1 && SkinGpuWanted())) return;
    const void *group = At<const void *>(entry, kEntryGroup);
    const void *mat = group ? At<const void *>(group, kGroupMaterial) : nullptr;
    g_group.active = true;
    g_group.kind = kind;
    g_group.where = where;
    g_group.visual = visual;
    g_group.group = group;
    g_group.material = mat;
    g_group.texture = TextureName(mat);
    g_group.matFlags = mat ? At<uint32_t>(mat, kMaterialFlags) : 0;
    g_group.faces = group ? At<uint32_t>(group, kGroupFaces) : 0;
}
void ShadersGroupEnd() { g_group.active = false; }

bool ShadersDraw(void *dev, int type, int base, unsigned minIndex, unsigned verts, unsigned start, unsigned n, void *dip, HRESULT *hr) {
    if (!g_group.active) return false;
    bool done = false;
    const long long t0 = SkinGpuTimeBegin();   // (the draws' time while the GPU skins: skin.state's gpu)
    auto *d = static_cast<IDirect3DDevice9 *>(dev);
    __try {
        done = Draw(d, static_cast<D3DPRIMITIVETYPE>(type), base, minIndex, verts, start, n, reinterpret_cast<DipFn>(dip), hr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults++;
        done = false;
    }
    g_group.pass++;
    SkinGpuTimeEnd(t0);
    return done;
}

int ShaderAdd(MmlMod *mod, const MmlShaderRule *in, std::string *err) {
    if (!in) { *err = "no rule"; return 0; }
    auto r = std::make_unique<Rule>();
    r->mod = mod;
    r->pattern = Lower(in->pattern && *in->pattern ? in->pattern : "*");
    r->kinds = in->kinds;
    r->classes = in->classes;
    r->where = in->where;
    r->flags = in->flags;
    if (in->source && *in->source) {
        std::string src = in->source;
        if (in->vs_entry && *in->vs_entry && !Compile(src, in->vs_entry, in->vs_target && *in->vs_target ? in->vs_target : "vs_3_0", &r->vsCode, err)) {
            *err = "vertex shader: " + *err;
            return 0;
        }
        if (in->ps_entry && *in->ps_entry && !Compile(src, in->ps_entry, in->ps_target && *in->ps_target ? in->ps_target : "ps_3_0", &r->psCode, err)) {
            *err = "pixel shader: " + *err;
            return 0;
        }
    } else {
        if (in->vs_code && in->vs_size) r->vsCode.assign(static_cast<const uint8_t *>(in->vs_code), static_cast<const uint8_t *>(in->vs_code) + in->vs_size);
        if (in->ps_code && in->ps_size) r->psCode.assign(static_cast<const uint8_t *>(in->ps_code), static_cast<const uint8_t *>(in->ps_code) + in->ps_size);
    }
    if (r->vsCode.empty() && r->psCode.empty()) { *err = "no shader: source with vs_entry and/or ps_entry, or vs_code / ps_code"; return 0; }
    if (r->vsCode.empty()) memset(r->vsReads, 0, sizeof r->vsReads); else ReadsOf(r->vsCode, r->vsReads);
    if (r->psCode.empty()) memset(r->psReads, 0, sizeof r->psReads); else ReadsOf(r->psCode, r->psReads);
    {
        uint32_t u[8];
        for (int i = 0; i < 8; i++) u[i] = r->vsReads[i] | r->psReads[i];
        r->readsLights = Reads(u, 27, 75);
        r->readsShadows = Reads(u, 76, 117);
        r->readsNear = Reads(u, 84, 117);
    }
    if (!GeomEnsureHooks(err)) return 0;
    r->handle = g_nextHandle++;
    int h = r->handle;
    g_rules.push_back(std::move(r));
    if (g_rules.size() == 1) MirrorUse(true);   // (the device's state followed while a rule draws)
    return h;
}

bool ShaderRemove(MmlMod *mod, int handle) {
    bool any = false;
    for (auto it = g_rules.begin(); it != g_rules.end();) {
        if ((*it)->mod == mod && (handle == 0 || (*it)->handle == handle)) { Release(**it); it = g_rules.erase(it); any = true; }
        else ++it;
    }
    if (any && g_rules.empty()) MirrorUse(false);
    return any;
}
void ShadersModGone(MmlMod *mod) { ShaderRemove(mod, 0); VdataModGone(mod); }

int ShaderConstants(MmlMod *mod, int handle, int reg, const float *values, int count, std::string *err) {
    if (!values || count <= 0 || reg < 160 || reg + count > 224) { *err = "registers c160..c223 (reg 160, count up to 64)"; return 0; }
    int n = 0;
    for (auto &r : g_rules) {
        if (r->mod != mod || (handle && r->handle != handle)) continue;
        int lo = reg - 160, hi = lo + count;
        memcpy(r->consts + lo * 4, values, size_t(count) * 16);
        if (r->constHi <= r->constLo) r->constLo = lo, r->constHi = hi;
        else r->constLo = std::min(r->constLo, lo), r->constHi = std::max(r->constHi, hi);
        n++;
    }
    if (!n) { *err = handle ? "no such rule of the mod's" : "the mod has no rules"; return 0; }
    return n;
}

void ShadersFrameEnd() {   // (render.cpp, after the frame's own drawing)
    VdataFrameEnd();
    g_frameSeconds = static_cast<float>((GetTickCount64() % 3600000ull) / 1000.0);   // (mml_Camera.w: once a frame)
    MirrorGiveBack(nullptr);   // (the game's shaders for what it draws after its world: the 2D pass)
    MirrorFrame();   // (the mirror read again from the device at the next frame's first draw)
    for (auto &r : g_rules) { r->drawsLast = r->drawsNow; r->drawsNow = 0; }
    if (g_surveyWanted) {
        std::vector<const Combo *> list;
        for (auto &kv : g_combos) list.push_back(&kv.second);
        std::sort(list.begin(), list.end(), [](const Combo *a, const Combo *b) { return a->faces > b->faces; });
        json out = json::array();
        unsigned draws = 0, faces = 0;
        for (const Combo *c : list) {
            json j = c->states;
            j["draws"] = c->draws;
            j["faces"] = c->faces;
            j["textures"] = c->textures;
            out.push_back(j);
            draws += c->draws;
            faces += c->faces;
        }
        g_surveys.push_back({g_surveyWanted, {{"combinations", out}, {"draws", draws}, {"faces", faces}}});
        if (g_surveys.size() > 4) g_surveys.erase(g_surveys.begin());
        g_combos.clear();
        g_surveyWanted = 0;
    }
}

bool ShadersMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method.rfind("shader.", 0) != 0) return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string err;
    try {
        if (method == "shader.add") {
            std::string src = p.value("source", ""), pat = p.value("pattern", "*"), vse = p.value("vs", ""), pse = p.value("ps", "");
            MmlShaderRule r{};
            r.size = sizeof r;
            r.pattern = pat.c_str();
            auto mask = [&](const char *key, const std::map<std::string, uint32_t> &names) {
                uint32_t m = 0;
                if (p.contains(key) && p[key].is_array())
                    for (auto &x : p[key]) { auto it = names.find(x.get<std::string>()); if (it != names.end()) m |= it->second; }
                return m;
            };
            r.kinds = mask("kinds", {{"object", MML_SHADE_OBJECT}, {"lightmapped", MML_SHADE_LIGHTMAPPED}, {"billboard", MML_SHADE_BILLBOARD}});
            r.classes = mask("classes", {{"opaque", MML_SHADE_OPAQUE}, {"alphatest", MML_SHADE_ALPHATEST}, {"translucent", MML_SHADE_TRANSLUCENT},
                                         {"additive", MML_SHADE_ADDITIVE}});
            r.where = mask("where", {{"frame", MML_GEOM_FRAME}, {"views", MML_GEOM_VIEWS}});
            r.flags = (p.value("all_passes", false) ? MML_SHADE_ALL_PASSES : 0) | (p.value("skinned", false) ? MML_SHADE_SKINNED : 0) |
                      (p.value("vertex_data", false) ? MML_SHADE_VERTEX_DATA : 0);
            r.source = src.c_str();
            r.vs_entry = vse.empty() ? nullptr : vse.c_str();
            r.ps_entry = pse.empty() ? nullptr : pse.c_str();
            int h = ShaderAdd(nullptr, &r, &err);
            if (!h) { *reply = "shader.add: " + err; return true; }
            *reply = dump({{"handle", h}});
        } else if (method == "shader.constants") {
            std::vector<float> v;
            for (auto &x : p.value("values", json::array())) v.push_back(x.get<float>());
            int reg = p.value("reg", 160), n = static_cast<int>(v.size() / 4);
            int done = ShaderConstants(nullptr, p.value("handle", 0), reg, v.data(), n, &err);
            if (!done) { *reply = "shader.constants: " + err + " (values: 4 numbers a register)"; return true; }
            *reply = dump({{"rules", done}, {"registers", n}});
        } else if (method == "shader.vertex_data") {
            std::vector<float> v;
            for (auto &x : p.value("values", json::array())) v.push_back(x.get<float>());
            if (!VdataSet(nullptr, p.value("visual", "").c_str(), p.value("lod", 0), v.empty() ? nullptr : v.data(), static_cast<int>(v.size() / 4), &err)) {
                *reply = "shader.vertex_data: " + err;
                return true;
            }
            *reply = dump({{"vertices", v.size() / 4}});
        } else if (method == "shader.remove") {
            bool all = p.value("all", false);
            ShaderRemove(nullptr, all ? 0 : p.value("handle", -1));
            *reply = dump({{"rules", g_rules.size()}});
        } else if (method == "shader.shadows") {
            if (!p.value("on", true)) {
                ShaderShadows(nullptr, nullptr);
            } else {
                std::string casters = p.value("casters", "");
                ShadowsTune(p.value("sun_distance", 0.0f), casters == "game" ? 1 : casters == "meshes" ? 0 : -1);
                MmlShadowSettings ss{p.value("size", 2048), p.value("extent", 60.0f), p.value("bias", 0.0f)};
                ShaderShadows(nullptr, &ss);
            }
            std::string sh;
            ShadowsState(&sh);
            *reply = dump(json::parse(sh, nullptr, false));
        } else if (method == "shader.state") {
            json rules = json::array();
            for (auto &r : g_rules) {
                json j = {{"owner", ModIdOf(r->mod)}, {"handle", r->handle}, {"pattern", r->pattern}, {"kinds", r->kinds}, {"classes", r->classes},
                          {"vs", r->vsCode.size()}, {"ps", r->psCode.size()}, {"draws_last_frame", r->drawsLast}};
                if (!r->error.empty()) j["error"] = r->error;
                rules.push_back(j);
            }
            std::string sh;
            ShadowsState(&sh);
            *reply = dump({{"rules", rules}, {"faults", g_faults}, {"shadows", json::parse(sh, nullptr, false)},
                           {"vertex_data", json::parse(VdataState(), nullptr, false)}});
        } else if (method == "shader.mirror") {
            if (p.contains("check")) MirrorCheckNext(std::max(0, std::min(p["check"].get<int>(), 100000)));
            *reply = MirrorStateJson();
        } else if (method == "shader.survey") {
            int id = p.value("id", 0);
            if (!id) {
                if (!GeomEnsureHooks(&err) || !GeomEnsureDip(&err)) { *reply = "shader.survey: " + err; return true; }
                if (!g_surveyWanted) { g_surveyWanted = g_nextSurvey++; g_combos.clear(); }
                *reply = dump({{"id", g_surveyWanted}, {"done", false}, {"next", "shader.survey {id} after a frame of the mission's world"}});
            } else {
                json out = {{"id", id}, {"done", false}};
                for (const SurveyDone &s : g_surveys) if (s.id == id) { out = s.result; out["id"] = id; out["done"] = true; }
                *reply = dump(out);
            }
        } else {
            *reply = "unknown shader method (shader.add, shader.remove, shader.state, shader.survey, shader.shadows, shader.constants, shader.vertex_data, shader.mirror)";
            return true;
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
