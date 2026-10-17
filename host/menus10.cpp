// The game's own menus in 1.0, driven by agents and extended for mods: what start.cpp and menus.cpp do with 1.2's
// GM_Menu classes, done with 1.0's screens (docs/VERSIONS.md; how they work: knowledge/notes/interface.md, "1.0").
//
// 1.0's screens have no command handler. Every screen's Tick (its vtable's slot 5) calls the base class's
// gui_menu_tick, which walks the keys and the mouse and returns the id of the control a click or Enter activated,
// and then switches on that id itself. So the loader hooks gui_menu_tick:
// - a command for a screen (menu.command, the start) is that tick's answer the next time the screen ticks: the
//   screen does with it what a click on that control does;
// - the first time a screen ticks (its constructor has laid it out by then), the mods' items asked for it are added
//   by the game's own gui_menu_clone_component_under (how the profile list grows): a copy of the control asked
//   for, right under it; the rows below are moved down and the windows around them grown by a row;
// - the id of an item the tick answers is delivered to its mod, and the screen gets 0 (nothing was activated).
// A page (mml.h menu_open) is a screen made as the game makes its own: its operator new, the base constructor, a
// vtable of the loader's (the base class's, with its own Tick), its controls built by the game's own builder from a
// design of the loader's (records as tables\menu.def has them), then made the active screen over the open one. Its
// Tick hands the activated ids (and the controls that changed) to its mod; Esc closes it.
//
// Addresses and layouts: game_build.h (`m10*`); the kinds and the screens: the tables below (st0rm94's 1.0 reverse,
// each read in 1.0's code).
#include "menus_shared.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include "json.hpp"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

// A screen: +0x18 the one under it, +0x20/+0x24 its controls, +0x2C the focused one, +0x34/+0x38 the cursor it
// last saw (ints, pixels), +0x40 flags, +0x44 the id it gives its parent when it closes.
enum { kScrUnder = 0x18, kScrComps = 0x20, kScrCompsEnd = 0x24, kScrFocus = 0x2C, kScrMouse = 0x34, kScrFlags = 0x40,
       kScrId = 0x44, kPageSize = 0x60 };
enum { kSlots = 6, kSlotDestroy = 2, kSlotCloseChild = 3, kSlotTick = 5 };
// A control: +4 id, +8 flags (1 selectable, 0x10 focused, 0x20 to be laid out, 0x100 hidden), +0xC text id, +0x10
// x, y, w, h in pixels from its parent's client origin, +0x20 its record's flags word and +0x24 its colour word (each
// kind its own use: a text's font and colour, a check box's state, a number's value), +0x2C a float (a slider's,
// a bar's value), +0x30 the text drawn, +0x34 its parent; a container's client origin (pixels on the screen) +0x38.
enum { kCtlId = 0x04, kCtlFlags = 0x08, kCtlTextId = 0x0C, kCtlX = 0x10, kCtlY = 0x14, kCtlW = 0x18, kCtlH = 0x1C, kCtlRec = 0x20,
       kCtlInt = 0x24, kCtlFloat = 0x2C, kCtlText = 0x30, kCtlParent = 0x34, kCtlOriginX = 0x38, kCtlOriginY = 0x3C };
enum { kCtlSlotForceUpdate = 3, kCtlSlotSetText = 8, kCtlSlotGetText = 9 };
enum : uint32_t { kFlagSelectable = 1, kFlagFocused = 0x10, kFlagHidden = 0x100 };
constexpr uint32_t kTagStop = 0x73746F70;    // "stop": a design's end
constexpr uint32_t kPageScreenId = 0x4D4D4C50;

constexpr uint32_t Tag(const char *t) {
    return static_cast<uint32_t>(static_cast<uint8_t>(t[0])) << 24 | static_cast<uint32_t>(static_cast<uint8_t>(t[1])) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(t[2])) << 8 | static_cast<uint8_t>(t[3]);
}

// The kinds by their vtables (gui_widget_create_from_design's arms; "ltxt" makes a "stxt"; "comp" is the base).
struct KindVt { uintptr_t vt; const char *tag; };
const KindVt kKinds[] = {
    {0x63D59C, "wint"}, {0x63D5C4, "winc"}, {0x63D5EC, "view"}, {0x63D614, "trib"}, {0x63D63C, "swnd"}, {0x63D664, "scln"},
    {0x63D68C, "scls"}, {0x63D6B4, "scrl"}, {0x63D6DC, "sbig"}, {0x63D704, "rect"}, {0x63D72C, "rtxt"}, {0x63D754, "rctv"},
    {0x63D77C, "rctf"}, {0x63D7A4, "rctg"}, {0x63D7CC, "rctt"}, {0x63D7F4, "rcta"}, {0x63D81C, "pull"}, {0x63D844, "pbrt"},
    {0x63D86C, "ntxt"}, {0x63D894, "orez"}, {0x63D8BC, "pbar"}, {0x63D8E4, "memi"}, {0x63D90C, "mtxt"}, {0x63D934, "joys"},
    {0x63D95C, "line"}, {0x63D984, "iten"}, {0x63D9AC, "wind"}, {0x63D9D4, "htxt"}, {0x63D9FC, "item"}, {0x63DA24, "frac"},
    {0x63DA4C, "frmt"}, {0x63DA74, "edtl"}, {0x63DA9C, "comb"}, {0x63DAC4, "ctxt"}, {0x63DAEC, "butw"}, {0x63DB14, "chck"},
    {0x63DB3C, "butu"}, {0x63DB64, "butl"}, {0x63DB8C, "butr"}, {0x63DBB4, "butt"}, {0x63DBDC, "comp"}, {0x63DC04, "seli"},
    {0x63DC2C, "rari"}, {0x63DC54, "stxt"}, {0x63DC7C, "cmbs"}, {0x63DCA4, "vidc"}, {0x63DCE4, "load"}, {0x63DD0C, "netp"},
    {0x63DD34, "edit"}, {0x63DD5C, "ednu"}, {0x63DD84, "edip"}, {0x63DDAC, "list"}, {0x63DDD4, "lstc"},
};
// The kinds that become the parent of the records after them (the factory's arms that write its out-slot).
bool IsContainerTag(uint32_t t) {
    return t == Tag("wind") || t == Tag("wint") || t == Tag("winc") || t == Tag("swnd") || t == Tag("frac") || t == Tag("frmt");
}
bool FactoryKnows(uint32_t t) {
    if (t == Tag("ltxt")) return true;
    for (const KindVt &k : kKinds) if (Tag(k.tag) == t && t != Tag("comp")) return true;
    return false;
}

