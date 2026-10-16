// The HUD for mods and agents: pictures, the game's own messages, markers.
//
// Pictures: textures opened by the game's own loader, IGraph::CreateITexture (vtable +4: stdcall(igraph, diffuse,
// alpha, flags, ITexture **out)), as the HUD opens its own (G_IndicatorsClass::Create: "1numbers.tga" with flags 2,
// "2blood.bmp" with "2blood+.bmp" and 4). LS3D looks for the file in maps\: the archives, or a mod's files\Maps\
// (files.cpp answers the game's opens). The flags are the game's: 2 without an alpha file (the menus' pictures,
// GM_Picture::SetString), 4 with one. ITexture's Release is its vtable's first slot; its size is at +0x28 and +0x2A
// (mEdit's ITexture.h). LS3D shares a texture opened twice (its count raised): each handle holds one reference.
// draw.cpp draws them (draw_picture, draw.picture, the markers).
//
// Messages: G_IndicatorsClass::ConsoleAddText (the console's five lines; it copies 64 bytes with strncpy: 63 kept,
// so the line ends) and MissionHelpAdd (interact.cpp's HudHelpLine: the help line with a text id of the loader's).
//
// Markers: a place (in the world, or following a frame) shown on the screen (projected at each 2D pass with the
// camera that frame was drawn with: draw.cpp's pass items; only while the mission runs, not under the pause menu),
// on the radar (RadarAddObject: a frame and a colour) and as the compass' target (CompassSetDestination and the HUD's
// flag 0x80, as the scripts' SETCOMPASS). The radar and the compass take frames: a marker at a place of the world
// gets an LS3D dummy frame of its own there (I3D_driver::CreateFrame(FRAME_DUMMY), SetPos; not linked into the scene:
// the HUD only reads its world matrix); a marker on a frame gives them that frame (its origin). The game draws its
// radar only where the mission shows it (the radar's flag is never set here: without the mission's radar map DrawAll
// reads a null). The compass taken by markers goes back to the target it had, and its flag, when the last of them
// goes, unless the mission changed it meanwhile.
//
// Bridge methods (game thread): hud.console {text, color}, hud.help {text}, hud.marker {at | on, bone, pos, label,
// color, picture, scale, screen, distance, edge, radar, compass; id: change one}, hud.remove {id | all}, hud.list.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kFrameDummy = 6;      // I3D_FRAME_TYPE's FRAME_DUMMY
constexpr int kVtRelease = 0x00, kVtUpdate = 0x18;
constexpr uint32_t kCompassShown = 0x80;
constexpr int kTexWidth = 0x28, kTexHeight = 0x2A;

using CreateTexFn = int(__stdcall *)(void *igraph, const char *file, const char *alpha, uint32_t flags, void **out);
using ReleaseFn = int(__stdcall *)(void *self);
using CreateFrameFn = void *(__stdcall *)(void *driver, int type);
using UpdateFn = int(__stdcall *)(void *frame);
using SetPosFn = void(__fastcall *)(void *frame, void *edx, const V3 *pos);
using HudFrameFn = void(__fastcall *)(uintptr_t hud, void *edx, void *frame);
using HudRadarAddFn = void(__fastcall *)(uintptr_t hud, void *edx, void *frame, uint32_t argb);
using HudFlagFn = void(__fastcall *)(uintptr_t hud, void *edx, uint32_t flags);
using HudConsoleFn = void(__fastcall *)(uintptr_t hud, void *edx, const char *text, uint32_t rgb);

const game::ExeBuild &E() { return game::Exe(); }
void *Vt(void *obj, int off) { return (*reinterpret_cast<void ***>(obj))[off / 4]; }
template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

struct Picture { int id; MmlMod *mod; std::string file, alpha; void *tex; int w, h; };
std::vector<Picture> g_pics;
int g_nextPic;

