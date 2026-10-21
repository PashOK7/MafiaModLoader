// Drawing in the world (mml.h draw_world; docs/MODDING.md "Drawing in the world").
//
// Lines, boxes, spheres and capsules in the world's space, drawn as lines over the frame's 3D world once the mods'
// RENDER_WORLD has run (render.cpp RenderStage): tested against the frame's depth (hidden behind walls, not writing
// it) or over everything (MML_DRAW_ON_TOP). What a physics mod draws to see its bodies, joints and contacts, what an
// agent draws to see a place (`draw.world`). The host draws on the game's Direct3D 9 device (the game on a wrapper,
// as in the labs) with every state it needs set and the game's state block applied after, so LS3D's own record of
// the device's states stays true (its own line and box drawing, ls3d+16B20 and +186A0, sets states through
// that record and draw with whatever world matrix the last object left: not used). Plain Direct3D 8: nothing drawn.
// A shape of 0 ms is drawn in the next frame of the world and dropped; a held one until its time is up. A sphere is
// its three great circles, a capsule its two ends' and the side lines, a box its twelve edges (turned by its axes).
//
// Bridge methods (game thread): draw.world {line | box | sphere | capsule, color, ms, on_top; clear}, draw.world_state.
#include "host.h"
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

constexpr size_t kMaxShapes = 20000;
constexpr int kSegments = 24;
constexpr float kPi = 3.14159265f;

struct Shape { MmlMod *mod; MmlDrawShape s; DWORD until; bool once; };
std::vector<Shape> g_shapes;
struct Vertex { float x, y, z; DWORD color; };
std::vector<Vertex> g_lines;
unsigned g_framesDrawn, g_drawnLast, g_refused;
std::string g_why = "nothing drawn yet";

