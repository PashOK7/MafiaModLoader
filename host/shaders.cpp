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
};
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
uint32_t ClassOf(IDirect3DDevice9 *d) {
    DWORD blend = 0, test = 0, src = 0, dst = 0;
    d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    d->GetRenderState(D3DRS_ALPHATESTENABLE, &test);
    if (blend) {
        d->GetRenderState(D3DRS_SRCBLEND, &src);
        d->GetRenderState(D3DRS_DESTBLEND, &dst);
        return dst == D3DBLEND_ONE ? MML_SHADE_ADDITIVE : MML_SHADE_TRANSLUCENT;
    }
    return test ? MML_SHADE_ALPHATEST : MML_SHADE_OPAQUE;
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
void Constants(IDirect3DDevice9 *d, float *c) {
    memset(c, 0, kRegs * 16);
    D3DMATRIX w{}, v{}, p{}, vp, wvp;
    d->GetTransform(D3DTS_WORLD, &w);
    d->GetTransform(D3DTS_VIEW, &v);
    d->GetTransform(D3DTS_PROJECTION, &p);
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
    c[67] = static_cast<float>((GetTickCount64() % 3600000ull) / 1000.0);
    DWORD fog = 0, table = 0, vert = 0, fogColor = 0, amb = 0, ref = 0, test = 0, lighting = 0;
    float fs = 0, fe = 0, fd = 0;
    d->GetRenderState(D3DRS_FOGENABLE, &fog);
    d->GetRenderState(D3DRS_FOGTABLEMODE, &table);
    d->GetRenderState(D3DRS_FOGVERTEXMODE, &vert);
    d->GetRenderState(D3DRS_FOGSTART, reinterpret_cast<DWORD *>(&fs));
    d->GetRenderState(D3DRS_FOGEND, reinterpret_cast<DWORD *>(&fe));
    d->GetRenderState(D3DRS_FOGDENSITY, reinterpret_cast<DWORD *>(&fd));
    d->GetRenderState(D3DRS_FOGCOLOR, &fogColor);
    d->GetRenderState(D3DRS_AMBIENT, &amb);
    d->GetRenderState(D3DRS_ALPHATESTENABLE, &test);
    d->GetRenderState(D3DRS_ALPHAREF, &ref);
    d->GetRenderState(D3DRS_LIGHTING, &lighting);
    DWORD mode = table != D3DFOG_NONE ? table : vert;
    c[68] = fs; c[69] = fe; c[70] = fd; c[71] = fog ? static_cast<float>(mode) : 0.0f;
    auto rgba = [](DWORD x, float *o) {
        o[0] = ((x >> 16) & 0xFF) / 255.0f; o[1] = ((x >> 8) & 0xFF) / 255.0f; o[2] = (x & 0xFF) / 255.0f; o[3] = ((x >> 24) & 0xFF) / 255.0f;
    };
    rgba(fogColor, c + 72);
    c[75] = fog ? 1.0f : 0.0f;
    D3DMATERIAL9 m{};
    d->GetMaterial(&m);
    memcpy(c + 76, &m.Diffuse, 16);
    memcpy(c + 80, &m.Ambient, 16);
    memcpy(c + 84, &m.Emissive, 16);
    rgba(amb, c + 88);
    c[92] = static_cast<float>(g_group.kind);
    c[93] = lighting ? 1.0f : 0.0f;
    c[94] = test ? (ref & 0xFF) / 255.0f : -1.0f;
    c[95] = static_cast<float>(g_group.pass);
    for (DWORD s = 0; s < 2; s++) {
        DWORD cop = 0, aop = 0, tci = 0;
        d->GetTextureStageState(s, D3DTSS_COLOROP, &cop);
        d->GetTextureStageState(s, D3DTSS_ALPHAOP, &aop);
        d->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &tci);
        IDirect3DBaseTexture9 *t = nullptr;
        d->GetTexture(s, &t);
        float *o = c + 96 + s * 4;
        o[0] = static_cast<float>(cop); o[1] = static_cast<float>(aop); o[2] = static_cast<float>(tci & 0xFFFF); o[3] = t ? 1.0f : 0.0f;
        if (t) t->Release();
    }
    D3DVIEWPORT9 vpo{};
    d->GetViewport(&vpo);
    int n = 0;
    for (DWORD i = 0; i < 8 && n < 8; i++) {
        BOOL on = FALSE;
        if (FAILED(d->GetLightEnable(i, &on)) || !on) continue;
        D3DLIGHT9 l{};
        d->GetLight(i, &l);
        float *o = c + 108 + n * 24;
        o[0] = l.Position.x; o[1] = l.Position.y; o[2] = l.Position.z; o[3] = static_cast<float>(l.Type);
        o[4] = l.Direction.x; o[5] = l.Direction.y; o[6] = l.Direction.z; o[7] = l.Range;
        memcpy(o + 8, &l.Diffuse, 16);
        memcpy(o + 12, &l.Ambient, 16);
        o[16] = l.Attenuation0; o[17] = l.Attenuation1; o[18] = l.Attenuation2; o[19] = l.Falloff;
        o[20] = std::cos(l.Theta / 2); o[21] = std::cos(l.Phi / 2);
        n++;
    }
    DWORD range = 0, tf = 0;
    d->GetRenderState(D3DRS_RANGEFOGENABLE, &range);
    d->GetRenderState(D3DRS_TEXTUREFACTOR, &tf);
    rgba(tf, c + 300);
    c[104] = static_cast<float>(n);
    c[105] = range ? 1.0f : 0.0f;
    c[106] = static_cast<float>(vpo.Width);
    c[107] = static_cast<float>(vpo.Height);
    ShadowsConstants(g_group.visual, c);
}

