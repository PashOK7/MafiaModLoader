// Drawing for mods and agents: text, lines and rectangles in the game's own frame.
//
// The game draws its 2D things (the HUD, the open menu, the console) in one place: the callback it gives its scene
// (game_build.h `draw2d`), which LS3D's render calls with stage 2 once the 3D frame is drawn, in every state that
// renders the scene (the game, the menus over it, the pause): first the HUD (G_IndicatorsClass::DrawAll, game_build.h
// hudDrawAll), then the open menu, then the console. The loader draws what was queued as a layer of the HUD's: right
// after the HUD (its hook of DrawAll), so the game's menus and console are drawn over the mods' drawing as over the
// HUD, and a mod's overlay stays on the screen under the pause menu (a pass with no HUD drawn: at the pass's end).
// It draws with the game's own means: its text function (G_IndicatorsClass::OutText, the
// fonts of the HUD and the menus) and the two IGraph calls its rectangles are made of (SetTexture, DrawPrimitive
// with screen-space vertices). So the drawing is in the back buffer before the frame is shown (`screenshot` sees
// it), under plain Direct3D 8 and under a wrapper alike: no device call of our own, no state of the device touched
// behind the engine's back (the three engine states set here, the ones the HUD sets for itself, are put back).
//
// Coordinates are the menus' virtual units: 800 x 600 whatever the resolution; the game's own scale (and its x
// offset) turn them into pixels, as for the HUD.
//
// Mods queue items every frame (mml.h draw_*): they are drawn in the frame being made and dropped at the start of
// the next loader frame. The bridge's items (draw.text, draw.rect, draw.line: for agents, without a mod) stay for a
// time (`ms`). Each owner's items are drawn behind a fault guard: a fault turns that mod's drawing off until it is
// reloaded (the bridge's: its items are dropped) and is logged; the game goes on.
//
// Not drawn: while the game loads (its loading screen is not the scene's 2D pass, and the loader has no frames
// then) and during the intro videos.
//
// Pictures (hud.cpp's textures, opened by the game's own loader) are drawn as the game draws its textured rectangles
// (SetTexture, then the strip), turned about their centre in pixels; the host's markers (hud.cpp) are queued at each
// 2D pass, projected with the camera of the frame being drawn.
//
// Bridge methods (game thread): draw.text {x, y, text, size, color, font, align, ms}, draw.rect {x, y, w, h, color,
// filled, ms}, draw.line {x0, y0, x1, y1, color, ms}, draw.picture {file, alpha, x, y, w, h, angle, color, uv, ms},
// draw.clear, draw.state.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "json.hpp"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

enum Kind { kText, kLine, kRect, kFrame, kPicture, kQuad };
struct Item {                // plain data: drawn inside __try
    MmlMod *owner;           // null: the host's (the bridge's items)
    int kind;
    float a, b, c, d;        // text: x, y, size; line: x0, y0, x1, y1; rectangles, pictures: x, y, w, h
    uint32_t argb;
    int font, align;
    DWORD until;             // the bridge's: the tick count it goes at
    int picture;             // a picture's handle (hud.cpp): its texture is looked up when it is drawn
    float angle;             // a picture's turn, degrees clockwise
    float p[8];              // a picture's uv (u0, v0, u1, v1); a quad's corners (x, y four times, a strip's order)
    char text[128];
};
std::vector<Item> g_frame;   // the mods', for the frame being made
std::vector<Item> g_pass;    // the host's for this 2D pass (hud.cpp's markers: projected with this frame's camera)
std::vector<Item> g_held;    // the bridge's
std::vector<MmlMod *> g_off; // mods whose drawing faulted (until they are reloaded: a reload is a new MmlMod)
bool g_hooked;
unsigned g_queuedLast, g_drawnLast;   // the last frame's: items queued by mods, items drawn in the last 2D pass
uint64_t g_passes;                    // 2D passes seen
std::string g_lastFault;
constexpr size_t kMaxItems = 4096;

using Draw2dFn = int(__stdcall *)(int stage, int a, int b);
Draw2dFn g_draw2d;

struct Vertex { float x, y, z, rhw; uint32_t diffuse, specular; float u, v; };   // the game's 2D vertex (FVF 0x1C4)
struct Space { float sx, sy, ox; };