struct Marker {
    int id;
    MmlMod *mod;              // null: the bridge's
    V3 pos;                   // the world's, or in the frame's space
    void *frame;
    uint32_t flags, argb;
    int picture;
    float scale;
    std::string label;        // the game's encoding
    std::string labelUtf8;    // as it was given
    void *dummy;              // a frame of ours at pos (the radar's and the compass' target for a place)
    void *radar;              // the frame on the radar (null: none)
};
std::vector<Marker> g_markers;
int g_nextMarker;
int g_compassBy;              // the marker the compass points for (0: none of ours)
void *g_compassOurs;          // the frame it was given
void *g_compassPrev;          // what it had before ours
bool g_compassPrevShown;

bool HudKnown() { return game::g_exe && E().hud && E().hudCompassSet && E().hudRadarAdd && E().hudAddFlag && E().hudFlags; }

bool Place(const Marker &m, V3 *out) {
    if (!m.frame) { *out = m.pos; return true; }
    float w[16];
    if (!FrameWorld(m.frame, w)) return false;
    *out = {w[12] + m.pos.x * w[0] + m.pos.y * w[4] + m.pos.z * w[8], w[13] + m.pos.x * w[1] + m.pos.y * w[5] + m.pos.z * w[9],
            w[14] + m.pos.x * w[2] + m.pos.y * w[6] + m.pos.z * w[10]};
    return true;
}

bool PlayerAt(V3 *out) {
    void *pl = game::g_exe ? game::Player() : nullptr;
    void *model = pl ? ActorFrame(pl) : nullptr;
    float w[16];
    if (!model || !FrameWorld(model, w)) return false;
    *out = {w[12], w[13], w[14]};
    return true;
}

void Release(void *obj) {
    if (!obj) return;
    auto f = [&] { reinterpret_cast<ReleaseFn>(Vt(obj, kVtRelease))(obj); };
    Guarded(f);
}

void *MakeDummy(const V3 &at) {
    void *driver = *reinterpret_cast<void **>(E().i3dDriver);
    if (!driver || !E().drvCreateFrame || !E().frameSetPos) return nullptr;
    void *frame = nullptr;
    auto f = [&] {
        frame = reinterpret_cast<CreateFrameFn>(Vt(driver, E().drvCreateFrame))(driver, kFrameDummy);
        if (!frame) return;
        reinterpret_cast<SetPosFn>(E().frameSetPos)(frame, nullptr, &at);
        reinterpret_cast<UpdateFn>(Vt(frame, kVtUpdate))(frame);
    };
    if (Guarded(f)) { Release(frame); return nullptr; }
    return frame;
}

void MoveDummy(void *frame, const V3 &at) {
    auto f = [&] {
        reinterpret_cast<SetPosFn>(E().frameSetPos)(frame, nullptr, &at);
        reinterpret_cast<UpdateFn>(Vt(frame, kVtUpdate))(frame);
    };
    Guarded(f);
}

void *&CompassField() { return game::at<void *>(reinterpret_cast<void *>(E().hud), E().hudCompassTarget); }

void CompassPoint(void *frame) {
    auto f = [&] {
        reinterpret_cast<HudFrameFn>(E().hudCompassSet)(E().hud, nullptr, frame);
        reinterpret_cast<HudFlagFn>(E().hudAddFlag)(E().hud, nullptr, kCompassShown);
    };
    Guarded(f);
}

// The target a marker gives the radar and the compass: its frame, or a dummy frame of its own at its place.
void *Target(Marker &m) {
    if (m.frame) return m.frame;
    if (!m.dummy) m.dummy = MakeDummy(m.pos);
    return m.dummy;
}

void TakeCompass(Marker &m) {
    void *t = Target(m);
    if (!t) return;
    if (!g_compassBy) {
        auto f = [&] {
            g_compassPrev = CompassField();
            g_compassPrevShown = (game::at<uint32_t>(reinterpret_cast<void *>(E().hud), E().hudFlags) & kCompassShown) != 0;
        };
        if (Guarded(f)) return;
    }
    CompassPoint(t);
    g_compassBy = m.id;
    g_compassOurs = t;
}