// What the host's own skinned shaders read of game.hlsli (VsSkin, PsSkin: the transforms, the eye, the fog, the
// draw, the stages, the viewport, the texture factor), c0..c26 and c75: a person's draw no rule takes asks the device
// for these alone (some 25 calls, not the 60 of every register; there are hundreds of such draws a frame).
void CoreConstants(IDirect3DDevice9 *d, float *c) {
    memset(c, 0, 27 * 16);
    D3DMATRIX w{}, v{}, p{}, vp, wvp;
    d->GetTransform(D3DTS_WORLD, &w);
    d->GetTransform(D3DTS_VIEW, &v);
    d->GetTransform(D3DTS_PROJECTION, &p);
    Mul(v, p, &vp);
    Mul(w, vp, &wvp);
    memcpy(c + 0, &w, 64);
    memcpy(c + 16, &v, 64);
    memcpy(c + 32, &p, 64);
    memcpy(c + 48, &wvp, 64);
    float tx = v._41, ty = v._42, tz = v._43;
    c[64] = -(tx * v._11 + ty * v._12 + tz * v._13);
    c[65] = -(tx * v._21 + ty * v._22 + tz * v._23);
    c[66] = -(tx * v._31 + ty * v._32 + tz * v._33);
    c[67] = static_cast<float>((GetTickCount64() % 3600000ull) / 1000.0);
    DWORD fog = 0, table = 0, vert = 0, fogColor = 0, ref = 0, test = 0, range = 0, tf = 0;
    float fs = 0, fe = 0, fd = 0;
    d->GetRenderState(D3DRS_FOGENABLE, &fog);
    if (fog) {
        d->GetRenderState(D3DRS_FOGTABLEMODE, &table);
        d->GetRenderState(D3DRS_FOGVERTEXMODE, &vert);
        d->GetRenderState(D3DRS_FOGSTART, reinterpret_cast<DWORD *>(&fs));
        d->GetRenderState(D3DRS_FOGEND, reinterpret_cast<DWORD *>(&fe));
        d->GetRenderState(D3DRS_FOGDENSITY, reinterpret_cast<DWORD *>(&fd));
        d->GetRenderState(D3DRS_FOGCOLOR, &fogColor);
        d->GetRenderState(D3DRS_RANGEFOGENABLE, &range);
    }
    d->GetRenderState(D3DRS_ALPHATESTENABLE, &test);
    if (test) d->GetRenderState(D3DRS_ALPHAREF, &ref);
    DWORD mode = table != D3DFOG_NONE ? table : vert;
    c[68] = fs; c[69] = fe; c[70] = fd; c[71] = fog ? static_cast<float>(mode) : 0.0f;
    c[72] = ((fogColor >> 16) & 0xFF) / 255.0f; c[73] = ((fogColor >> 8) & 0xFF) / 255.0f; c[74] = (fogColor & 0xFF) / 255.0f;
    c[75] = fog ? 1.0f : 0.0f;
    c[92] = static_cast<float>(g_group.kind);
    c[94] = test ? (ref & 0xFF) / 255.0f : -1.0f;
    c[95] = static_cast<float>(g_group.pass);
    for (DWORD s = 0; s < 2; s++) {
        DWORD cop = 0, aop = 0, tci = 0;
        d->GetTextureStageState(s, D3DTSS_COLOROP, &cop);
        float *o = c + 96 + s * 4;
        o[0] = static_cast<float>(cop);
        if (cop == D3DTOP_DISABLE) continue;   // (a stage off: nothing more of it is read)
        d->GetTextureStageState(s, D3DTSS_ALPHAOP, &aop);
        d->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &tci);
        IDirect3DBaseTexture9 *t = nullptr;
        d->GetTexture(s, &t);
        o[1] = static_cast<float>(aop); o[2] = static_cast<float>(tci & 0xFFFF); o[3] = t ? 1.0f : 0.0f;
        if (t) t->Release();
        if (cop == D3DTOP_BLENDFACTORALPHA || aop == D3DTOP_BLENDFACTORALPHA) d->GetRenderState(D3DRS_TEXTUREFACTOR, &tf);
    }
    D3DVIEWPORT9 vpo{};
    d->GetViewport(&vpo);
    c[105] = range ? 1.0f : 0.0f;
    c[106] = static_cast<float>(vpo.Width);
    c[107] = static_cast<float>(vpo.Height);
    c[300] = ((tf >> 16) & 0xFF) / 255.0f; c[301] = ((tf >> 8) & 0xFF) / 255.0f; c[302] = (tf & 0xFF) / 255.0f; c[303] = ((tf >> 24) & 0xFF) / 255.0f;
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