// The space mods draw in (mml.h): 800 x 600 units over the HUD's area, the whole screen (1.2's middle screen of three
// past its x offset, hudXOffset), whatever the HUD's own units are: the game's are 1/800 of that width, but a mod
// that lays the HUD out at the screen's aspect (mods/fusion_fix) makes them square, 600 high and wider than 800 on a
// wide screen, and the mods' 800 would end short of the screen's right edge. The screen's width in pixels is the
// float before the HUD's scale (G_IndicatorsClass::ResolutionChanged writes both). False before the HUD is set up.
bool ModSpace(const game::ExeBuild &e, Space *s) {
    const float *scale = reinterpret_cast<const float *>(e.hud + e.hudScale);
    float ox = e.hudXOffset ? *reinterpret_cast<const float *>(e.hudXOffset) : 0.0f;
    float width = scale[-2];
    if (!(scale[0] > 0.01f && scale[0] < 100.0f && scale[1] > 0.01f && scale[1] < 100.0f && width > 2.0f * ox + 1.0f)) return false;
    *s = Space{(width - 2.0f * ox) / 800.0f, scale[1], ox};
    return true;
}

template <class F> F Vt(void *obj, int off) { return *reinterpret_cast<F *>(*reinterpret_cast<uint8_t **>(obj) + off); }
using SetTextureFn = int(__stdcall *)(void *igraph, void *texture);
using DrawPrimitiveFn = int(__stdcall *)(void *igraph, int type, int count, const void *vertices, int format);
using SetStateFn = int(__stdcall *)(void *self, int id, int value);
using GetStateFn = int(__stdcall *)(void *self, int id);
enum { kPrimLineList = 1, kPrimLineStrip = 2, kPrimTriStrip = 5 };

bool Usable(float v) { return v == v && v > -1.0e6f && v < 1.0e6f; }

bool Off(const MmlMod *mod) { return std::find(g_off.begin(), g_off.end(), mod) != g_off.end(); }

Vertex V(float x, float y, uint32_t argb, float u = 0.0f, float v = 0.0f) { return Vertex{x, y, 0.0f, 1.0f, argb, 0xFF000000u, u, v}; }

void DrawOne(const Item &it, void *ig, const Space &s) {
    const game::ExeBuild &e = game::Exe();
    if (it.kind == kText) {
        using OutTextFn = float(__fastcall *)(uintptr_t hud, void *edx, const char *text, float x, float y, float w, float h,
                                              uint32_t argb, int flags, int font, const char *end);
        reinterpret_cast<OutTextFn>(e.hudOutText)(e.hud, nullptr, it.text, it.a * s.sx + s.ox, it.b * s.sy, 0.0f, it.c * s.sy,
                                                  it.argb, it.align, it.font, nullptr);
        return;
    }
    if (it.kind == kPicture) {
        void *tex = HudPictureTexture(it.picture);
        if (!tex) return;   // (freed since it was queued)
        Vt<SetTextureFn>(ig, 0x0C)(ig, tex);
        // turned about its centre in pixels (the units are not square away from 4:3)
        float cx = (it.a + it.c * 0.5f) * s.sx + s.ox, cy = (it.b + it.d * 0.5f) * s.sy, hw = it.c * 0.5f * s.sx, hh = it.d * 0.5f * s.sy;
        float r = it.angle * 0.017453293f, cs = std::cos(r), sn = std::sin(r);
        auto at = [&](float x, float y, float u, float v) { return V(cx + x * cs - y * sn, cy + x * sn + y * cs, it.argb, u, v); };
        Vertex v[4] = {at(-hw, -hh, it.p[0], it.p[1]), at(hw, -hh, it.p[2], it.p[1]), at(-hw, hh, it.p[0], it.p[3]), at(hw, hh, it.p[2], it.p[3])};
        Vt<DrawPrimitiveFn>(ig, 0x58)(ig, kPrimTriStrip, 2, v, 1);
        return;
    }
    Vt<SetTextureFn>(ig, 0x0C)(ig, nullptr);
    if (it.kind == kQuad) {
        Vertex v[4];
        for (int k = 0; k < 4; k++) v[k] = V(it.p[2 * k] * s.sx + s.ox, it.p[2 * k + 1] * s.sy, it.argb);
        Vt<DrawPrimitiveFn>(ig, 0x58)(ig, kPrimTriStrip, 2, v, 1);
        return;
    }
    if (it.kind == kLine) {
        Vertex v[2] = {V(it.a * s.sx + s.ox, it.b * s.sy, it.argb), V(it.c * s.sx + s.ox, it.d * s.sy, it.argb)};
        Vt<DrawPrimitiveFn>(ig, 0x58)(ig, kPrimLineList, 1, v, 1);
        return;
    }
    float x0 = it.a * s.sx + s.ox, y0 = it.b * s.sy, x1 = (it.a + it.c) * s.sx + s.ox, y1 = (it.b + it.d) * s.sy;
    if (it.kind == kRect) {
        Vertex v[4] = {V(x0, y0, it.argb), V(x1, y0, it.argb), V(x0, y1, it.argb), V(x1, y1, it.argb)};
        Vt<DrawPrimitiveFn>(ig, 0x58)(ig, kPrimTriStrip, 2, v, 1);
    } else {
        Vertex v[5] = {V(x0, y0, it.argb), V(x1, y0, it.argb), V(x1, y1, it.argb), V(x0, y1, it.argb), V(x0, y0, it.argb)};
        Vt<DrawPrimitiveFn>(ig, 0x58)(ig, kPrimLineStrip, 4, v, 1);
    }
}