// The screens by their vtables: the name of the same screen's definition in 1.2 where it has one (so a mod asks
// for "gamemenu" on both builds), else st0rm94's name; the design it is built from (tables\menu.def's group).
struct ScreenVt { uintptr_t vt; const char *name; int design; };
const ScreenVt kScreens[] = {
    {0x63D584, "menu", -1},             {0x63D378, "carcyclopedia", -1},  {0x63D398, "mp_person_select", -1},
    {0x63D3B4, "mp_menu", 40},          {0x63D3CC, "mp_lobby", 42},       {0x63D41C, "mp_message", 35},
    {0x63D434, "race_root", 22},        {0x63D44C, "race_championship", 23}, {0x63D464, "race_round_select", 24},
    {0x63D47C, "race_champ_launch", 25}, {0x63D494, "race_champ_results", 26}, {0x63D4AC, "race_setup", 27},
    {0x63D4C4, "race_circuit_select", 28}, {0x63D4DC, "race_opponents", 29}, {0x63D4F4, "race_car_maker", 30},
    {0x63D50C, "race_car_model", 31},   {0x63D524, "race_car_color", 32}, {0x63D53C, "race_launch", 33},
    {0x63D554, "race_results", 34},     {0x63D56C, "race_bonus_car", 35}, {0x63D404, "race_champ_select", -1},
    {0x63DDFC, "combomenu", 19},        {0x63DE14, "simplemessage", 0},   {0x63DE2C, "questionbox", 1},
    {0x63DE44, "messagebox", 2},        {0x63DE5C, "exityesno", 54},      {0x63DE74, "mainmenu", 6},
    {0x63DE8C, "exityesno", 54},        {0x63DEA4, "gamemenu", 7},        {0x63DEC4, "optionsa", 8},
    {0x63DEDC, "optionsb", 9},          {0x63DEF4, "optionsc1", 10},      {0x63DF0C, "keycapture", 11},
    {0x63DF24, "keyconflict", 53},      {0x63DF3C, "inventory", 15},      {0x63DF54, "actionmenu", 16},
    {0x63DF6C, "profileselect", 4},     {0x63DF84, "profilecreate", 3},   {0x63DF9C, "profiledelete", 5},
    {0x63DFB4, "loadgame", 12},         {0x63DFCC, "freeride", 14},       {0x63DFE4, "freeridestart", 52},
    {0x63DFFC, "sessionlist", 47},      {0x63E014, "gunshop", 20},        {0x63E02C, "garageselect", 37},
    {0x63E044, "garagefull", 38},       {0x63E05C, "garagesummary", 39},  {0x63E074, "gameover", 48},
    {0x63E08C, "gamemenuracing", 46},   {0x63E0A4, "loadprogress", 50},   {0x63E0CC, "freeridetrack", 51},
};

uint32_t R32(uintptr_t a) { uint32_t v = 0; MemRead(a, &v, 4); return v; }
float RF(uintptr_t a) { float v = 0; MemRead(a, &v, 4); return v; }
void W32(uintptr_t a, uint32_t v) { MemWrite(a, &v, 4); }
void WF(uintptr_t a, float v) { MemWrite(a, &v, 4); }

uintptr_t g_faultAt;
int Note(EXCEPTION_POINTERS *e) {
    g_faultAt = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (Note(GetExceptionInformation())) { return GetExceptionCode(); }
}

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string GameString(uintptr_t p) {
    char buf[256] = {};
    if (!p) return "";
    for (size_t i = 0; i + 1 < sizeof buf; i++)
        if (!MemRead(p + i, buf + i, 1) || !buf[i]) { buf[i] = 0; break; }
    return buf;
}
std::string KindOfTag(uint32_t tag) {
    char k[5] = {static_cast<char>(tag >> 24), static_cast<char>(tag >> 16), static_cast<char>(tag >> 8), static_cast<char>(tag), 0};
    for (int i = 0; i < 4; i++) if (k[i] < 32 || k[i] > 126) return "";
    return k;
}
uint32_t TagOfKind(const std::string &kind) { return kind.size() == 4 ? Tag(kind.c_str()) : 0; }

using TickFn = int(__fastcall *)(uintptr_t screen, void *edx, uint32_t keyFlags, uint32_t key, int x, int y, int wheel, uint32_t buttons, int ms);
using DtorFn = void(__fastcall *)(uintptr_t screen, void *edx);
using CtorFn = uintptr_t(__fastcall *)(uintptr_t screen, void *edx, uintptr_t parent, uint32_t id);
using BuildFn = void(__fastcall *)(uintptr_t screen, void *edx, const void *table, int design, int gate);
using ThisFn = void(__fastcall *)(uintptr_t self, void *edx);
using CloneFn = uintptr_t(__fastcall *)(uintptr_t screen, void *edx, uint32_t tag, int id, int newId);
using CloseChildFn = void(__fastcall *)(uintptr_t screen, void *edx, uint32_t child, int result);
using StrFn = void(__fastcall *)(uintptr_t control, void *edx, const char *text);
using NewFn = void *(__cdecl *)(size_t size);
TickFn oTick;
DtorFn oDtor;
bool g_ok;

const game::ExeBuild &E() { return game::Exe(); }

// ---- the screens and their controls
std::vector<uintptr_t> Controls(uintptr_t screen) {
    std::vector<uintptr_t> out;
    uintptr_t b = R32(screen + kScrComps), e = R32(screen + kScrCompsEnd);
    if (!b || e < b || (e - b) / 4 > 4096) return out;
    out.resize((e - b) / 4);
    if (!out.empty() && !MemRead(b, out.data(), out.size() * 4)) out.clear();
    return out;
}
uintptr_t Find(uintptr_t screen, int id) {
    for (uintptr_t c : Controls(screen)) if (static_cast<int>(R32(c + kCtlId)) == id) return c;
    return 0;
}
uint32_t TagOf(uintptr_t control) {
    uintptr_t vt = R32(control);
    for (const KindVt &k : kKinds) if (k.vt == vt) return Tag(k.tag);
    return 0;
}
bool IsContainer(uintptr_t control) { return IsContainerTag(TagOf(control)); }
bool DescendsFrom(uintptr_t c, uintptr_t ancestor) {
    for (uintptr_t p = R32(c + kCtlParent), n = 0; p && n < 16; p = R32(p + kCtlParent), n++)
        if (p == ancestor) return true;
    return false;
}
// The control's place on the screen, pixels: its own from its parent's client origin.
float AbsX(uintptr_t c) { uintptr_t p = R32(c + kCtlParent); return RF(c + kCtlX) + (p ? RF(p + kCtlOriginX) : 0); }
float AbsY(uintptr_t c) { uintptr_t p = R32(c + kCtlParent); return RF(c + kCtlY) + (p ? RF(p + kCtlOriginY) : 0); }
void Scale(float *sx, float *sy) {
    *sx = E().menuScale ? RF(E().menuScale) : 1;
    *sy = E().menuScale ? RF(E().menuScale + 4) : 1;
    if (!(*sx > 0.01f)) *sx = 1;
    if (!(*sy > 0.01f)) *sy = 1;
}

const ScreenVt *ScreenOf(uintptr_t vt) {
    for (const ScreenVt &s : kScreens) if (s.vt == vt) return &s;
    return nullptr;
}

// ---- the loader's state of each live screen
struct Shown { int id; int handle; };
struct Page;
struct Live {
    bool seen = false;          // its first tick has run (the items are in)
    std::string name;
    std::vector<Shown> items;   // the mods' items in it
    bool hasCommand = false;
    int command = 0;            // its next tick's answer
    Page *page = nullptr;
};
std::map<uintptr_t, Live> g_live;

struct Orphan { uintptr_t screen; int id; std::string mod, menu; int after; std::string text; };
std::vector<Orphan> g_orphans;

// A page: its description, kept for when the game builds it and for the values set before that.
struct PageCtl {
    int id;
    uint32_t tag;                    // 1.0's kind
    const char *text = nullptr;      // interned (a picture's file; null: none)
    uint32_t firstText = 0, lastText = 0;   // a selector's options (text ids of the loader's)
    bool hasRange = false;
    int lo = INT_MIN, hi = INT_MAX;  // a number's range
    int parent = -1;                 // the index of the container it is in (-1: the page's window itself)
};
struct Page {
    MmlMod *mod = nullptr;           // null: the bridge's
    bool bridge = false;
    MmlMenuFn fn = nullptr;
    void *user = nullptr;
    std::string name;
    std::vector<PageCtl> ctls;       // in the order of the design
    std::vector<uint8_t> design;     // the records (0x24 bytes each), "stop" last
    std::map<int, float> values;     // set before the page was made, or the last seen (changes are told)
    uintptr_t screen = 0;
    bool waiting = false, closing = false;
    DWORD askedAt = 0;
};
std::vector<std::unique_ptr<Page>> g_pages;