// The compass given back by marker `id` (removed, or no longer the compass'): to the newest other marker that wants
// it, else to what the mission had, if it still shows ours.
void LeaveCompass(int id) {
    if (g_compassBy != id) return;
    for (auto it = g_markers.rbegin(); it != g_markers.rend(); ++it)
        if (it->id != id && (it->flags & MML_MARKER_COMPASS)) { TakeCompass(*it); return; }
    auto f = [&] {
        if (CompassField() != g_compassOurs) return;   // (the mission set its own since)
        reinterpret_cast<HudFrameFn>(E().hudCompassSet)(E().hud, nullptr, g_compassPrev);
        if (!g_compassPrevShown) reinterpret_cast<HudFlagFn>(E().hudRemoveFlag)(E().hud, nullptr, kCompassShown);
    };
    Guarded(f);
    g_compassBy = 0;
    g_compassOurs = nullptr;
}

void RadarOff(Marker &m) {
    if (!m.radar) return;
    auto f = [&] { reinterpret_cast<HudFrameFn>(E().hudRadarRemove)(E().hud, nullptr, m.radar); };
    Guarded(f);
    m.radar = nullptr;
}

// The radar and the compass made to agree with the marker's flags (its target may have changed).
void Apply(Marker &m) {
    if (m.dummy && !m.frame) MoveDummy(m.dummy, m.pos);
    void *t = (m.flags & (MML_MARKER_RADAR | MML_MARKER_COMPASS)) ? Target(m) : nullptr;
    if (m.radar && (!(m.flags & MML_MARKER_RADAR) || m.radar != t)) RadarOff(m);
    if ((m.flags & MML_MARKER_RADAR) && t && !m.radar) {
        auto f = [&] { reinterpret_cast<HudRadarAddFn>(E().hudRadarAdd)(E().hud, nullptr, t, m.argb ? m.argb : 0xFFFFFFFFu); };
        if (!Guarded(f)) m.radar = t;
    }
    if (m.flags & MML_MARKER_COMPASS) TakeCompass(m);   // (the newest set takes it)
    else LeaveCompass(m.id);
}

void Drop(size_t i) {
    RadarOff(g_markers[i]);
    LeaveCompass(g_markers[i].id);   // (to another marker, which it finds past this one by its id; or the mission's)
    void *dummy = g_markers[i].dummy;
    g_markers.erase(g_markers.begin() + i);
    Release(dummy);
}

bool Fill(Marker *m, const MmlHudMarker *in, std::string *err) {
    if (!in || in->size < sizeof(MmlHudMarker)) { *err = "marker: an MmlHudMarker with its size set"; return false; }
    for (float v : in->pos)
        if (!(v == v && v > -1e6f && v < 1e6f)) { *err = "pos: not a usable place"; return false; }
    if (in->frame) {
        float w[16];
        if (!FrameWorld(in->frame, w)) { *err = "frame: not a frame"; return false; }
    }
    if (in->picture && !HudPictureTexture(in->picture)) { *err = "picture: not a loaded picture (picture_load)"; return false; }
    m->pos = {in->pos[0], in->pos[1], in->pos[2]};
    if (m->frame != in->frame && m->dummy) { Release(m->dummy); m->dummy = nullptr; }
    m->frame = in->frame;
    m->flags = in->flags;
    m->argb = in->argb ? in->argb : 0xFFFFFFFFu;
    m->picture = in->picture;
    m->scale = in->scale > 0 && in->scale < 600 ? in->scale : 24.0f;
    m->labelUtf8 = in->label ? in->label : "";
    m->label = GameAnsi(m->labelUtf8);
    if (m->label.size() > 100) m->label.resize(100);
    return true;
}