struct P3 { float x, y, z; };
P3 Add(P3 a, P3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
P3 Sub(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
P3 Mul(P3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(P3 a, P3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
P3 Cross(P3 a, P3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
P3 Unit(P3 a, P3 fb) { float l = std::sqrt(Dot(a, a)); return l > 1e-6f ? Mul(a, 1.0f / l) : fb; }
P3 At(const float *p) { return {p[0], p[1], p[2]}; }

void Line(P3 a, P3 b, DWORD c, std::vector<Vertex> &v) {
    v.push_back({a.x, a.y, a.z, c});
    v.push_back({b.x, b.y, b.z, c});
}
// a circle (or an arc from a0 to a1 radians) about `centre` in the plane of u, w
void Arc(P3 centre, P3 u, P3 w, float r, float a0, float a1, DWORD c, std::vector<Vertex> &v) {
    int n = std::max(2, static_cast<int>(kSegments * (a1 - a0) / (2 * kPi) + 0.5f));
    P3 prev = Add(centre, Add(Mul(u, r * std::cos(a0)), Mul(w, r * std::sin(a0))));
    for (int i = 1; i <= n; i++) {
        float t = a0 + (a1 - a0) * i / n;
        P3 p = Add(centre, Add(Mul(u, r * std::cos(t)), Mul(w, r * std::sin(t))));
        Line(prev, p, c, v);
        prev = p;
    }
}

void Lines(const MmlDrawShape &s, std::vector<Vertex> &v) {
    DWORD c = s.color;
    P3 a = At(s.a), b = At(s.b);
    switch (s.kind) {
    case MML_DRAW_LINE:
        Line(a, b, c, v);
        break;
    case MML_DRAW_SPHERE:
        Arc(a, {1, 0, 0}, {0, 1, 0}, s.radius, 0, 2 * kPi, c, v);
        Arc(a, {1, 0, 0}, {0, 0, 1}, s.radius, 0, 2 * kPi, c, v);
        Arc(a, {0, 1, 0}, {0, 0, 1}, s.radius, 0, 2 * kPi, c, v);
        break;
    case MML_DRAW_CAPSULE: {
        P3 axis = Unit(Sub(b, a), {0, 1, 0});
        P3 u = Unit(Cross(axis, std::fabs(axis.y) < 0.9f ? P3{0, 1, 0} : P3{1, 0, 0}), {1, 0, 0}), w = Cross(axis, u);
        float r = s.radius;
        Arc(a, u, w, r, 0, 2 * kPi, c, v);
        Arc(b, u, w, r, 0, 2 * kPi, c, v);
        for (P3 side : {u, Mul(u, -1), w, Mul(w, -1)}) Line(Add(a, Mul(side, r)), Add(b, Mul(side, r)), c, v);
        Arc(a, u, Mul(axis, -1), r, 0, kPi, c, v);   // the ends: half circles away from the middle
        Arc(a, w, Mul(axis, -1), r, 0, kPi, c, v);
        Arc(b, u, axis, r, 0, kPi, c, v);
        Arc(b, w, axis, r, 0, kPi, c, v);
        break;
    }
    case MML_DRAW_BOX: {
        P3 ax[3] = {At(s.axes), At(s.axes + 3), At(s.axes + 6)};
        if (Dot(ax[0], ax[0]) + Dot(ax[1], ax[1]) + Dot(ax[2], ax[2]) < 1e-9f) { ax[0] = {1, 0, 0}; ax[1] = {0, 1, 0}; ax[2] = {0, 0, 1}; }
        P3 h[3] = {Mul(ax[0], b.x), Mul(ax[1], b.y), Mul(ax[2], b.z)};
        auto corner = [&](int i) {
            return Add(a, Add(Mul(h[0], (i & 1) ? 1.0f : -1.0f), Add(Mul(h[1], (i & 2) ? 1.0f : -1.0f), Mul(h[2], (i & 4) ? 1.0f : -1.0f))));
        };
        for (int i = 0; i < 8; i++)
            for (int bit = 1; bit < 8; bit <<= 1)
                if (!(i & bit)) Line(corner(i), corner(i | bit), c, v);
        break;
    }
    default:
        break;
    }
}

bool Valid(const MmlDrawShape &s) {
    if (s.kind < MML_DRAW_LINE || s.kind > MML_DRAW_CAPSULE) return false;
    for (float f : {s.a[0], s.a[1], s.a[2], s.b[0], s.b[1], s.b[2], s.radius})
        if (!std::isfinite(f)) return false;
    return (s.kind != MML_DRAW_SPHERE && s.kind != MML_DRAW_CAPSULE) || (s.radius > 0 && s.radius < 1000);
}

uint32_t ParseColour(const json &p, uint32_t fallback) {
    if (!p.contains("color")) return fallback;
    const json &c = p["color"];
    if (c.is_number()) return c.get<uint32_t>();
    std::string s = c.is_string() ? c.get<std::string>() : "";
    if (s.size() == 7 && s[0] == '#') return 0xFF000000u | static_cast<uint32_t>(strtoul(s.c_str() + 1, nullptr, 16));
    if (s.size() == 9 && s[0] == '#') return static_cast<uint32_t>(strtoul(s.c_str() + 1, nullptr, 16));
    return fallback;
}
bool Vec3(const json &j, float *out) {
    if (!j.is_array() || j.size() != 3) return false;
    for (int i = 0; i < 3; i++) out[i] = j[i].get<float>();
    return true;
}

} // namespace

int DrawWorldAdd(MmlMod *mod, const MmlDrawShape *shape, std::string *err) {
    if (!shape) {   // the mod's shapes taken away
        g_shapes.erase(std::remove_if(g_shapes.begin(), g_shapes.end(), [&](const Shape &s) { return s.mod == mod; }), g_shapes.end());
        return 1;
    }
    MmlDrawShape s{};
    memcpy(&s, shape, std::min<size_t>(shape->size ? shape->size : sizeof s, sizeof s));
    s.size = sizeof s;
    if (!Valid(s)) { *err = "kind: MML_DRAW_LINE, _BOX, _SPHERE, _CAPSULE; finite numbers; a sphere's or capsule's radius 0..1000"; g_refused++; return 0; }
    if (g_shapes.size() >= kMaxShapes) { *err = "20000 shapes at most at once"; g_refused++; return 0; }
    int ms = std::min(std::max(s.ms, 0), 600000);
    g_shapes.push_back({mod, s, GetTickCount() + static_cast<DWORD>(ms), ms == 0});
    return 1;
}

bool DrawWorldWanted() { return !g_shapes.empty(); }

// render.cpp, at the world's stage 1 after the mods' RENDER_WORLD, the game's state kept around it
void DrawWorldNow(void *device9) {
    auto *d = static_cast<IDirect3DDevice9 *>(device9);
    DWORD now = GetTickCount();
    g_shapes.erase(std::remove_if(g_shapes.begin(), g_shapes.end(),
                                  [&](const Shape &s) { return !s.once && static_cast<int>(s.until - now) <= 0; }), g_shapes.end());
    if (g_shapes.empty()) return;
    if (!d) { g_why = "no Direct3D 9 device (the game on plain Direct3D 8)"; g_shapes.erase(std::remove_if(g_shapes.begin(), g_shapes.end(), [](const Shape &s) { return s.once; }), g_shapes.end()); return; }
    D3DMATRIX identity{};
    identity._11 = identity._22 = identity._33 = identity._44 = 1;
    d->SetTransform(D3DTS_WORLD, &identity);   // (the view and the projection are the frame's camera's at stage 1)
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d->SetTexture(0, nullptr);
    d->SetTexture(1, nullptr);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_CLIPPING, TRUE);
    unsigned drawn = 0;
    for (int top = 0; top < 2; top++) {
        g_lines.clear();
        for (const Shape &s : g_shapes)
            if (((s.s.flags & MML_DRAW_ON_TOP) != 0) == (top != 0)) { Lines(s.s, g_lines); drawn++; }
        if (g_lines.empty()) continue;
        d->SetRenderState(D3DRS_ZENABLE, top ? D3DZB_FALSE : D3DZB_TRUE);
        for (size_t i = 0; i < g_lines.size(); i += 2 * 30000) {   // (a draw's primitives: well under the device's limit)
            UINT n = static_cast<UINT>(std::min<size_t>(g_lines.size() - i, 2 * 30000) / 2);
            d->DrawPrimitiveUP(D3DPT_LINELIST, n, &g_lines[i], sizeof(Vertex));
        }
    }
    g_shapes.erase(std::remove_if(g_shapes.begin(), g_shapes.end(), [](const Shape &s) { return s.once; }), g_shapes.end());
    g_framesDrawn++;
    g_drawnLast = drawn;
    g_why = "drawn";
}

void DrawWorldModGone(MmlMod *mod) {
    g_shapes.erase(std::remove_if(g_shapes.begin(), g_shapes.end(), [&](const Shape &s) { return s.mod == mod; }), g_shapes.end());
}

bool DrawWorldMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "draw.world" && method != "draw.world_state") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "draw.world_state") {
            size_t bridge = std::count_if(g_shapes.begin(), g_shapes.end(), [](const Shape &s) { return !s.mod; });
            *reply = dump({{"shapes", g_shapes.size()}, {"bridge", bridge}, {"frames_drawn", g_framesDrawn}, {"drawn_last_frame", g_drawnLast},
                           {"refused", g_refused}, {"last", g_why}});
            *ok = true;
            return true;
        }
        if (p.value("clear", false)) {
            size_t n = g_shapes.size();
            DrawWorldAdd(nullptr, nullptr, nullptr);
            *reply = dump({{"cleared", n - g_shapes.size()}});
            *ok = true;
            return true;
        }
        MmlDrawShape s{};
        s.size = sizeof s;
        s.color = ParseColour(p, 0xFFFFFF00u);
        s.ms = p.value("ms", 5000);
        if (p.value("on_top", false)) s.flags |= MML_DRAW_ON_TOP;
        if (p.contains("line")) {
            const json &l = p["line"];
            if (!l.is_array() || l.size() != 2 || !Vec3(l[0], s.a) || !Vec3(l[1], s.b)) { *reply = "draw.world: line [[x, y, z], [x, y, z]]"; return true; }
            s.kind = MML_DRAW_LINE;
        } else if (p.contains("sphere")) {
            const json &o = p["sphere"];
            if (!Vec3(o.value("centre", json()), s.a)) { *reply = "draw.world: sphere {centre: [x, y, z], radius}"; return true; }
            s.radius = o.value("radius", 0.5f);
            s.kind = MML_DRAW_SPHERE;
        } else if (p.contains("capsule")) {
            const json &o = p["capsule"];
            if (!Vec3(o.value("a", json()), s.a) || !Vec3(o.value("b", json()), s.b)) { *reply = "draw.world: capsule {a, b, radius}"; return true; }
            s.radius = o.value("radius", 0.2f);
            s.kind = MML_DRAW_CAPSULE;
        } else if (p.contains("box")) {
            const json &o = p["box"];
            if (!Vec3(o.value("centre", json()), s.a) || !Vec3(o.value("half", json()), s.b)) { *reply = "draw.world: box {centre, half: [hx, hy, hz], axes: [[x axis], [y], [z]]}"; return true; }
            if (o.contains("axes") && o["axes"].is_array() && o["axes"].size() == 3)
                for (int k = 0; k < 3; k++) Vec3(o["axes"][k], s.axes + 3 * k);
            s.kind = MML_DRAW_BOX;
        } else {
            *reply = "draw.world: line, box, sphere or capsule (in the world's metres); color (\"#AARRGGBB\"), ms (default 5000; 0 one frame), on_top; or clear";
            return true;
        }
        std::string err;
        if (!DrawWorldAdd(nullptr, &s, &err)) { *reply = "draw.world: " + err; return true; }
        *reply = dump({{"shapes", g_shapes.size()}, {"ms", s.ms}});
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