// ---- the kinds' values (each read in its code: knowledge/notes/interface.md "1.0")
//   scrl, scln, pbar, pbrt   +0x2C, a float 0..1
//   chck                     +0x20, 0 or 1 (Space or a click toggles it)
//   iten, ednu               +0x24, an int (a click or Enter: the screen adds 1; the widget took 2 for a left)
//   cmbs                     +0xC, the text id shown, between +0x3C and +0x40 (the options' first and last)
//   swnd                     +0x44 the scroll aimed at, -(+0x28 the range)..0 (+0x40 the scroll now)
bool ValueOf(uintptr_t c, float *v) {
    uint32_t t = TagOf(c);
    if (t == Tag("scrl") || t == Tag("scln") || t == Tag("pbar") || t == Tag("pbrt")) { *v = RF(c + kCtlFloat); return true; }
    if (t == Tag("chck")) { *v = R32(c + kCtlRec) ? 1.0f : 0.0f; return true; }
    if (t == Tag("iten") || t == Tag("ednu")) { *v = static_cast<float>(static_cast<int32_t>(R32(c + kCtlInt))); return true; }
    if (t == Tag("cmbs")) { *v = static_cast<float>(static_cast<int32_t>(R32(c + kCtlTextId) - R32(c + 0x3C))); return true; }
    if (t == Tag("swnd")) { float range = RF(c + 0x28); *v = range > 0 ? -RF(c + 0x44) / range : 0; return true; }
    return false;
}
bool HasValueTag(uint32_t t) {
    return t == Tag("scrl") || t == Tag("scln") || t == Tag("pbar") || t == Tag("pbrt") || t == Tag("chck") || t == Tag("iten") ||
           t == Tag("ednu") || t == Tag("cmbs") || t == Tag("swnd");
}
void ForceUpdate(uintptr_t c) {
    uintptr_t fn = R32(R32(c) + 4 * kCtlSlotForceUpdate);
    auto call = [&] { reinterpret_cast<ThisFn>(fn)(c, nullptr); };
    if (fn) Guarded(call);
}
bool SetValueOf(uintptr_t c, float value, const PageCtl *pc) {
    uint32_t t = TagOf(c);
    float unit = value < 0 ? 0 : value > 1 ? 1 : value;
    int n = static_cast<int>(lroundf(value));
    if (t == Tag("scrl") || t == Tag("scln") || t == Tag("pbar") || t == Tag("pbrt")) { WF(c + kCtlFloat, unit); return true; }
    if (t == Tag("chck")) { W32(c + kCtlRec, value != 0 ? 1 : 0); return true; }
    if (t == Tag("iten") || t == Tag("ednu")) {
        if (pc && pc->hasRange) n = std::min(std::max(n, pc->lo), pc->hi);
        W32(c + kCtlInt, static_cast<uint32_t>(n));
        return true;
    }
    if (t == Tag("cmbs")) {
        uint32_t first = R32(c + 0x3C), last = R32(c + 0x40);
        if (!first || last < first) return false;
        uint32_t id = first + static_cast<uint32_t>(std::max(n, 0));
        W32(c + kCtlTextId, id > last ? last : id);
        ForceUpdate(c);
        return true;
    }
    if (t == Tag("swnd")) { WF(c + 0x44, -unit * RF(c + 0x28)); return true; }
    return false;
}
// A control's text: an edit box's buffer by its own setter, a picture's file by its own (the texture loaded), any
// other's pointer (kept: the strings are interned).
bool SetTextOf(uintptr_t c, const char *text) {
    uint32_t t = TagOf(c);
    if (t == Tag("edit") || t == Tag("edtl") || t == Tag("rctf") || t == Tag("rcta")) {
        uintptr_t fn = R32(R32(c) + 4 * kCtlSlotSetText);
        auto call = [&] { reinterpret_cast<StrFn>(fn)(c, nullptr, text); };
        return fn && !Guarded(call);
    }
    W32(c + kCtlText, reinterpret_cast<uintptr_t>(text));
    return true;
}
std::string TextOf(uintptr_t c) {
    uint32_t t = TagOf(c);
    if (t == Tag("edit") || t == Tag("edtl")) return GameString(c + 0x38);
    return GameString(R32(c + kCtlText));
}

// ---- the mods' items, added when a screen first ticks
void MoveDown(const std::vector<uintptr_t> &all, uintptr_t c, float dy) {
    WF(c + kCtlY, RF(c + kCtlY) + dy);
    if (!IsContainer(c)) return;
    for (uintptr_t d : all)   // a container's client origin, and those of the containers in it, are on the screen
        if (IsContainer(d) && (d == c || DescendsFrom(d, c))) WF(d + kCtlOriginY, RF(d + kCtlOriginY) + dy);
}

void AddItems(uintptr_t screen, Live *live) {
    const game::ExeBuild &e = E();
    std::map<int, int> placed;   // the control an item was asked after -> the item put there last
    for (const MenuItemInfo &reg : MenuItemsFor(live->name)) {
        bool shown = false;
        for (const Shown &s : live->items) if (s.handle == reg.handle) shown = true;
        if (shown) continue;
        std::vector<uintptr_t> controls = Controls(screen);
        int afterId = placed.count(reg.after) ? placed[reg.after] : reg.after;
        uintptr_t src = 0;
        for (uintptr_t c : controls) if (static_cast<int>(R32(c + kCtlId)) == afterId) { src = c; break; }
        if (!src) {
            HWARN("menu: '%s' has no control %d: the item '%s' of %s is not added", reg.menu.c_str(), reg.after, reg.text, ModIdOf(reg.mod));
            continue;
        }
        uint32_t tag = TagOf(src);
        if (!tag) {
            HWARN("menu: control %d of '%s' is of a kind not known: the item '%s' is not added", afterId, reg.menu.c_str(), reg.text);
            continue;
        }
        // An id between the control's and the next one in use: the keyboard walks a screen by rising id.
        uint32_t srcId = R32(src + kCtlId), next = 0xFFFFFFFF, highest = 0;
        for (uintptr_t c : controls) {
            uint32_t id = R32(c + kCtlId);
            if (id > srcId && id < next) next = id;
            if (id > highest) highest = id;
        }
        uint32_t newId = next - srcId >= 2 ? srcId + 1 : highest + 1;
        if (newId != srcId + 1)
            HWARN("menu: '%s' has no free id after %u: the item '%s' gets %u (the keyboard reaches it last)", reg.menu.c_str(), srcId, reg.text, newId);
        // The row's pitch: the distance to the next row under it in the same parent (rows may be closer than their
        // height), else its height.
        uintptr_t parent = R32(src + kCtlParent);
        float top = RF(src + kCtlY), h = RF(src + kCtlH), pitch = h;
        for (uintptr_t c : controls)
            if (c != src && R32(c + kCtlParent) == parent && RF(c + kCtlY) > top + 0.5f) pitch = std::min(pitch, RF(c + kCtlY) - top);
        if (pitch < h * 0.5f) pitch = h;   // something beside it, not a row under it
        uintptr_t added = 0;
        auto call = [&] { added = reinterpret_cast<CloneFn>(e.m10CloneUnder)(screen, nullptr, tag, static_cast<int>(srcId), static_cast<int>(newId)); };
        if (DWORD ex = Guarded(call)) {
            HWARN("menu: adding the item '%s' to '%s' faulted (%08X at %s)", reg.text, reg.menu.c_str(), static_cast<unsigned>(ex), DescribeAddress(g_faultAt).c_str());
            continue;
        }
        if (!added) {
            HWARN("menu: the game did not add the item '%s' to '%s' (a \"%s\" cannot be copied)", reg.text, reg.menu.c_str(), KindOfTag(tag).c_str());
            continue;
        }
        WF(added + kCtlY, top + pitch);
        // Everything under the row moves down by it, and each window around it grows by it, up to the screen's
        // outermost one (the way the profile screen makes room for its rows).
        controls = Controls(screen);
        uintptr_t pivot = src;
        for (uintptr_t p = parent, n = 0; p && n < 16; pivot = p, p = R32(p + kCtlParent), n++) {
            float pivotTop = RF(pivot + kCtlY);
            for (uintptr_t c : controls)
                if (c != pivot && c != added && R32(c + kCtlParent) == p && RF(c + kCtlY) > pivotTop + 0.5f) MoveDown(controls, c, pitch);
            WF(p + kCtlH, RF(p + kCtlH) + pitch);
        }
        W32(added + kCtlFlags, R32(added + kCtlFlags) & ~(kFlagFocused | kFlagHidden));
        W32(added + kCtlTextId, 0);
        W32(added + kCtlText, reinterpret_cast<uintptr_t>(reg.text));
        live->items.push_back({static_cast<int>(newId), reg.handle});
        placed[reg.after] = static_cast<int>(newId);
    }
}