// Where a marker is on the screen this pass, in the HUD's units; false: not shown.
bool OnScreen(const Marker &m, const MmlRenderCamera &c, float margin, float *ux, float *uy) {
    V3 w;
    if (!Place(m, &w)) return false;
    float d[3] = {w.x - c.eye.x, w.y - c.eye.y, w.z - c.eye.z};
    float x = d[0] * c.right.x + d[1] * c.right.y + d[2] * c.right.z, y = d[0] * c.up.x + d[1] * c.up.y + d[2] * c.up.z;
    float z = d[0] * c.fwd.x + d[1] * c.fwd.y + d[2] * c.fwd.z;
    float sx = 0, sy = 0;   // camera_project's 800 x 600
    bool front = z > c.near_z * 0.5f;
    if (front) {
        sx = ((x * c.proj[0] + z * c.proj[8]) / z + 1) * 400.0f;
        sy = (1 - (y * c.proj[5] + z * c.proj[9]) / z) * 300.0f;
    }
    bool inside = front && sx >= 0 && sx <= 800 && sy >= 0 && sy <= 600;
    if (!inside) {
        if (!(m.flags & MML_MARKER_EDGE)) return false;
        float dx = front ? sx - 400 : x, dy = front ? sy - 300 : -y;   // (behind: toward its side)
        float l = std::sqrt(dx * dx + dy * dy);
        if (!(l > 1e-4f)) { dx = 0; dy = 1; l = 1; }
        dx /= l; dy /= l;
        float hx = 400 - margin, hy = 300 - margin;
        float t = std::min(std::fabs(dx) > 1e-4f ? hx / std::fabs(dx) : 1e9f, std::fabs(dy) > 1e-4f ? hy / std::fabs(dy) : 1e9f);
        sx = 400 + dx * t;
        sy = 300 + dy * t;
    }
    return DrawUnitsFromScreen(sx, sy, ux, uy);
}

void DrawMarker(const Marker &m, const MmlRenderCamera &c, const V3 *player, bool havePlayer) {
    float h = m.scale, ux, uy;
    if (!OnScreen(m, c, h * 0.6f + 4, &ux, &uy)) return;
    if (m.picture) {
        int pw = 0, ph = 0;
        HudPictureSize(m.picture, &pw, &ph);
        float w = ph > 0 ? h * pw / ph : h;
        DrawPassPicture(m.mod, m.picture, ux - w / 2, uy - h / 2, w, h, 0, m.argb);
    } else {
        float r = h / 2, o = r + 1.5f;
        const float shade[8] = {ux, uy - o, ux + o, uy, ux - o, uy, ux, uy + o};
        const float diamond[8] = {ux, uy - r, ux + r, uy, ux - r, uy, ux, uy + r};
        DrawPassQuad(m.mod, shade, 0xA0000000u);
        DrawPassQuad(m.mod, diamond, m.argb);
    }
    float ty = uy + h / 2 + 2;
    if (!m.label.empty()) {
        DrawPassText(m.mod, ux, ty, 16, m.argb, MML_FONT_SMALL, MML_DRAW_CENTER, m.label.c_str());
        ty += 16;
    }
    V3 w;
    if ((m.flags & MML_MARKER_DISTANCE) && havePlayer && Place(m, &w)) {
        char b[32];
        snprintf(b, sizeof b, "%.0f m", Len(w - *player));
        DrawPassText(m.mod, ux, ty, 14, m.argb, MML_FONT_SMALL, MML_DRAW_CENTER, b);
    }
}

Picture *FindPic(int id) {
    for (Picture &p : g_pics) if (p.id == id) return &p;
    return nullptr;
}

} // namespace