Rule *RuleFor(IDirect3DDevice9 *d) {
    uint32_t cls = 0;
    std::string name;
    for (auto it = g_rules.rbegin(); it != g_rules.rend(); ++it) {   // (the last added first)
        Rule &r = **it;
        if (!r.error.empty()) continue;
        if (r.kinds && !(r.kinds & (1u << (g_group.kind - 1)))) continue;
        if (r.where && !(r.where & static_cast<uint32_t>(g_group.where))) continue;
        if (g_group.pass > 0 && !(r.flags & MML_SHADE_ALL_PASSES)) continue;
        if (!cls) cls = ClassOf(d);
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
    d->SetVertexShader(vs);
    d->SetPixelShader(ps);
    HRESULT hr = dip(d, t, 0, minIndex, verts, start, n);   // (the LOD's vertices from 0 in the skin's buffers)
    SkinGpuUnbind(d, k);
    return hr;
}

bool ShadowDraw(IDirect3DDevice9 *d, D3DPRIMITIVETYPE t, INT base, UINT minIndex, UINT verts, UINT start, UINT n, DipFn dip, HRESULT *hr) {
    DWORD blend = 0, test = 0, dst = 0;
    d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    d->GetRenderState(D3DRS_ALPHATESTENABLE, &test);
    if (blend) d->GetRenderState(D3DRS_DESTBLEND, &dst);
    D3DMATRIX v{};
    d->GetTransform(D3DTS_VIEW, &v);
    bool sky = !memcmp(&v, g_shadowBack, sizeof g_shadowBack);
    if (sky || (blend && (!test || dst == D3DBLEND_ONE))) { g_shadowSkipped++; *hr = S_OK; return true; }   // (left out: no light stopped)
    if (!MakeShadowShaders(d)) { *hr = S_OK; return true; }
    if (!g_shadowVPSet) {   // (what the map is read with is what it was drawn with)
        D3DMATRIX p{};
        d->GetTransform(D3DTS_PROJECTION, &p);
        Mul(v, p, reinterpret_cast<D3DMATRIX *>(g_shadowVP));
        g_shadowVPSet = true;
    }
    float c[kRegs * 4];
    Constants(d, c);
    IDirect3DVertexShader9 *oldVs = nullptr;
    IDirect3DPixelShader9 *oldPs = nullptr;
    d->GetVertexShader(&oldVs);
    d->GetPixelShader(&oldPs);
    d->SetVertexShaderConstantF(0, c, kRegs);
    d->SetPixelShaderConstantF(0, c, kRegs);
    SkinGpuDraw skin;
    const bool skinned = SkinGpuFor(d, g_group.visual, base, &skin);
    d->SetVertexShader(g_shadowVs);
    d->SetPixelShader(g_shadowPs);
    if (blend) { d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); g_shadowCut++; }   // (a fence's cut-outs: tested, not blended)
    if (skinned)
        *hr = SkinnedDip(d, skin, static_cast<IDirect3DVertexShader9 *>(SkinGpuShader(2)), g_shadowPs, t, minIndex, verts, start, n, dip);
    else
        *hr = dip(d, t, base, minIndex, verts, start, n);
    if (blend) d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);   // (as LS3D left it: its own cache of the states holds)
    d->SetVertexShader(oldVs);
    d->SetPixelShader(oldPs);
    if (oldVs) oldVs->Release();
    if (oldPs) oldPs->Release();
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
    Rule *r = g_rules.empty() || !g_group.where ? nullptr : RuleFor(d);   // (the rules: the frame's and the views' draws)
    if (r && !Make(*r, d)) r = nullptr;
    if (!r && !skinned) return false;
    float c[kRegs * 4];
    if (r) Constants(d, c);
    else CoreConstants(d, c);   // (a person no rule takes: the host's shaders read these alone)
    IDirect3DVertexShader9 *oldVs = nullptr;
    IDirect3DPixelShader9 *oldPs = nullptr;
    d->GetVertexShader(&oldVs);
    d->GetPixelShader(&oldPs);
    if (r) {
        d->SetVertexShaderConstantF(0, c, kRegs);
        d->SetPixelShaderConstantF(0, c, kRegs);
    } else {
        d->SetVertexShaderConstantF(0, c, 27);
        d->SetPixelShaderConstantF(0, c, 27);
        d->SetPixelShaderConstantF(75, c + 300, 1);
    }
    if (r && r->constHi > r->constLo) {   // (the mod's own registers)
        d->SetVertexShaderConstantF(160 + r->constLo, r->consts + r->constLo * 4, r->constHi - r->constLo);
        d->SetPixelShaderConstantF(160 + r->constLo, r->consts + r->constLo * 4, r->constHi - r->constLo);
    }
    if (!skinned && r->vs) d->SetVertexShader(r->vs);
    if (!skinned && r->ps) d->SetPixelShader(r->ps);
    // the shadow map at sampler 2 (shadows.cpp), the sampler's state put back after
    IDirect3DBaseTexture9 *map = r ? ShadowsTexture() : nullptr, *oldTex = nullptr;
    DWORD ss[5] = {};
    const D3DSAMPLERSTATETYPE kSs[5] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV};
    if (map) {
        d->GetTexture(2, &oldTex);
        for (int i = 0; i < 5; i++) d->GetSamplerState(2, kSs[i], &ss[i]);
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
        if (oldTex) oldTex->Release();
        for (int i = 0; i < 5; i++) d->SetSamplerState(2, kSs[i], ss[i]);
    }
    d->SetVertexShader(oldVs);
    d->SetPixelShader(oldPs);
    if (oldVs) oldVs->Release();
    if (oldPs) oldPs->Release();
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
    if (!GeomEnsureHooks(err)) return 0;
    r->handle = g_nextHandle++;
    int h = r->handle;
    g_rules.push_back(std::move(r));
    return h;
}

bool ShaderRemove(MmlMod *mod, int handle) {
    bool any = false;
    for (auto it = g_rules.begin(); it != g_rules.end();) {
        if ((*it)->mod == mod && (handle == 0 || (*it)->handle == handle)) { Release(**it); it = g_rules.erase(it); any = true; }
        else ++it;
    }
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
            *reply = "unknown shader method (shader.add, shader.remove, shader.state, shader.survey, shader.shadows, shader.constants, shader.vertex_data)";
            return true;
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