void HideControl(uintptr_t screen, int id) {
    if (uintptr_t c = Find(screen, id)) W32(c + kCtlFlags, (R32(c + kCtlFlags) | kFlagHidden) & ~(kFlagSelectable | kFlagFocused));
}

Page *PageOfScreen(uintptr_t screen) {
    auto it = g_live.find(screen);
    return it != g_live.end() ? it->second.page : nullptr;
}

// A screen seen for the first time (its first tick, or the loader's frame finding it in the stack: a page may be
// put over it before it ticks): its name, and the mods' items added. Its constructor has run by then (screens are
// made in the ticks of others, or before the menus' loop).
void Seen(uintptr_t screen) {
    Live &live = g_live[screen];
    if (live.seen) return;
    live.seen = true;
    if (live.page) return;
    const ScreenVt *s = ScreenOf(R32(screen));
    live.name = s ? s->name : "";
    if (!live.name.empty()) AddItems(screen, &live);
}

int __fastcall hkTick(uintptr_t screen, void *edx, uint32_t keyFlags, uint32_t key, int x, int y, int wheel, uint32_t buttons, int ms) {
    Seen(screen);
    int r = oTick(screen, edx, keyFlags, key, x, y, wheel, buttons, ms);
    auto it = g_live.find(screen);   // (the tick does not delete its screen; looked up again all the same)
    if (it == g_live.end()) return r;
    Live &l = it->second;
    if (l.hasCommand) {   // (a mod's item's id too: delivered below, as a click on it)
        l.hasCommand = false;
        HLOG("menu: command %d given to the screen '%s' 0x%08X as its control's activation", l.command, l.name.c_str(), static_cast<unsigned>(screen));
        r = l.command;
    }
    if (r > 0)
        for (const Shown &s : l.items) {
            if (s.id != r) continue;
            MenuItemInfo info;
            if (MenuItemByHandle(s.handle, &info)) MenuDeliver(info.mod, info.fn, info.user, r, std::string("item '") + info.text + "' of " + info.menu);
            return 0;
        }
    return r;
}

void __fastcall hkDtor(uintptr_t screen, void *edx) {
    auto it = g_live.find(screen);
    if (it != g_live.end()) {
        Page *page = it->second.page;
        g_live.erase(it);
        for (size_t i = 0; page && i < g_pages.size(); i++)
            if (g_pages[i].get() == page) { g_pages.erase(g_pages.begin() + i); break; }
    }
    oDtor(screen, edx);
}

// ---- pages
struct Rec { int32_t id; uint32_t tag; float x, y, w, h; uint32_t textId, flags, colour; };
static_assert(sizeof(Rec) == 0x24, "a design record");
uintptr_t g_pageVt[1 + kSlots];   // the type record, then the slots

Page *PageOf(MmlMod *mod, bool bridge) {
    for (auto &p : g_pages)
        if (p->bridge == bridge && p->mod == mod && (bridge || mod) && !p->closing) return p.get();
    return nullptr;
}
const PageCtl *CtlOf(const Page *p, int id) {
    for (const PageCtl &c : p->ctls) if (c.id == id) return &c;
    return nullptr;
}

// A page's mod told of a click (or a change). Its function may close the page (a Back button): the screen and the
// page are gone then, false.
bool Tell(uintptr_t screen, int id) {
    Page *p = PageOfScreen(screen);
    if (!p || p->closing) return false;
    if (p->bridge || p->mod) MenuDeliver(p->mod, p->fn, p->user, id, "page " + p->name);
    p = PageOfScreen(screen);
    return p && !p->closing;
}

// What the player changed that sends no id (a check box, a selector's drop-down): told as a click.
void TellChanges(uintptr_t screen) {
    Page *p = PageOfScreen(screen);
    if (!p) return;
    std::vector<int> changed;
    for (const PageCtl &pc : p->ctls) {
        if (pc.tag != Tag("chck") && pc.tag != Tag("cmbs")) continue;
        uintptr_t c = Find(screen, pc.id);
        float v = 0;
        if (!c || !ValueOf(c, &v)) continue;
        auto it = p->values.find(pc.id);
        if (it != p->values.end() && it->second != v) changed.push_back(pc.id);
        p->values[pc.id] = v;
    }
    for (int id : changed)
        if (!Tell(screen, id)) return;
}

void CloseNow(Page *p) {
    uintptr_t s = p->screen, parent = R32(s + kScrUnder);
    W32(E().menuTop, parent);
    // the parent told by the base class's OnCloseChild (the input re-armed), not its own: a screen's own takes the
    // ids of its own children
    if (parent) {
        uintptr_t fn = R32(E().m10BaseVt + 4 * kSlotCloseChild);
        auto call = [&] { reinterpret_cast<CloseChildFn>(fn)(parent, nullptr, kPageScreenId, 0); };
        Guarded(call);
    }
    uintptr_t destroy = R32(R32(s) + 4 * kSlotDestroy);
    auto del = [&] { reinterpret_cast<ThisFn>(destroy)(s, nullptr); };   // its controls and itself (the dtor hook forgets it)
    if (DWORD ex = Guarded(del)) HWARN("menu: closing the page %s faulted (%08X)", p->name.c_str(), static_cast<unsigned>(ex));
}

void Close(Page *p) {
    if (!p->screen) {   // it waits for a menu: forgotten
        for (size_t i = 0; i < g_pages.size(); i++)
            if (g_pages[i].get() == p) { g_pages.erase(g_pages.begin() + i); break; }
        return;
    }
    p->closing = true;
    if (m10::Top() == p->screen) CloseNow(p);   // else at the frame it is the active one again (a drop-down over it closed)
}