// One owner's items (those of `items` that are `owner`'s); the exception's code when the drawing faulted.
DWORD SafeDrawOwner(const Item *items, size_t n, const MmlMod *owner, void *ig, const Space *s, unsigned *drawn) {
    __try {
        for (size_t i = 0; i < n; i++)
            if (items[i].owner == owner) { DrawOne(items[i], ig, *s); ++*drawn; }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

// The engine's state for 2D drawing set as the HUD sets it for itself (G_IndicatorsClass::DrawAll: IGraph's state 0,
// the blending, to 1; the driver's state 3 to 0), every owner's items drawn, the state put back.
DWORD SafeDrawAll(const Item *frame, size_t nFrame, const Item *pass, size_t nPass, const Item *held, size_t nHeld, MmlMod **owners,
                  size_t nOwners, DWORD *ownerFault, unsigned *drawn) {
    __try {
        const game::ExeBuild &e = game::Exe();
        void *ig = *reinterpret_cast<void **>(e.igraph), *drv = *reinterpret_cast<void **>(e.i3dDriver);
        if (!ig || !drv) return 0;
        Space s;
        if (!ModSpace(e, &s)) return 0;   // the HUD is not set up yet
        int blend = Vt<GetStateFn>(ig, 0x48)(ig, 0), func = Vt<GetStateFn>(ig, 0x48)(ig, 1);
        int state3 = Vt<GetStateFn>(drv, 0x18)(drv, 3);
        Vt<SetStateFn>(drv, 0x14)(drv, 3, 0);
        Vt<SetStateFn>(ig, 0x44)(ig, 0, 1);
        Vt<SetStateFn>(ig, 0x44)(ig, 1, 0);
        for (size_t k = 0; k < nOwners; k++) {
            DWORD f = SafeDrawOwner(frame, nFrame, owners[k], ig, &s, drawn);
            if (!f) f = SafeDrawOwner(pass, nPass, owners[k], ig, &s, drawn);
            if (!f && !owners[k]) f = SafeDrawOwner(held, nHeld, nullptr, ig, &s, drawn);
            ownerFault[k] = f;
        }
        Vt<SetStateFn>(ig, 0x44)(ig, 1, func);
        Vt<SetStateFn>(ig, 0x44)(ig, 0, blend);
        Vt<SetStateFn>(drv, 0x14)(drv, 3, state3);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

void DrawQueued() {
    g_passes++;
    g_drawnLast = 0;
    g_pass.clear();
    HudPass();   // (hud.cpp: the markers, where this frame's camera puts them)
    if (g_frame.empty() && g_pass.empty() && g_held.empty()) return;
    std::vector<MmlMod *> owners;   // in the order they first queued; the bridge's items last (on top)
    for (const std::vector<Item> *v : {&g_pass, &g_frame})
        for (const Item &it : *v)
            if (it.owner && std::find(owners.begin(), owners.end(), it.owner) == owners.end()) owners.push_back(it.owner);
    owners.push_back(nullptr);
    std::vector<DWORD> faults(owners.size(), 0);
    DWORD ex = SafeDrawAll(g_frame.data(), g_frame.size(), g_pass.data(), g_pass.size(), g_held.data(), g_held.size(), owners.data(),
                           owners.size(), faults.data(), &g_drawnLast);
    if (ex) {
        // not in an item: in the state calls around them. Nobody's fault but ours: say it once and stop drawing.
        char b[96];
        snprintf(b, sizeof b, "exception %08X while setting the engine's 2D state", static_cast<unsigned>(ex));
        if (g_lastFault != b) Log(MML_LOG_ERROR, "host", "draw: %s; nothing is drawn", b);
        g_lastFault = b;
        g_frame.clear();
        g_pass.clear();
        g_held.clear();
        return;
    }
    for (size_t k = 0; k < owners.size(); k++) {
        if (!faults[k]) continue;
        char b[160];
        snprintf(b, sizeof b, "exception %08X while drawing %s's items", static_cast<unsigned>(faults[k]), ModIdOf(owners[k]));
        if (owners[k]) {
            g_off.push_back(owners[k]);
            Log(MML_LOG_ERROR, "host", "draw: %s: its drawing is off until it is reloaded", b);
        } else {
            g_held.clear();   // (and the host's markers are drawn again next pass: said once)
            if (g_lastFault != b) Log(MML_LOG_ERROR, "host", "draw: %s: the bridge's items are dropped", b);
        }
        g_lastFault = b;
    }
}

// The 2D pass's layers: the HUD, the mods' drawing (here, once a pass), the open menu, the console.
bool g_inPass;      // inside a stage 2 of the frame's 2D callback
bool g_drewPass;    // the queue was drawn in this pass (after the HUD)
using DrawAllFn = int(__fastcall *)(void *hud, void *edx);
DrawAllFn g_drawAll;
bool g_drawAllHooked;
int __fastcall DrawAllHook(void *hud, void *edx) {
    const int r = HudGameHidden() ? 0 : g_drawAll(hud, edx);   // (hud.cpp hud_game: the game's HUD hidden)
    if (g_inPass && !g_drewPass) {
        g_drewPass = true;
        DrawQueued();
    }
    return r;
}

int __stdcall Draw2dHook(int stage, int a, int b) {
    bool ui = stage == 2 && GfxUiBegin();   // (gfx.cpp: a mod takes the 2D pass: into its target)
    if (stage == 2) { g_inPass = true; g_drewPass = false; }
    int r = g_draw2d(stage, a, b);
    if (stage == 2) {
        if (!g_drewPass) DrawQueued();   // (no HUD drawn in this pass: over what the game drew, as before)
        g_inPass = false;
    }
    if (ui) GfxUiEnd();
    RenderStage(stage);   // (render.cpp: stage 1, the 3D world drawn: MML_EVENT_RENDER_WORLD)
    return r;
}

bool Queue(std::vector<Item> *to, const Item &it) {
    if (to->size() >= kMaxItems) return false;
    if (!Usable(it.a) || !Usable(it.b) || !Usable(it.c) || !Usable(it.d)) return false;
    to->push_back(it);
    return true;
}

// A mod's item for this frame; one past the frame's kMaxItems (every mod's together) is dropped, counted, and the
// mod told once in the log (else a mod drawing much hides what it, or another mod, draws after: debug_views' triggers'
// areas once took the whole frame and its detectors were not drawn, with no word).
unsigned g_droppedNow, g_droppedLast;
std::vector<MmlMod *> g_told;   // mods told that the frame was full
void QueueMod(MmlMod *mod, const Item &it) {
    if (g_frame.size() < kMaxItems) { Queue(&g_frame, it); return; }
    g_droppedNow++;
    if (std::find(g_told.begin(), g_told.end(), mod) != g_told.end()) return;
    g_told.push_back(mod);
    Log(MML_LOG_WARN, "host", "draw: mod %s: the frame's %zu items (every mod's) are used: what it draws past them is "
        "dropped (draw.state `dropped_last_frame`)", ModIdOf(mod), kMaxItems);
}

// The game's fonts draw its texts' code page (GameCodePage); the mods and the bridge give UTF-8. A text that is UTF-8
// with more than ASCII in it is turned into the game's; anything else (ASCII, or a text already in the game's code
// page, as mml.h asked before 2026-10-17: its bytes past 0x7F are seldom valid UTF-8) is drawn as it is.
std::string ForFont(const char *text) {
    if (!text) return std::string();
    bool ascii = true;
    for (const char *p = text; *p && ascii; ++p) ascii = static_cast<unsigned char>(*p) < 0x80;
    if (ascii) return text;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (n <= 0) return text;
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &w[0], n);
    int m = WideCharToMultiByte(GameCodePage(), 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (m <= 1) return text;
    std::string s(m, '\0');
    WideCharToMultiByte(GameCodePage(), 0, w.c_str(), -1, &s[0], m, nullptr, nullptr);
    s.pop_back();
    return s;
}

Item TextItem(MmlMod *mod, float x, float y, float size, uint32_t argb, int font, int align, const char *text) {
    Item it{};
    it.owner = mod;
    it.kind = kText;
    it.a = x; it.b = y; it.c = size;
    it.argb = argb;
    it.font = font < 0 || font >= MML_FONT_COUNT_ ? MML_FONT_SMALL : font;
    it.align = align == MML_DRAW_CENTER || align == MML_DRAW_RIGHT ? align : MML_DRAW_LEFT;
    strncpy(it.text, text ? text : "", sizeof it.text - 1);
    return it;
}

Item PictureItem(MmlMod *mod, int picture, float x, float y, float w, float h, float angle, uint32_t argb, const float *uv) {
    Item it{};
    it.owner = mod;
    it.kind = kPicture;
    it.picture = picture;
    it.a = x; it.b = y; it.c = w; it.d = h;
    it.angle = angle == angle ? angle : 0.0f;
    it.argb = argb;
    const float all[4] = {0, 0, 1, 1};
    memcpy(it.p, uv ? uv : all, sizeof all);
    return it;
}

Item ShapeItem(MmlMod *mod, int kind, float a, float b, float c, float d, uint32_t argb) {
    Item it{};
    it.owner = mod;
    it.kind = kind;
    it.a = a; it.b = b; it.c = c; it.d = d;
    it.argb = argb;
    return it;
}

float SafeTextWidth(int font, float size, const char *text) {
    __try {
        const game::ExeBuild &e = game::Exe();
        using TextSizeFn = float(__fastcall *)(uintptr_t hud, void *edx, const char *text, float size, int font, const char *end);
        Space s;
        if (!ModSpace(e, &s)) return 0.0f;
        float factor = *reinterpret_cast<const float *>(e.hud + e.hudFonts + font * e.hudFontSize + 8);
        return reinterpret_cast<TextSizeFn>(e.hudTextSize)(e.hud, nullptr, text, size * s.sy * factor, font, nullptr) / s.sx;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
}

// 0xAARRGGBB from "#RRGGBB", "#AARRGGBB" or a number; white when absent.
uint32_t Colour(const json &p, const char *key) {
    if (!p.contains(key)) return 0xFFFFFFFFu;
    const json &c = p[key];
    if (c.is_number()) return static_cast<uint32_t>(c.get<int64_t>());
    if (!c.is_string()) return 0xFFFFFFFFu;
    std::string s = c.get<std::string>();
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    else if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.erase(0, 2);
    uint32_t v = static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 16));
    return s.size() <= 6 ? v | 0xFF000000u : v;
}

} // namespace

void DrawInit() {
    if (!game::g_exe || !game::Exe().draw2d) return;
    std::string err;
    g_hooked = HookAdd(nullptr, game::Exe().draw2d, reinterpret_cast<void *>(&Draw2dHook), reinterpret_cast<void **>(&g_draw2d), &err);
    if (!g_hooked) HWARN("draw: cannot hook the frame's 2D pass: %s", err.c_str());
    if (!g_hooked || !game::Exe().hudDrawAll) return;   // (without it: the mods' drawing over the menus, at the pass's end)
    g_drawAllHooked = HookAdd(nullptr, game::Exe().hudDrawAll, reinterpret_cast<void *>(&DrawAllHook), reinterpret_cast<void **>(&g_drawAll), &err);
    if (!g_drawAllHooked) HWARN("draw: cannot hook the HUD's DrawAll (the mods' drawing goes over the menus): %s", err.c_str());
}

void DrawFrame() {
    g_queuedLast = static_cast<unsigned>(g_frame.size());
    g_frame.clear();
    g_droppedLast = g_droppedNow;
    g_droppedNow = 0;
    g_told.erase(std::remove_if(g_told.begin(), g_told.end(), [](MmlMod *m) { return !ModAlive(m); }), g_told.end());
    DWORD now = GetTickCount();
    g_held.erase(std::remove_if(g_held.begin(), g_held.end(), [&](const Item &it) { return static_cast<int>(it.until - now) <= 0; }), g_held.end());
    // a mod reloaded is a new MmlMod: the old ones' marks go once they are dead
    g_off.erase(std::remove_if(g_off.begin(), g_off.end(), [](MmlMod *m) { return !ModAlive(m); }), g_off.end());
}

void DrawText(MmlMod *mod, float x, float y, float size, uint32_t argb, int font, int align, const char *text) {
    if (!mod || Off(mod) || !text || !*text || !(size > 0.0f)) return;
    QueueMod(mod, TextItem(mod, x, y, size, argb, font, align, ForFont(text).c_str()));
}

float DrawTextWidth(int font, float size, const char *text) {
    if (!g_hooked || !text || !(size > 0.0f) || font < 0 || font >= MML_FONT_COUNT_) return 0.0f;
    return SafeTextWidth(font, size, ForFont(text).c_str());
}

void DrawLine(MmlMod *mod, float x0, float y0, float x1, float y1, uint32_t argb) {
    if (!mod || Off(mod)) return;
    QueueMod(mod, ShapeItem(mod, kLine, x0, y0, x1, y1, argb));
}

void DrawRect(MmlMod *mod, float x, float y, float w, float h, uint32_t argb, int filled) {
    if (!mod || Off(mod)) return;
    QueueMod(mod, ShapeItem(mod, filled ? kRect : kFrame, x, y, w, h, argb));
}

void DrawPicture(MmlMod *mod, int picture, float x, float y, float w, float h, float angle, uint32_t argb, const float uv[4]) {
    if (!mod || Off(mod) || picture <= 0) return;
    QueueMod(mod, PictureItem(mod, picture, x, y, w, h, angle, argb, uv));
}

// The host's items for this 2D pass (hud.cpp, from HudPass): drawn under their owner's fault guard, dropped after it.
void DrawPassText(MmlMod *owner, float x, float y, float size, uint32_t argb, int font, int align, const char *text) {
    if (Off(owner) || !text || !*text) return;
    Queue(&g_pass, TextItem(owner, x, y, size, argb, font, align, text));
}
void DrawPassPicture(MmlMod *owner, int picture, float x, float y, float w, float h, float angle, uint32_t argb) {
    if (Off(owner)) return;
    Queue(&g_pass, PictureItem(owner, picture, x, y, w, h, angle, argb, nullptr));
}
void DrawPassQuad(MmlMod *owner, const float xy[8], uint32_t argb) {
    if (Off(owner)) return;
    Item it = ShapeItem(owner, kQuad, xy[0], xy[1], xy[6], xy[7], argb);
    memcpy(it.p, xy, sizeof it.p);
    Queue(&g_pass, it);
}
// A point of camera_project's (800 x 600 over the whole viewport) in the mods' drawing units (800 x 600 of the HUD's
// area, the middle screen where the game adds an x offset: ModSpace); false before the HUD is set up.
bool DrawUnitsFromScreen(float x, float y, float *ux, float *uy) {
    __try {
        const game::ExeBuild &e = game::Exe();
        Space s;
        if (!ModSpace(e, &s)) return false;
        float width = reinterpret_cast<const float *>(e.hud + e.hudScale)[-2];   // (the screen's pixels)
        *ux = (x / 800.0f * width - s.ox) / s.sx;
        *uy = y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void DrawRemove(MmlMod *mod) {
    g_frame.erase(std::remove_if(g_frame.begin(), g_frame.end(), [&](const Item &it) { return it.owner == mod; }), g_frame.end());
}

bool DrawMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method.compare(0, 5, "draw.")) return false;
    *ok = false;
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "draw.state") {
            json off = json::array();
            for (MmlMod *m : g_off) off.push_back(ModIdOf(m));
            *reply = dump({{"hooked", g_hooked}, {"under_menus", g_drawAllHooked}, {"passes", g_passes}, {"queued_last_frame", g_queuedLast},
                           {"dropped_last_frame", g_droppedLast}, {"max_items", kMaxItems}, {"drawn_last_pass", g_drawnLast}, {"held", g_held.size()}, {"off", off}, {"last_fault", g_lastFault}});
            *ok = true;
            return true;
        }
        if (method == "draw.clear") {
            size_t n = g_held.size();
            g_held.clear();
            *reply = dump({{"cleared", n}});
            *ok = true;
            return true;
        }
        int ms = p.value("ms", 3000);
        if (ms <= 0 || ms > 600000) { *reply = "ms: 1..600000 (how long it stays; default 3000)"; return true; }
        Item it{};
        if (method == "draw.text") {
            std::string text = p.value("text", "");
            if (text.empty()) { *reply = "text: what to write; x, y (800x600 units), size (default 20), color (\"#AARRGGBB\"), font (0 small, 1 big, 2 title), align (\"left\", \"center\", \"right\"), ms"; return true; }
            std::string align = p.value("align", "left");
            it = TextItem(nullptr, p.value("x", 0.0f), p.value("y", 0.0f), p.value("size", 20.0f), Colour(p, "color"), p.value("font", 0),
                          align == "center" ? MML_DRAW_CENTER : align == "right" ? MML_DRAW_RIGHT : MML_DRAW_LEFT, ForFont(text.c_str()).c_str());
            if (!(it.c > 0.0f)) { *reply = "size: above 0"; return true; }
        } else if (method == "draw.rect") {
            it = ShapeItem(nullptr, p.value("filled", true) ? kRect : kFrame, p.value("x", 0.0f), p.value("y", 0.0f), p.value("w", 0.0f),
                           p.value("h", 0.0f), Colour(p, "color"));
        } else if (method == "draw.line") {
            it = ShapeItem(nullptr, kLine, p.value("x0", 0.0f), p.value("y0", 0.0f), p.value("x1", 0.0f), p.value("y1", 0.0f), Colour(p, "color"));
        } else if (method == "draw.picture") {
            std::string file = p.value("file", ""), err;
            if (file.empty()) { *reply = "file: a picture of the game's maps\\ (\"2sight.bmp\", a mod's files\\Maps\\...); alpha (its \"+\" twin), x, y, w, h (800x600 units), angle, color, uv [u0, v0, u1, v1], ms"; return true; }
            int pic = HudPictureLoad(nullptr, file.c_str(), p.value("alpha", "").c_str(), &err);
            if (!pic) { *reply = "draw.picture: " + err; return true; }
            float uv[4] = {0, 0, 1, 1};
            if (p.contains("uv") && p["uv"].is_array() && p["uv"].size() == 4)
                for (int k = 0; k < 4; k++) uv[k] = p["uv"][k].get<float>();
            int pw = 0, ph = 0;
            HudPictureSize(pic, &pw, &ph);
            it = PictureItem(nullptr, pic, p.value("x", 0.0f), p.value("y", 0.0f), p.value("w", static_cast<float>(pw)), p.value("h", static_cast<float>(ph)),
                             p.value("angle", 0.0f), Colour(p, "color"), uv);
        } else {
            *reply = "unknown method '" + method + "' (draw.text, draw.rect, draw.line, draw.picture, draw.clear, draw.state)";
            return true;
        }
        it.until = GetTickCount() + static_cast<DWORD>(ms);
        if (!Queue(&g_held, it)) { *reply = "not queued: too many items held (draw.clear), or a coordinate is not a usable number"; return true; }
        json out = {{"held", g_held.size()}, {"ms", ms}, {"hooked", g_hooked}};
        if (!g_hooked) out["note"] = "the frame's 2D pass is not hooked (not a known build): nothing will be drawn";
        *reply = dump(out);
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