int HudPictureLoad(MmlMod *mod, const char *file, const char *alpha, std::string *err) {
    if (!game::g_exe || !E().igraph) { *err = "no game"; return 0; }
    if (!file || !*file) { *err = "file: a picture's name (maps\\...)"; return 0; }
    std::string a = alpha ? alpha : "";
    if (!mod)   // the bridge's: one each
        for (const Picture &p : g_pics) if (!p.mod && p.file == file && p.alpha == a) return p.id;
    void *ig = *reinterpret_cast<void **>(E().igraph);
    if (!ig) { *err = "the game's graphics are not up yet"; return 0; }
    void *tex = nullptr;
    int r = -1;
    auto f = [&] { r = reinterpret_cast<CreateTexFn>(Vt(ig, 0x04))(ig, file, a.empty() ? nullptr : a.c_str(), a.empty() ? 2u : 4u, &tex); };
    if (Guarded(f)) { *err = "CreateITexture faulted"; return 0; }
    if (r != 0 || !tex) {
        char b[64];
        snprintf(b, sizeof b, " (CreateITexture: 0x%08X)", static_cast<unsigned>(r));
        *err = std::string("not opened: maps\\") + file + (a.empty() ? "" : " with " + a) + b;
        Release(tex);
        return 0;
    }
    Picture p{++g_nextPic, mod, file, a, tex, 0, 0};
    auto g = [&] { p.w = game::at<uint16_t>(tex, kTexWidth); p.h = game::at<uint16_t>(tex, kTexHeight); };
    Guarded(g);
    g_pics.push_back(p);
    return p.id;
}

bool HudPictureSize(int picture, int *w, int *h) {
    Picture *p = FindPic(picture);
    if (!p) return false;
    if (w) *w = p->w;
    if (h) *h = p->h;
    return true;
}

void HudPictureFree(MmlMod *mod, int picture) {
    for (size_t i = 0; i < g_pics.size(); i++)
        if (g_pics[i].id == picture && g_pics[i].mod == mod) {
            Release(g_pics[i].tex);
            g_pics.erase(g_pics.begin() + i);
            return;
        }
}

void *HudPictureTexture(int picture) {
    Picture *p = FindPic(picture);
    return p ? p->tex : nullptr;
}

int HudConsole(const std::string &utf8, uint32_t argb, std::string *err) {
    if (!game::g_exe || !E().hud || !E().hudConsoleAdd) { *err = "no game"; return 0; }
    if (utf8.empty()) { *err = "text: the line"; return 0; }
    std::string a = GameAnsi(utf8);
    if (a.size() > 63) a.resize(63);   // (it copies 64 bytes: the line must end in them)
    auto f = [&] { reinterpret_cast<HudConsoleFn>(E().hudConsoleAdd)(E().hud, nullptr, a.c_str(), argb & 0xFFFFFFu); };
    if (Guarded(f)) { *err = "ConsoleAddText faulted"; return 0; }
    return 1;
}

int HudMarkerAdd(MmlMod *mod, const MmlHudMarker *in, std::string *err) {
    if (!HudKnown()) { *err = "no game (the HUD is not known for this build)"; return 0; }
    if (MissionState() == MML_MISSION_NONE) { *err = "no mission (markers are the mission's)"; return 0; }
    Marker m{};
    if (!Fill(&m, in, err)) return 0;
    m.id = ++g_nextMarker;
    m.mod = mod;
    g_markers.push_back(m);
    Apply(g_markers.back());
    return m.id;
}

bool HudMarkerSet(MmlMod *mod, int id, const MmlHudMarker *in, std::string *err) {
    for (Marker &m : g_markers)
        if (m.id == id && m.mod == mod) {
            Marker n = m;
            if (!Fill(&n, in, err)) return false;
            m = n;
            Apply(m);
            return true;
        }
    *err = "marker: not one of its own";
    return false;
}

bool HudMarkerRemove(MmlMod *mod, int id) {
    for (size_t i = 0; i < g_markers.size(); i++)
        if (g_markers[i].id == id && g_markers[i].mod == mod) { Drop(i); return true; }
    return false;
}

void HudPass() {
    if (g_markers.empty() || MissionState() != MML_MISSION_RUNNING) return;
    MmlRenderCamera c{};
    c.size = sizeof c;
    if (!RenderFrameCamera(&c)) return;
    V3 player;
    bool have = PlayerAt(&player);
    for (const Marker &m : g_markers)
        if ((m.flags & MML_MARKER_SCREEN) && (!m.mod || ModAlive(m.mod))) DrawMarker(m, c, &player, have);
}