int __fastcall PageTick(uintptr_t screen, void *edx, uint32_t keyFlags, uint32_t key, int x, int y, int wheel, uint32_t buttons, int ms) {
    int r = reinterpret_cast<TickFn>(E().m10Tick)(screen, edx, keyFlags, key, x, y, wheel, buttons, ms);   // (through the hook)
    Page *p = PageOfScreen(screen);
    if (!p || p->closing) return 0;
    if (wheel)   // the scrolled windows: the game's own screens scroll theirs themselves (the load menu by the wheel)
        for (const PageCtl &pc : p->ctls)
            if (pc.tag == Tag("swnd"))
                if (uintptr_t c = Find(screen, pc.id)) WF(c + 0x44, RF(c + 0x44) + wheel / 4.0f);
    if (r == 0 && (key & 0xFF) == 1) {   // Esc
        Close(p);
        return 0;
    }
    if (r > 0) {
        const PageCtl *pc = CtlOf(p, r);
        uintptr_t c = Find(screen, r);
        if (pc && c && (pc->tag == Tag("iten") || pc->tag == Tag("ednu")))   // the screen steps a number (+1; a left took 2)
            SetValueOf(c, static_cast<float>(static_cast<int32_t>(R32(c + kCtlInt)) + 1), pc);
        if (!Tell(screen, r)) return 0;
    }
    TellChanges(screen);
    return 0;
}

bool PushNow(Page *p) {
    const game::ExeBuild &e = E();
    uintptr_t top = m10::Top(), s = 0;
    if (top) Seen(top);   // (its items in before the page covers it)
    auto make = [&] {
        s = reinterpret_cast<uintptr_t>(reinterpret_cast<NewFn>(e.gameNew)(kPageSize));
        if (!s) return;
        memset(reinterpret_cast<void *>(s), 0, kPageSize);
        reinterpret_cast<CtorFn>(e.m10Ctor)(s, nullptr, top, kPageScreenId);
        W32(s, reinterpret_cast<uintptr_t>(&g_pageVt[1]));
    };
    if (Guarded(make) || !s) { HWARN("menu: the page %s could not be made", p->name.c_str()); return false; }
    Live &live = g_live[s];
    live.seen = true;
    live.name = p->name;
    live.page = p;
    p->screen = s;
    p->waiting = false;
    std::vector<uint8_t> design = p->design;   // (the builder adds a "base" record's offsets into the records)
    auto build = [&] { reinterpret_cast<BuildFn>(e.m10Build)(s, nullptr, design.data(), 0, 0); };
    if (DWORD ex = Guarded(build)) {
        HWARN("menu: building the page %s faulted (%08X at %s): the page is dropped", p->name.c_str(), static_cast<unsigned>(ex),
              DescribeAddress(g_faultAt).c_str());
        g_live.erase(s);
        p->screen = 0;
        return false;   // (its object is left as it is: not made active, not freed)
    }
    std::vector<uintptr_t> controls = Controls(s);
    if (controls.size() != p->ctls.size())
        HWARN("menu: the page %s: the game made %zu of %zu controls", p->name.c_str(), controls.size(), p->ctls.size());
    // A container block after another one: the builder made it the other's child (the last container is the parent
    // of what follows); it belongs to the page's window.
    for (size_t i = 0; i < controls.size() && i < p->ctls.size(); i++) {
        const PageCtl &pc = p->ctls[i];
        if (!IsContainerTag(pc.tag) || pc.parent < 0 || static_cast<size_t>(pc.parent) >= controls.size()) continue;
        uintptr_t want = controls[static_cast<size_t>(pc.parent)], c = controls[i];
        if (R32(c + kCtlParent) == want) continue;
        float title = RF(c + kCtlOriginY) - RF(c + kCtlY) - (R32(c + kCtlParent) ? RF(R32(c + kCtlParent) + kCtlOriginY) : 0);
        W32(c + kCtlParent, want);
        WF(c + kCtlOriginX, RF(c + kCtlX) + RF(want + kCtlOriginX));
        WF(c + kCtlOriginY, RF(c + kCtlY) + RF(want + kCtlOriginY) + title);
    }
    for (size_t i = 0; i < controls.size() && i < p->ctls.size(); i++) {
        const PageCtl &pc = p->ctls[i];
        if (pc.text) SetTextOf(controls[i], pc.text);
    }
    // a scrolled window scrolls as far as its controls reach (the load menu sets it so: its last row's bottom - its
    // height + 4)
    for (uintptr_t w : controls) {
        if (TagOf(w) != Tag("swnd")) continue;
        float bottom = 0;
        for (uintptr_t c : controls) if (R32(c + kCtlParent) == w) bottom = std::max(bottom, RF(c + kCtlY) + RF(c + kCtlH));
        WF(w + 0x28, std::max(0.0f, bottom - RF(w + kCtlH) + 4.0f));
    }
    for (auto &kv : p->values)
        if (uintptr_t c = Find(s, kv.first)) SetValueOf(c, kv.second, CtlOf(p, kv.first));
    for (const PageCtl &pc : p->ctls)   // the values now, for what the player changes
        if (uintptr_t c = Find(s, pc.id)) { float v = 0; if (ValueOf(c, &v)) p->values[pc.id] = v; }
    auto update = [&] { reinterpret_cast<ThisFn>(e.m10UpdateAll)(s, nullptr); };
    Guarded(update);
    float cx = 0, cy = 0;
    if (e.m10Cursor) { cx = RF(e.m10Cursor); cy = RF(e.m10Cursor + 4); }
    W32(s + kScrMouse, static_cast<uint32_t>(static_cast<int>(cx)));
    W32(s + kScrMouse + 4, static_cast<uint32_t>(static_cast<int>(cy)));
    W32(e.menuTop, s);
    return true;
}

// 1.2's kinds asked for on 1.0: the 1.0 kind that does the same.
uint32_t KindFor(const std::string &kind, uint32_t flags, bool first, bool titled) {
    if (first) return titled ? Tag("wint") : Tag("wind");
    if (kind == "stxt") return flags & MML_MENU_CENTER ? Tag("ctxt") : flags & MML_MENU_RIGHT ? Tag("rtxt") : Tag("stxt");
    if (kind == "butt") return flags & MML_MENU_RIGHT ? Tag("butr") : Tag("butt");
    if (kind == "slid") return Tag("scrl");
    if (kind == "sldi") return Tag("scls");
    if (kind == "wins") return Tag("swnd");
    if (kind == "bsel" || kind == "bslb") return Tag("cmbs");
    if (kind == "edin") return Tag("ednu");
    if (kind == "edit") return Tag("edtl");   // (31 characters; 1.0's "edit" takes 10)
    if (kind == "rctt") return Tag("rctf");   // a picture from a file (1.0's own "rctt" is a render target)
    if (kind == "butc") return Tag("butt");
    return TagOfKind(kind);
}
// A text's colour: MML_MENU_TEXT_* as 1.2 draws them (1.0 takes the colour from the record; the game's own texts
// have 0 there, its usual dark text).
uint32_t TextColour(uint32_t flags) {
    switch (flags & 0x300) {
    case MML_MENU_TEXT_WHITE: return 0xFFFFFFFF;
    case MML_MENU_TEXT_YELLOW: return 0xFFFFFF00;
    case MML_MENU_TEXT_BLACK: return 0xFF000000;
    default: return 0;
    }
}

int Open(MmlMod *mod, bool bridge, const char *name, const MmlMenuControl *controls, int count, MmlMenuFn fn, void *user, std::string *err) {
    if (!g_ok) { *err = "the menus are not known for this build"; return 0; }
    if (!name || !controls || count < 1 || count > 256) { *err = "a name and 1..256 controls"; return 0; }
    std::string n = Lower(name);
    if (n.empty() || n.size() > 32 || n.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
        *err = "name: letters, digits, _ (at most 32)";
        return 0;
    }
    if (PageOf(mod, bridge)) { *err = "a page of this owner is open already (one at a time)"; return 0; }
    MenuTextsHook();
    auto page = std::make_unique<Page>();
    page->mod = mod;
    page->bridge = bridge;
    page->fn = fn;
    page->user = user;
    page->name = Lower(std::string(bridge ? "bridge" : ModIdOf(mod)) + "_" + n);
    // The records in the builder's order: the page's window, the controls directly in it, then each container
    // with its controls (the builder makes a container the parent of every record after it, and has no "end";
    // PushNow gives each block back to the page's window).
    struct Entry { Rec rec; PageCtl ctl; int block; };   // block: -1 the page's window, else a container's number
    std::vector<Entry> entries;
    int depth = 0, block = -1, blocks = 0;
    for (int i = 0; i < count; i++) {
        const MmlMenuControl &c = controls[i];
        std::string kind = Lower(c.kind ? c.kind : "");
        if (kind == "end") {
            if (depth == 0) { *err = "control " + std::to_string(i) + ": \"end\" with no window open"; return 0; }
            if (depth > 1) depth = 1;
            block = -1;
            continue;
        }
        bool first = i == 0;
        if (first && kind != "wind" && kind != "wint" && kind != "winc") { *err = "the first control must be a window (\"wind\")"; return 0; }
        uint32_t tag = KindFor(kind, c.flags, first, first && c.text && *c.text);
        if (!tag || !FactoryKnows(tag)) {
            *err = "control " + std::to_string(i) + ": the game has no control kind \"" + kind +
                   "\" (wind, swnd/wins, stxt, ctxt, item, butt, line, scrl/slid, pbar, chck, cmbs/bsel, iten, edtl/edit, rctf ...)";
            return 0;
        }
        if (!(c.x == c.x && c.y == c.y && c.w == c.w && c.h == c.h)) { *err = "control " + std::to_string(i) + ": x, y, w, h must be numbers"; return 0; }
        bool container = IsContainerTag(tag);
        if (!first && container && depth > 1) {
            *err = "control " + std::to_string(i) + ": a container in a container (\"end\" the one before: containers do not nest)";
            return 0;
        }
        Rec rec{};
        rec.id = c.id;
        rec.tag = tag;
        rec.x = c.x;
        rec.y = c.y;
        rec.w = c.w;
        rec.h = c.h;   // (both builds measure a window's controls from under its title)
        PageCtl pc;
        pc.id = c.id;
        pc.tag = tag;
        std::string text = c.text ? c.text : "";
        if (tag == Tag("cmbs")) {   // options: the record's flags and colour words are the first and last text id
            std::vector<std::string> options = MenuSplitOptions(text);
            uint32_t ids = MenuTextIds(options);
            if (!ids) { *err = "control " + std::to_string(i) + ": the options need the text database (not hooked)"; return 0; }
            pc.firstText = ids;
            pc.lastText = ids + static_cast<uint32_t>(options.size()) - 1;
            rec.flags = pc.firstText;
            rec.colour = pc.lastText;
            rec.textId = pc.firstText;
        } else if (tag == Tag("rctf")) {
            if (!text.empty()) pc.text = MenuIntern(text);   // the file, by the control's own SetText once it is made
        } else {
            if (!text.empty()) {
                rec.textId = MenuTextIds({text});
                if (!rec.textId) pc.text = MenuIntern(text);   // (no text database: the pointer set once it is made)
            }
            if (tag == Tag("stxt") || tag == Tag("ctxt") || tag == Tag("rtxt")) {   // a text: its font and colour
                rec.flags = c.flags & 0xF;
                rec.colour = TextColour(c.flags);
            }
        }
        if (container && !first) block = blocks++;
        entries.push_back({rec, pc, first ? -2 : block});
        if (container) depth = first ? 1 : 2;
    }
    for (int pass = -2; pass < blocks; pass++)   // the window, what is directly in it, then each block
        for (const Entry &en : entries) {
            if (en.block != pass) continue;
            PageCtl pc = en.ctl;
            if (IsContainerTag(pc.tag) && pass >= 0) pc.parent = 0;   // the page's window
            page->ctls.push_back(pc);
            const uint8_t *b = reinterpret_cast<const uint8_t *>(&en.rec);
            page->design.insert(page->design.end(), b, b + sizeof(Rec));
        }
    Rec stop{};
    stop.tag = kTagStop;
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&stop);
    page->design.insert(page->design.end(), b, b + sizeof(Rec));
    Page *p = page.get();
    g_pages.push_back(std::move(page));
    if (m10::Top()) {
        if (!PushNow(p)) { *err = "the game could not make the page (the log tells)"; g_pages.pop_back(); return 0; }
        return 1;
    }
    // No menu is open: the pause menu is opened first (the game's own Esc), the page goes over it.
    if (MissionState() != MML_MISSION_RUNNING) { g_pages.pop_back(); *err = "no menu is open and no mission runs: there is nothing to put the page over"; return 0; }
    p->waiting = true;
    p->askedAt = GetTickCount();
    InputTapKey(1 /* Esc */, 2);
    return 1;
}

// ---- describing
json Described(uintptr_t screen) {
    json out = {{"name", m10::Name(screen)}, {"controls", json::array()}};
    if (Page *p = PageOfScreen(screen)) out["page_of"] = p->bridge ? "bridge" : ModIdOf(p->mod);
    float sx, sy;
    Scale(&sx, &sy);
    auto unit = [](float v) { return std::round(v * 10.0f) / 10.0f; };
    auto it = g_live.find(screen);
    for (uintptr_t c : Controls(screen)) {
        uint32_t flags = R32(c + kCtlFlags), parent = R32(c + kCtlParent);
        char f[16];
        snprintf(f, sizeof f, "0x%08X", flags);
        json j = {{"id", static_cast<int>(R32(c + kCtlId))}, {"kind", KindOfTag(TagOf(c))}, {"flags", f},
                  {"text", MenuUtf8(TextOf(c).c_str())}, {"text_id", R32(c + kCtlTextId)},
                  {"x", unit(RF(c + kCtlX) / sx)}, {"y", unit(RF(c + kCtlY) / sy)}, {"w", unit(RF(c + kCtlW) / sx)}, {"h", unit(RF(c + kCtlH) / sy)},
                  {"parent", parent ? json(static_cast<int>(R32(parent + kCtlId))) : json()},
                  {"focused", (flags & kFlagFocused) != 0}, {"enabled", (flags & kFlagSelectable) != 0}, {"hidden", (flags & kFlagHidden) != 0},
                  {"screen", {unit(AbsX(c) / sx), unit(AbsY(c) / sy), unit(RF(c + kCtlW) / sx), unit(RF(c + kCtlH) / sy)}}};
        if (!j["kind"].get<std::string>().size()) {
            char v[16];
            snprintf(v, sizeof v, "0x%06X", R32(c));
            j["vtable"] = v;
        }
        float v = 0;
        if (ValueOf(c, &v)) j["value"] = std::round(v * 1000) / 1000;
        if (it != g_live.end())
            for (const Shown &s : it->second.items)
                if (s.id == static_cast<int>(R32(c + kCtlId))) {
                    MenuItemInfo info;
                    if (MenuItemByHandle(s.handle, &info)) j["item_of"] = info.mod ? ModIdOf(info.mod) : "bridge";
                }
        out["controls"].push_back(j);
    }
    return out;
}