void HudModGone(MmlMod *mod) {
    for (size_t i = 0; i < g_markers.size();)
        if (g_markers[i].mod == mod) Drop(i);
        else i++;
    for (size_t i = 0; i < g_pics.size();)
        if (g_pics[i].mod == mod) { Release(g_pics[i].tex); g_pics.erase(g_pics.begin() + i); }
        else i++;
}

void HudMissionClose() {
    while (!g_markers.empty()) Drop(g_markers.size() - 1);
    g_compassBy = 0;
    g_compassOurs = nullptr;
}

void HudActorGone(void *actor) {
    void *model = actor ? game::at<void *>(actor, game::kActorFrame) : nullptr;
    if (!model) return;
    for (size_t i = 0; i < g_markers.size();) {
        bool on = false;
        for (void *f = g_markers[i].frame; f && !on; f = game::at<void *>(f, game::kFrameParent)) on = f == model;
        if (on) Drop(i);
        else i++;
    }
}

bool HudMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    static const char *kMethods[] = {"hud.console", "hud.help", "hud.marker", "hud.remove", "hud.list"};
    if (std::none_of(std::begin(kMethods), std::end(kMethods), [&](const char *m) { return method == m; })) return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    auto r3 = [](const V3 &v) { return json::array({std::round(v.x * 100) / 100, std::round(v.y * 100) / 100, std::round(v.z * 100) / 100}); };
    auto colour = [&](uint32_t dflt) -> uint32_t {
        if (!p.contains("color")) return dflt;
        const json &c = p["color"];
        if (c.is_number()) return static_cast<uint32_t>(c.get<int64_t>());
        if (!c.is_string()) return dflt;
        std::string s = c.get<std::string>();
        if (!s.empty() && s[0] == '#') s.erase(0, 1);
        uint32_t v = static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 16));
        return s.size() <= 6 ? v | 0xFF000000u : v;
    };
    std::string err;
    try {
        if (method == "hud.console") {
            if (!HudConsole(p.value("text", ""), colour(0xFFFFFFFFu), &err)) { *reply = "hud.console: " + err; return true; }
            *reply = dump({{"shown", true}});
        } else if (method == "hud.help") {
            std::string text = p.value("text", "");
            if (text.empty()) { *reply = "hud.help: text: the line"; return true; }
            int ms = HudHelpLine(text);
            if (!ms) { *reply = "hud.help: not shown (no game, or the game's text database is not hooked)"; return true; }
            *reply = dump({{"ms", ms}});
        } else if (method == "hud.list") {
            json list = json::array(), pics = json::array();
            for (const Marker &m : g_markers) {
                json j = {{"id", m.id}, {"by", m.mod ? ModIdOf(m.mod) : "bridge"}, {"label", m.labelUtf8}, {"flags", m.flags},
                          {"radar", m.radar != nullptr}, {"compass", g_compassBy == m.id}};
                V3 w;
                if (Place(m, &w)) j["at"] = r3(w);
                list.push_back(j);
            }
            for (const Picture &pc : g_pics)
                pics.push_back({{"id", pc.id}, {"by", pc.mod ? ModIdOf(pc.mod) : "bridge"}, {"file", pc.file}, {"alpha", pc.alpha}, {"w", pc.w}, {"h", pc.h}});
            json out = {{"markers", list}, {"pictures", pics}, {"compass_by", g_compassBy}};
            if (HudKnown()) {
                auto f = [&] {
                    out["hud_flags"] = game::at<uint32_t>(reinterpret_cast<void *>(E().hud), E().hudFlags);
                    char b[16];
                    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(CompassField())));
                    out["compass_target"] = b;
                };
                Guarded(f);
            }
            *reply = dump(out);
        } else if (method == "hud.remove") {
            size_t n = 0;
            if (p.value("all", false)) {
                for (size_t i = 0; i < g_markers.size();)
                    if (!g_markers[i].mod) { Drop(i); n++; }
                    else i++;
            } else if (HudMarkerRemove(nullptr, p.value("id", 0))) {
                n = 1;
            } else {
                *reply = "hud.remove: id: one of the bridge's markers (hud.list), or all";
                return true;
            }
            *reply = dump({{"removed", n}});
        } else {   // hud.marker
            MmlHudMarker m{};
            m.size = sizeof m;
            int id = p.value("id", 0);
            const Marker *old = nullptr;
            for (const Marker &k : g_markers) if (id && k.id == id && !k.mod) old = &k;
            if (id && !old) { *reply = "hud.marker: id: one of the bridge's markers (hud.list)"; return true; }
            V3 pos = old ? old->pos : V3{0, 0, 0};
            void *frame = old ? old->frame : nullptr;
            if (p.contains("on") && p["on"].is_string()) {
                void *actor = nullptr;
                if (!ParseActor(p["on"].get<std::string>(), &actor, &err)) { *reply = "hud.marker: on: " + err; return true; }
                frame = ActorFrame(actor);
                if (!frame) { *reply = "hud.marker: on: the actor has no model"; return true; }
                if (p.contains("bone") && p["bone"].is_string()) {
                    void *b = game::FindChild(frame, p["bone"].get<std::string>().c_str());
                    if (!b) { *reply = "hud.marker: bone: not in its model"; return true; }
                    frame = b;
                }
                pos = {0, 0, 0};
                if (p.contains("pos") && p["pos"].is_array() && p["pos"].size() == 3) pos = {p["pos"][0].get<float>(), p["pos"][1].get<float>(), p["pos"][2].get<float>()};
            } else if (p.contains("at") && p["at"].is_array() && p["at"].size() == 3) {
                pos = {p["at"][0].get<float>(), p["at"][1].get<float>(), p["at"][2].get<float>()};
                frame = nullptr;
            } else if (!old) {
                *reply = "hud.marker: at [x, y, z], or on an actor (\"car\", \"player\", \"0x...\") with bone, pos; label, color, picture (a file of maps\\), scale, screen, distance, edge (default true), radar, compass";
                return true;
            }
            m.pos[0] = pos.x; m.pos[1] = pos.y; m.pos[2] = pos.z;
            m.frame = frame;
            auto flag = [&](const char *key, uint32_t bit, bool dflt) {
                bool was = old ? (old->flags & bit) != 0 : dflt;
                if (p.value(key, was)) m.flags |= bit;
            };
            flag("screen", MML_MARKER_SCREEN, true);
            flag("distance", MML_MARKER_DISTANCE, true);
            flag("edge", MML_MARKER_EDGE, true);
            flag("radar", MML_MARKER_RADAR, false);
            flag("compass", MML_MARKER_COMPASS, false);
            m.argb = colour(old ? old->argb : 0xFFFFFFFFu);
            m.scale = p.value("scale", old ? old->scale : 24.0f);
            std::string label = p.contains("label") ? p.value("label", "") : (old ? old->labelUtf8 : "");
            m.label = label.c_str();
            m.picture = old ? old->picture : 0;
            if (p.contains("picture")) {
                std::string file = p.value("picture", "");
                m.picture = file.empty() ? 0 : HudPictureLoad(nullptr, file.c_str(), p.value("alpha", "").c_str(), &err);
                if (!file.empty() && !m.picture) { *reply = "hud.marker: picture: " + err; return true; }
            }
            if (old) {
                if (!HudMarkerSet(nullptr, id, &m, &err)) { *reply = "hud.marker: " + err; return true; }
            } else if (!(id = HudMarkerAdd(nullptr, &m, &err))) {
                *reply = "hud.marker: " + err;
                return true;
            }
            *reply = dump({{"id", id}, {"at", r3(pos)}, {"flags", m.flags}});
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