// A design of the game's own table (tables\menu.def, read by the game at its start): its records as they are.
json Design(int design, std::string *err) {
    uintptr_t at = E().m10Designs;
    const size_t kMax = 0x7E90 / sizeof(Rec);   // the table's room
    for (int g = 0; g < design; g++) {
        size_t n = 0;
        while (R32(at + 4) != kTagStop && n++ < kMax) at += sizeof(Rec);
        at += sizeof(Rec);
        if (at >= E().m10Designs + 0x7E90) { *err = "no design " + std::to_string(design) + " in tables\\menu.def"; return json(); }
    }
    json out = {{"file", "tables\\menu.def"}, {"design", design}, {"controls", json::array()}};
    using GetFn = const char *(__fastcall *)(uintptr_t db, void *edx, unsigned id);
    for (size_t n = 0; n < kMax && at < E().m10Designs + 0x7E90; n++, at += sizeof(Rec)) {
        Rec r{};
        MemRead(at, &r, sizeof r);
        if (r.tag == kTagStop) break;
        char f[16], c[16];
        snprintf(f, sizeof f, "0x%08X", r.flags);
        snprintf(c, sizeof c, "0x%08X", r.colour);
        json j = {{"id", r.id}, {"kind", KindOfTag(r.tag)}, {"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}, {"flags", f}, {"colour", c}};
        if (r.textId) {
            j["text_id"] = r.textId;
            const char *t = nullptr;
            const game::ExeBuild &e = E();
            auto get = [&] { t = reinterpret_cast<GetFn>(e.textGetOrNull)(e.textDb, nullptr, r.textId); };
            if (e.textGetOrNull && e.textDb && !Guarded(get) && t) j["text"] = MenuUtf8(t);
        }
        out["controls"].push_back(j);
    }
    return out;
}

} // namespace

namespace m10 {

bool Known() { return g_ok; }

void Init() {
    const game::ExeBuild &e = E();
    if (!e.m10Tick || !e.menuTop || !e.gameNew) return;
    std::string err;
    if (!HookAdd(nullptr, e.m10Tick, reinterpret_cast<void *>(&hkTick), reinterpret_cast<void **>(&oTick), &err) ||
        !HookAdd(nullptr, e.m10Dtor, reinterpret_cast<void *>(&hkDtor), reinterpret_cast<void **>(&oDtor), &err)) {
        HWARN("menus: cannot hook 1.0's screens: %s", err.c_str());
        return;
    }
    // the pages' class: the base screen's vtable (its type record before it) with the loader's Tick
    if (!MemRead(e.m10BaseVt - 4, g_pageVt, sizeof g_pageVt)) { HWARN("menus: 1.0's base screen vtable cannot be read"); return; }
    g_pageVt[1 + kSlotTick] = reinterpret_cast<uintptr_t>(&PageTick);
    g_ok = true;
    MenuTextsHook();
    HLOG("menus: 1.0's screens hooked (commands, items, pages)");
}

void Frame() {
    for (uintptr_t s = Top(), n = 0; s && n < 16; s = Under(s), n++) Seen(s);
    for (const Orphan &o : g_orphans)
        if (g_live.count(o.screen)) HideControl(o.screen, o.id);
    g_orphans.clear();
    for (size_t i = 0; i < g_pages.size(); i++) {
        Page *p = g_pages[i].get();
        if (p->closing && p->screen && Top() == p->screen) { CloseNow(p); return; }
        if (!p->waiting || p->screen) continue;
        if (Top()) {
            if (!PushNow(p) && !p->screen) g_pages.erase(g_pages.begin() + i);
            return;
        }
        if (GetTickCount() - p->askedAt > 2000) {
            HWARN("menu: no menu opened for the page %s: dropped", p->name.c_str());
            g_pages.erase(g_pages.begin() + i);
            return;
        }
    }
}

uintptr_t Top() { return g_ok ? R32(E().menuTop) : 0; }
uintptr_t Under(uintptr_t screen) { return screen ? R32(screen + kScrUnder) : 0; }

std::string Name(uintptr_t screen) {
    if (Page *p = PageOfScreen(screen)) return p->name;
    const ScreenVt *s = ScreenOf(R32(screen));
    return s ? s->name : "";
}

bool Command(uintptr_t screen, int id) {
    if (!g_ok || !screen) return false;
    Live &l = g_live[screen];
    l.hasCommand = true;
    l.command = id;
    return true;
}

std::string Describe(uintptr_t screen) { return Described(screen).dump(-1, ' ', false, json::error_handler_t::replace); }

bool ControlValue(uintptr_t screen, int id, float *value) {
    uintptr_t c = Find(screen, id);
    return c && ValueOf(c, value);
}

void ItemAsked(int handle) {
    MenuItemInfo info;
    if (!MenuItemByHandle(handle, &info) || !info.mod) return;
    for (size_t i = 0; i < g_orphans.size();) {   // its own item, still in an open screen, from before its reload
        const Orphan &o = g_orphans[i];
        auto live = g_live.find(o.screen);
        if (o.mod == ModIdOf(info.mod) && o.menu == info.menu && o.after == info.after && o.text == info.text && live != g_live.end()) {
            live->second.items.push_back({o.id, handle});
            g_orphans.erase(g_orphans.begin() + i);
        } else {
            i++;
        }
    }
}

void ModRemoved(MmlMod *mod) {
    for (auto &kv : g_live)
        for (size_t k = 0; k < kv.second.items.size();) {
            MenuItemInfo info;
            if (!MenuItemByHandle(kv.second.items[k].handle, &info) || info.mod != mod) { k++; continue; }
            g_orphans.push_back({kv.first, kv.second.items[k].id, ModIdOf(mod), info.menu, info.after, info.text});
            kv.second.items.erase(kv.second.items.begin() + k);
        }
    if (Page *p = PageOf(mod, false)) {
        p->fn = nullptr;
        Close(p);
        for (auto &q : g_pages) if (q->mod == mod && !q->bridge) q->mod = nullptr;   // (it lives until the game deletes it)
    }
}

int PageOpen(MmlMod *mod, bool bridge, const char *name, const MmlMenuControl *controls, int count, MmlMenuFn fn, void *user, std::string *err) {
    return Open(mod, bridge, name, controls, count, fn, user, err);
}

void PageClose(MmlMod *mod, bool bridge) {
    if (Page *p = PageOf(mod, bridge)) Close(p);
}

bool PageIsOpen(MmlMod *mod, bool bridge) { return PageOf(mod, bridge) != nullptr; }

int PageText(MmlMod *mod, bool bridge, int id, const char *text) {
    Page *p = PageOf(mod, bridge);
    if (!p || !text) return 0;
    bool known = false;
    for (PageCtl &pc : p->ctls)
        if (pc.id == id) { pc.text = MenuIntern(text); known = true; }
    if (!known) return 0;
    if (!p->screen) return 1;
    uintptr_t c = Find(p->screen, id);
    return c && SetTextOf(c, MenuIntern(text)) ? 1 : 0;
}

int PageValue(MmlMod *mod, bool bridge, int id, float value) {
    Page *p = PageOf(mod, bridge);
    if (!p || !(value == value) || value < -1e9f || value > 1e9f) return 0;
    const PageCtl *pc = CtlOf(p, id);
    if (!pc || !HasValueTag(pc->tag)) return 0;
    if (!p->screen) { p->values[id] = value; return 1; }
    uintptr_t c = Find(p->screen, id);
    if (!c || !SetValueOf(c, value, pc)) return 0;
    float now = 0;
    if (ValueOf(c, &now)) p->values[id] = now;   // (set by the mod: not a change to tell it)
    return 1;
}

int PageGetValue(MmlMod *mod, bool bridge, int id, float *value) {
    Page *p = PageOf(mod, bridge);
    if (!p || !value) return 0;
    if (p->screen) {
        uintptr_t c = Find(p->screen, id);
        return c && ValueOf(c, value) ? 1 : 0;
    }
    auto v = p->values.find(id);
    if (v == p->values.end()) return 0;
    *value = v->second;
    return 1;
}

int PageRange(MmlMod *mod, bool bridge, int id, int lo, int hi) {
    Page *p = PageOf(mod, bridge);
    if (!p || lo > hi) return 0;
    for (PageCtl &pc : p->ctls) {
        if (pc.id != id) continue;
        if (pc.tag != Tag("iten") && pc.tag != Tag("ednu")) return 0;
        pc.hasRange = true;
        pc.lo = lo;
        pc.hi = hi;
        if (p->screen)
            if (uintptr_t c = Find(p->screen, id)) SetValueOf(c, static_cast<float>(static_cast<int32_t>(R32(c + kCtlInt))), &pc);
        return 1;
    }
    return 0;
}

int PageGetText(MmlMod *mod, bool bridge, int id, std::string *text) {
    Page *p = PageOf(mod, bridge);
    if (!p || !p->screen) return 0;
    uintptr_t c = Find(p->screen, id);
    if (!c) return 0;
    *text = TextOf(c);
    return 1;
}

std::string PagesJson() {
    json pages = json::array();
    for (auto &pg : g_pages) pages.push_back({{"name", pg->name}, {"owner", pg->bridge ? "bridge" : ModIdOf(pg->mod)}, {"open", pg->screen != 0}});
    return pages.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool Method(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "menu.click" && method != "menu.def" && method != "menu.page" && method != "menu.page.set" && method != "menu.page.close")
        return false;
    *ok = false;
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "menu.click") {
            // a control of an open screen (the active one unless `menu` names another) clicked in its middle by the
            // injected mouse: the game's own hover focus and click
            std::string want = Lower(p.value("menu", ""));
            int id = p.value("id", 0);
            uintptr_t found = 0;
            for (uintptr_t s = Top(), n = 0; s && !found && n < 16; s = Under(s), n++) {
                if (!want.empty() && Name(s) != want) continue;
                found = Find(s, id);
                if (!want.empty() || found) break;
            }
            if (!found) { *reply = "id: a control of the open menu (menu.state lists them); menu: another open menu's name"; return true; }
            float sx, sy;
            Scale(&sx, &sy);
            float x = (AbsX(found) + RF(found + kCtlW) / 2) / sx, y = (AbsY(found) + RF(found + kCtlH) / 2) / sy;
            std::string err;
            if (!InputClick(x, y, &err)) { *reply = err; return true; }
            *reply = dump({{"id", id}, {"at", {std::round(x * 10) / 10, std::round(y * 10) / 10}},
                           {"note", "the cursor moved there and the left button pressed 80 ms later, for 100 ms"}});
        } else if (method == "menu.def") {
            int design = -1;
            std::string name = Lower(p.value("name", ""));
            if (p.contains("design") && p["design"].is_number_integer()) design = p["design"].get<int>();
            else
                for (const ScreenVt &s : kScreens) if (name == s.name && s.design >= 0) { design = s.design; break; }
            if (design < 0 || design > 200) {
                *reply = "name: a screen's name (\"gamemenu\", \"mainmenu\", \"freeride\", \"loadgame\", \"optionsb\"...), or design: a group of tables\\menu.def";
                return true;
            }
            std::string err;
            json d = Design(design, &err);
            if (d.is_null()) { *reply = err; return true; }
            if (!name.empty()) d["name"] = name;
            *reply = dump(d);
        } else if (method == "menu.page") {
            if (!p.contains("controls") || !p["controls"].is_array() || p["controls"].empty()) {
                *reply = "name; controls: [{id, kind (\"wind\" first; \"wins\", \"stxt\", \"item\", \"butt\", \"line\", \"slid\", \"pbar\", \"chck\", \"bsel\", \"iten\", \"edit\", \"rctt\", \"end\"; or 1.0's own: \"ctxt\", \"cmbs\", \"scrl\", \"swnd\"...), x, y, w, h, text (options: \"A|B|C\"), flags, value, range}] in 800x600 units";
                return true;
            }
            std::vector<MmlMenuControl> controls;
            std::vector<std::string> kinds, texts;
            kinds.reserve(p["controls"].size());
            texts.reserve(p["controls"].size());
            for (const json &c : p["controls"]) {
                if (!c.is_object()) { *reply = "controls: objects"; return true; }
                kinds.push_back(c.value("kind", ""));
                texts.push_back(MenuAnsi(c.value("text", "")));
                uint32_t flags = 0;
                if (c.contains("flags")) flags = c["flags"].is_string() ? static_cast<uint32_t>(strtoul(c["flags"].get<std::string>().c_str(), nullptr, 0)) : c["flags"].get<uint32_t>();
                controls.push_back({c.value("id", 0), kinds.back().c_str(), c.value("x", 0.0f), c.value("y", 0.0f), c.value("w", 0.0f), c.value("h", 0.0f),
                                    texts.back().c_str(), flags});
            }
            std::string err;
            if (!Open(nullptr, true, p.value("name", "page").c_str(), controls.data(), static_cast<int>(controls.size()), nullptr, nullptr, &err)) { *reply = err; return true; }
            Page *pg = PageOf(nullptr, true);
            for (const json &c : p["controls"]) {
                const json &r = c.contains("range") ? c["range"] : json();
                if (r.is_array() && r.size() == 2 && !PageRange(nullptr, true, c.value("id", 0), r[0].get<int>(), r[1].get<int>()))
                    HWARN("menu: the bridge's page: control %d takes no range (iten, ednu do)", c.value("id", 0));
                if (c.contains("value") && c["value"].is_number() && !PageValue(nullptr, true, c.value("id", 0), c["value"].get<float>()))
                    HWARN("menu: the bridge's page: control %d takes no value", c.value("id", 0));
            }
            *reply = dump({{"name", pg ? pg->name : ""}, {"open", pg && pg->screen != 0},
                           {"note", pg && pg->screen ? "over the open menu; clicks are logged and listed by menu.items; Esc or menu.page.close closes it"
                                                     : "no menu was open: the pause menu is being opened, the page goes over it"}});
        } else if (method == "menu.page.set") {
            Page *pg = PageOf(nullptr, true);
            if (!pg) { *reply = "the bridge has no page open"; return true; }
            int id = p.value("id", 0);
            json out = {{"id", id}};
            if (p.contains("text")) out["text"] = PageText(nullptr, true, id, MenuAnsi(p.value("text", "")).c_str()) != 0;
            if (p.contains("range") && p["range"].is_array() && p["range"].size() == 2)
                out["range"] = PageRange(nullptr, true, id, p["range"][0].get<int>(), p["range"][1].get<int>()) != 0;
            if (p.contains("value") && p["value"].is_number()) out["value"] = PageValue(nullptr, true, id, p["value"].get<float>()) != 0;
            float v = 0;
            if (PageGetValue(nullptr, true, id, &v)) out["now"] = v;
            std::string text;
            if (PageGetText(nullptr, true, id, &text)) out["now_text"] = MenuUtf8(text.c_str());
            if (!p.contains("text") && !p.contains("value") && !p.contains("range") && !out.contains("now_text")) {
                *reply = "id; text, value, range [min, max] (or none: read them)";
                return true;
            }
            *reply = dump(out);
        } else {
            Page *pg = PageOf(nullptr, true);
            if (!pg) { *reply = "the bridge has no page open"; return true; }
            Close(pg);
            *reply = dump({{"closed", true}});
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace m10
} // namespace host
