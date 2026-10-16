// The game's own menus, extended for mods: an item added to a menu of the game's, a page of a mod's own.
//
// Nothing here is drawn by the loader. The game's menus are objects (GM_Menu) holding controls (a window, rows,
// buttons, texts) that the game makes from definitions (tables\menu\<name>.mnu: 32-byte records {id, kind, x, y,
// w, h, text id, flags}) with one factory a kind; a click or Enter on a control gives its id to the menu's command
// handler (its vtable's slot 9). Layouts, addresses: game_build.h (`menuCreate` ...); how it was found:
// knowledge/notes/interface.md.
//
// An item (mml.h menu_item): GM_Menu::Create is hooked; when a menu a mod asked for has been made, the item is
// added with the game's own GM_Menu::PridejComponentPod, the function its profile and save lists grow by: a copy
// of a control put right under it, the window grown, what is below moved down. The copy gets an id between its
// neighbours' (the keyboard walks a menu by rising id) and its text by the control's own SetText. So that the
// click reaches the mod, the menu object gets a copy of its class's vtable whose command slot is the loader's:
// ids that are items' go to their mods, every other one to the class's own handler.
//
// A page (mml.h menu_open): a GM_Menu made as the game makes its submenus (its operator new, its constructor, a
// vtable: here the base class's with three slots of the loader's: the definition's name, OnCreate, the command
// handler), put over the open menu by the game's own push. Its controls are made by the game from a definition:
// the loader answers the game's file layer (rw_data's dtaOpen, dtaRead, dtaClose, hooked when the first page is
// opened) for that one name with records built from the mod's description, so every control is made by the game's
// factories, parented, laid out and focused by the game's loader. Texts are then set in OnCreate, as the game's
// own menus set theirs. Esc closes a page (the base class's key handler); the game deletes it.
//
// Bridge methods (game thread): menu.item {menu, after, text} (an item of the bridge's own, to try a place: its
// clicks are logged), menu.items (the items asked for, the bridge's last clicks), menu.item.remove {handle},
// menu.page {name, controls: [{id, kind, x, y, w, h, text, flags}]}, menu.page.close. menu.state (start.cpp) shows
// every open menu's name and controls.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include "json.hpp"
#include "menus_shared.h"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

enum { kMenuUnder = 0x04, kMenuComps = 0x14, kMenuCompsEnd = 0x18, kMenuSize = 0x30 };
enum { kCtlId = 0x04, kCtlFlags = 0x08, kCtlTextId = 0x0C, kCtlText = 0x10, kCtlX = 0x14, kCtlY = 0x18, kCtlW = 0x1C,
       kCtlH = 0x20, kCtlRelX = 0x24, kCtlRelY = 0x28, kCtlParent = 0x2C };
enum { kSlots = 12, kSlotName = 0, kSlotOnCreate = 1, kSlotCommand = 9 };   // GM_Menu's vtable
enum { kCtlSlotTag = 2, kCtlSlotSetText = 16 };                            // a control's
constexpr uint32_t kTagEnd = 0x75707570;       // "pupu": the container is complete
constexpr uint32_t kVirtualFile = 0x4D4D4C01;  // the handle the page's definition is read through

using This0Fn = uintptr_t(__fastcall *)(uintptr_t self, void *edx);
using FindFn = uintptr_t(__fastcall *)(uintptr_t menu, void *edx, int id);
using AddUnderFn = uintptr_t(__fastcall *)(uintptr_t menu, void *edx, int id, int newId, int grow, int m1, int m2, int m3);
using CloseFn = int(__fastcall *)(uintptr_t menu, void *edx, int result);
using CommandFn = int(__fastcall *)(uintptr_t menu, void *edx, int id);
using SetTextFn = int(__fastcall *)(uintptr_t control, void *edx, const char *text);
using PushFn = int(__stdcall *)(uintptr_t menu, int arg);
using NewFn = void *(__cdecl *)(size_t size);
using CreateFn = int(__fastcall *)(uintptr_t menu, void *edx);
using DestroyFn = void(__fastcall *)(uintptr_t menu, void *edx);
using DtaOpenFn = uint32_t(__stdcall *)(const char *path, int direct);
using DtaReadFn = uint32_t(__stdcall *)(uint32_t handle, void *dst, uint32_t size);
using DtaCloseFn = uint32_t(__stdcall *)(uint32_t handle);
CreateFn oMenuCreate;
DestroyFn oMenuDestroy;
DtaOpenFn oDtaOpen;
DtaReadFn oDtaRead;
DtaCloseFn oDtaClose;
bool g_ok, g_dtaHooked;

// A copy of a menu class's vtable with the loader's command slot (the entry before a vtable is its type's record:
// copied too). A menu object's vtable pointer is &slot[0].
struct VTable {
    uintptr_t rtti;
    uintptr_t slot[kSlots];
    uintptr_t orig;      // the class's own vtable
    uintptr_t command;   // its own command handler
};
std::vector<VTable *> g_tables;   // never freed: a menu object may hold one
VTable *g_pageTable;

struct ItemReg {
    int handle;
    MmlMod *mod;         // null: the bridge's
    std::string menu;    // the definition's name, lower case
    int after;
    const char *text;    // interned
    MmlMenuFn fn;
    void *user;
};
std::vector<ItemReg> g_items;
int g_nextHandle = 1;

struct Page {
    MmlMod *mod;         // null: the bridge's
    bool bridge;
    std::string def;     // the definition's name the game asks the file layer for
    std::vector<uint8_t> blob;          // the definition: a 16-byte header, then the records
    std::vector<const char *> texts;    // by control, in the order they are made (interned; null: none)
    std::vector<int> ids;               // by control, the same order
    std::vector<uint32_t> tags;         // by control, the same order
    std::map<int, float> values;        // the controls' values (sliders, bars, checks, numbers...), set when the page is made
    std::map<int, std::pair<int, int>> ranges;   // numbers' ranges
    MmlMenuFn fn;
    void *user;
    uintptr_t menu = 0;  // the game's object once it is made
    bool waiting = false;   // for a menu to be open (the pause menu, opened by the loader's Esc)
    DWORD askedAt = 0;
};
std::vector<std::unique_ptr<Page>> g_pages;
Page *g_creating;        // the page whose definition the game is reading now
size_t g_readAt;

struct Shown { int id; int handle; };
struct LiveMenu {
    std::string name;
    std::vector<Shown> items;   // the mods' items in this menu object
    Page *page = nullptr;
};
std::map<uintptr_t, LiveMenu> g_live;   // by menu object

// An item shown in a menu whose mod has just unloaded: a reload asks for the same item again (the same menu, place
// and text) and takes this control over; what nobody has taken by the next frame is hidden.
struct Orphan { uintptr_t menu; int id; std::string mod, menuName; int after; std::string text; };
std::vector<Orphan> g_orphans;

struct Click { int id; std::string what; DWORD at; };
std::vector<Click> g_clicks;            // the bridge's own items and pages: what was clicked last

std::set<std::string> g_strings;        // texts the game's controls point at: never freed
const char *Intern(const std::string &s) { return g_strings.insert(s).first->c_str(); }

uintptr_t g_faultAt;   // where the last guarded call faulted
int Note(EXCEPTION_POINTERS *e) {
    g_faultAt = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (Note(GetExceptionInformation())) { return GetExceptionCode(); }
}

uint32_t R32(uintptr_t a) { uint32_t v = 0; MemRead(a, &v, 4); return v; }
float RF(uintptr_t a) { float v = 0; MemRead(a, &v, 4); return v; }
void W32(uintptr_t a, uint32_t v) { MemWrite(a, &v, 4); }
void WF(uintptr_t a, float v) { MemWrite(a, &v, 4); }

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// The game's strings are in its language's code page (GameCodePage), whatever the system's ANSI code page is (code page
// 1251 turned a French "é" into "й"); the bridge speaks UTF-8.
std::string Utf8(const char *ansi) {
    wchar_t w[512];
    char out[1536];
    if (MultiByteToWideChar(GameCodePage(), 0, ansi, -1, w, 512) <= 0) return "";
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof out, nullptr, nullptr) > 0 ? std::string(out) : std::string();
}
std::string Ansi(const std::string &utf8) {
    wchar_t w[512];
    char out[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w, 512) <= 0) return utf8;
    return WideCharToMultiByte(GameCodePage(), 0, w, -1, out, sizeof out, nullptr, nullptr) > 0 ? std::string(out) : utf8;
}
std::string GameString(uintptr_t p) {
    char buf[256] = {};
    if (!p) return "";
    for (size_t i = 0; i + 1 < sizeof buf; i++)
        if (!MemRead(p + i, buf + i, 1) || !buf[i]) { buf[i] = 0; break; }
    return buf;
}

VTable *Ours(uintptr_t vptr) {
    for (VTable *t : g_tables)
        if (vptr == reinterpret_cast<uintptr_t>(&t->slot[0])) return t;
    return nullptr;
}

std::vector<uintptr_t> Controls(uintptr_t menu) {
    std::vector<uintptr_t> out;
    uintptr_t b = R32(menu + kMenuComps), e = R32(menu + kMenuCompsEnd);
    if (!b || e < b || (e - b) / 4 > 4096) return out;
    out.resize((e - b) / 4);
    if (!out.empty() && !MemRead(b, out.data(), out.size() * 4)) out.clear();
    return out;
}

std::string DefNameOf(uintptr_t menu) {
    uintptr_t name = 0, fn = R32(R32(menu) + 4 * kSlotName);
    auto call = [&] { name = reinterpret_cast<This0Fn>(fn)(menu, nullptr); };
    if (!fn || Guarded(call)) return "";
    return Lower(GameString(name));
}

bool SetText(uintptr_t control, const char *text) {
    uintptr_t fn = R32(R32(control) + 4 * kCtlSlotSetText);
    auto call = [&] { reinterpret_cast<SetTextFn>(fn)(control, nullptr, text); };
    return fn && !Guarded(call);
}

uint32_t TagOf(uintptr_t control) {
    uint32_t tag = 0;
    uintptr_t fn = R32(R32(control) + 4 * kCtlSlotTag);
    auto call = [&] { tag = static_cast<uint32_t>(reinterpret_cast<This0Fn>(fn)(control, nullptr)); };
    return fn && !Guarded(call) ? tag : 0;
}

// ---- clicks
struct CallArgs { MmlMenuFn fn; int id; void *user; };
void CallTramp(void *p) {
    CallArgs *a = static_cast<CallArgs *>(p);
    a->fn(a->id, a->user);
}
// An owner whose items and pages are live: a loaded mod, or the loader's own (panel.cpp: the mods' settings).
bool OwnerAlive(MmlMod *mod) { return LoaderOwned(mod) || ModAlive(mod); }

void Deliver(MmlMod *mod, MmlMenuFn fn, void *user, int id, const std::string &what) {
    if (LoaderOwned(mod)) {   // the loader's own: its code, called as it is (with the x87 at 53 bits, as a mod's)
        uint16_t fpu = FpuDouble();
        if (fn) fn(id, user);
        FpuRestore(fpu);
        return;
    }
    if (!mod) {   // the bridge's own: told in the log, kept for menu.items
        HLOG("menu: %s clicked (id %d)", what.c_str(), id);
        g_clicks.push_back({id, what, GetTickCount()});
        if (g_clicks.size() > 32) g_clicks.erase(g_clicks.begin());
        return;
    }
    if (!fn || !ModAlive(mod)) return;
    CallArgs args{fn, id, user};
    ModCall(mod, CallTramp, &args, "a menu callback");
}

// The command slot of every vtable of ours: a mod's item, a page's control, or the class's own handler.
int __fastcall MenuCommand(uintptr_t menu, void *edx, int id) {
    auto it = g_live.find(menu);
    if (it != g_live.end()) {
        for (const Shown &s : it->second.items) {
            if (s.id != id) continue;
            for (const ItemReg &r : g_items)
                if (r.handle == s.handle) {
                    ItemReg reg = r;   // (its function may add or remove items)
                    Deliver(reg.mod, reg.fn, reg.user, id, std::string("item '") + reg.text + "' of " + reg.menu);
                    return 0;
                }
            return 0;   // its mod is gone
        }
        if (Page *page = it->second.page) {
            if (page->bridge || page->mod) Deliver(page->mod, page->fn, page->user, id, "page " + page->def);
            return 0;
        }
    }
    VTable *t = Ours(R32(menu));
    if (!t || !t->command) return 0;
    return reinterpret_cast<CommandFn>(t->command)(menu, edx, id);
}

VTable *TableFor(uintptr_t classVt) {
    for (VTable *t : g_tables)
        if (t->orig == classVt && t != g_pageTable) return t;
    VTable *t = new VTable{};
    if (!MemRead(classVt - 4, &t->rtti, 4 + 4 * kSlots)) { delete t; return nullptr; }
    t->orig = classVt;
    t->command = t->slot[kSlotCommand];
    t->slot[kSlotCommand] = reinterpret_cast<uintptr_t>(&MenuCommand);
    g_tables.push_back(t);
    return t;
}

// ---- items: added to a menu the game has just made
void AddItems(uintptr_t menu, LiveMenu *live) {
    const game::ExeBuild &e = game::Exe();
    std::map<int, int> placed;   // the control an item was asked after -> the item put there last
    for (size_t k = 0; k < g_items.size(); k++) {
        ItemReg reg = g_items[k];
        if (reg.menu != live->name || (reg.mod && !OwnerAlive(reg.mod))) continue;
        std::vector<uintptr_t> controls = Controls(menu);
        int afterId = placed.count(reg.after) ? placed[reg.after] : reg.after;
        uintptr_t src = 0;
        for (uintptr_t c : controls) if (static_cast<int>(R32(c + kCtlId)) == afterId) { src = c; break; }
        if (!src) {
            HWARN("menu: '%s' has no control %d: the item '%s' of %s is not added", reg.menu.c_str(), reg.after, reg.text, ModIdOf(reg.mod));
            continue;
        }
        // An id between the control's and the next one in use: the keyboard walks a menu by rising id.
        uint32_t srcId = R32(src + kCtlId), next = 0xFFFFFFFF, highest = 0;
        for (uintptr_t c : controls) {
            uint32_t id = R32(c + kCtlId);
            if (id > srcId && id < next) next = id;
            if (id > highest) highest = id;
        }
        uint32_t newId = next - srcId >= 2 ? srcId + 1 : highest + 1;
        if (newId != srcId + 1)
            HWARN("menu: '%s' has no free id after %u: the item '%s' gets %u (the keyboard reaches it last)", reg.menu.c_str(), srcId, reg.text, newId);
        // What grows and what moves: the control's parent (the window), and its siblings below it. The game's
        // function puts the copy at the control's bottom and moves by its height; rows may be closer than their
        // height (the main menu's buttons: 30 high, 25 or 26 apart), so the copy goes one row's pitch down and
        // everything below it moves by that pitch (read below, set after the game's function).
        uintptr_t parent = R32(src + kCtlParent);
        if (!parent) {
            HWARN("menu: control %u of '%s' has no window: the item '%s' is not added", srcId, reg.menu.c_str(), reg.text);
            continue;
        }
        float top = RF(src + kCtlRelY), h = RF(src + kCtlH), pitch = h;
        std::vector<uintptr_t> below;
        for (uintptr_t c : controls)
            if (c != src && R32(c + kCtlParent) == parent && RF(c + kCtlRelY) > top + 0.5f) {
                below.push_back(c);
                pitch = std::min(pitch, RF(c + kCtlRelY) - top);
            }
        if (pitch < h * 0.5f) pitch = h;   // something beside it, not a row under it
        int grow = static_cast<int>(R32(parent + kCtlId));
        uintptr_t added = 0;
        auto call = [&] {
            reinterpret_cast<AddUnderFn>(e.menuAddUnder)(menu, nullptr, static_cast<int>(srcId), static_cast<int>(newId), grow, 0, 0, 0);
            added = reinterpret_cast<FindFn>(e.menuFind)(menu, nullptr, static_cast<int>(newId));
        };
        if (DWORD ex = Guarded(call)) {
            HWARN("menu: adding the item '%s' to '%s' faulted (%08X)", reg.text, reg.menu.c_str(), static_cast<unsigned>(ex));
            continue;
        }
        if (!added || added == src) {
            HWARN("menu: the game did not add the item '%s' to '%s' (control %u cannot be copied)", reg.text, reg.menu.c_str(), srcId);
            continue;
        }
        WF(added + kCtlRelY, top + pitch);
        WF(parent + kCtlH, RF(parent + kCtlH) - RF(added + kCtlH) + pitch);   // (the game's function grew it by the height)
        for (uintptr_t c : below) WF(c + kCtlRelY, RF(c + kCtlRelY) + pitch);
        W32(added + kCtlFlags, R32(added + kCtlFlags) & 0x7FFFFFFF);   // the copy of a focused control is not focused
        SetText(added, reg.text);
        live->items.push_back({static_cast<int>(newId), reg.handle});
        placed[reg.after] = static_cast<int>(newId);
    }
    if (live->items.empty() || Ours(R32(menu))) return;
    if (VTable *t = TableFor(R32(menu))) W32(menu, reinterpret_cast<uintptr_t>(&t->slot[0]));
}

int __fastcall hkMenuCreate(uintptr_t menu, void *edx) {
    int r = oMenuCreate(menu, edx);
    LiveMenu &live = g_live[menu];
    live.items.clear();
    live.name = DefNameOf(menu);
    if (!live.name.empty()) AddItems(menu, &live);
    return r;
}

void __fastcall hkMenuDestroy(uintptr_t menu, void *edx) {
    Page *page = nullptr;
    auto it = g_live.find(menu);
    if (it != g_live.end()) { page = it->second.page; g_live.erase(it); }
    oMenuDestroy(menu, edx);
    if (!page) return;
    for (size_t i = 0; i < g_pages.size(); i++)
        if (g_pages[i].get() == page) { g_pages.erase(g_pages.begin() + i); break; }
}

// ---- pages: the game reads the definition through its file layer; the loader answers for that one name
bool IsPageFile(const char *path) {
    if (!g_creating || !path) return false;
    char name[260] = {};
    auto copy = [&] { strncpy(name, path, sizeof name - 1); };
    if (Guarded(copy)) return false;
    std::string p = Lower(name), want = "tables\\menu\\" + g_creating->def;
    return p.compare(0, want.size(), want) == 0 && (p[want.size()] == '.' || p[want.size()] == '_');
}
uint32_t __stdcall hkDtaOpen(const char *path, int direct) {
    if (!IsPageFile(path)) return oDtaOpen(path, direct);
    g_readAt = 0;
    return kVirtualFile;
}
uint32_t __stdcall hkDtaRead(uint32_t handle, void *dst, uint32_t size) {
    if (handle != kVirtualFile || !g_creating) return oDtaRead(handle, dst, size);
    size_t left = g_creating->blob.size() - g_readAt, n = size < left ? size : left;
    memcpy(dst, g_creating->blob.data() + g_readAt, n);
    g_readAt += n;
    return static_cast<uint32_t>(n);
}
uint32_t __stdcall hkDtaClose(uint32_t handle) { return handle == kVirtualFile ? 1 : oDtaClose(handle); }

bool HookFiles(std::string *err) {
    if (g_dtaHooked) return true;
    HMODULE rw = GetModuleHandleA("rw_data.dll");
    void *open = rw ? GetProcAddress(rw, "_dtaOpen@8") : nullptr, *read = rw ? GetProcAddress(rw, "_dtaRead@12") : nullptr,
         *close = rw ? GetProcAddress(rw, "_dtaClose@4") : nullptr;
    if (!open || !read || !close) { *err = "rw_data.dll's dtaOpen, dtaRead, dtaClose are not found"; return false; }
    if (!HookAdd(nullptr, reinterpret_cast<uintptr_t>(open), reinterpret_cast<void *>(&hkDtaOpen), reinterpret_cast<void **>(&oDtaOpen), err) ||
        !HookAdd(nullptr, reinterpret_cast<uintptr_t>(read), reinterpret_cast<void *>(&hkDtaRead), reinterpret_cast<void **>(&oDtaRead), err) ||
        !HookAdd(nullptr, reinterpret_cast<uintptr_t>(close), reinterpret_cast<void *>(&hkDtaClose), reinterpret_cast<void **>(&oDtaClose), err))
        return false;
    HLOG("menus: the game's file layer answers for the pages' definitions (rw_data.dll hooked)");
    return g_dtaHooked = true;
}

const char *__fastcall PageDefName(uintptr_t menu, void *) {
    auto it = g_live.find(menu);
    return it != g_live.end() && it->second.page ? it->second.page->def.c_str() : nullptr;
}
// ---- texts of the loader's own: ids the game's text database answers from the loader. A selector's options are
// text ids (it walks a range of them, G_TextDatabase::GetText gives each one's string), so a mod's options get ids
// here. GetText and GetTextOrNULL (thiscall(db, id) ret 4) answer ids past kTextBase from this table.
constexpr uint32_t kTextBase = 0x4D4D0000;
std::vector<const char *> g_ourTexts;   // id - kTextBase - 1
using GetTextFn = const char *(__fastcall *)(uintptr_t db, void *edx, uint32_t id);
GetTextFn oGetText, oGetTextOrNull;
bool g_textsOk;
// The game's own ids answered by the loader (items.cpp: a mod's item's name at 3300 + its id...), interned strings.
std::map<uint32_t, const char *> g_setTexts;
const char *OurText(uint32_t id) {
    uint32_t i = id - kTextBase - 1;
    if (id > kTextBase && i < g_ourTexts.size()) return g_ourTexts[i];
    if (!g_setTexts.empty()) {
        auto it = g_setTexts.find(id);
        if (it != g_setTexts.end()) return it->second;
    }
    return nullptr;
}
const char *__fastcall hkGetText(uintptr_t db, void *edx, uint32_t id) {
    const char *t = OurText(id);
    return t ? t : oGetText(db, edx, id);
}
const char *__fastcall hkGetTextOrNull(uintptr_t db, void *edx, uint32_t id) {
    const char *t = OurText(id);
    return t ? t : oGetTextOrNull(db, edx, id);
}
// Consecutive ids for these strings (0: the texts are not hooked); the same strings again: the same ids (a page
// opened again takes no new ones).
std::map<std::string, uint32_t> g_textBlocks;   // the strings joined by '\n' -> the first id
uint32_t TextIds(const std::vector<std::string> &strings) {
    if (!g_textsOk || strings.empty()) return 0;
    std::string key;
    for (const std::string &s : strings) key += s + '\n';
    auto it = g_textBlocks.find(key);
    if (it != g_textBlocks.end()) return it->second;
    uint32_t first = kTextBase + 1 + static_cast<uint32_t>(g_ourTexts.size());
    for (const std::string &s : strings) g_ourTexts.push_back(Intern(s));
    return g_textBlocks[key] = first;
}

// ---- the kinds' values, each by the control's own functions (read in their code, 2026-10-13):
//   slid, pbar   slot 19 SetValue(float 0..1), kept at +0x40 (slid 0x5DBA50, pbar 0x5DF350)
//   wins         slot 19 SetScroll(float pixels, 0..+0x40 the range), +0x44 (0x5D9AB0); slot 27 SetRange(0, float)
//   chck         slot 23 SetCheck(bool) / 24 GetCheck, +0x40 (0x5DCCC0)
//   bslb         slot 29 SetTexts(id off, id on) (0x5DCFD0); 23 SetState(bool): +0x48, the text id shown (0x5DD000)
//   bsel         slot 29 SetRange(first id, last id) (0x5DCE30); its option is its own text id (+0xC), stepped
//                through the range by a click, Enter or Space; set by slot 14 SetTextID
//   iten, edin   slot 29 SetRange(min, max), 21 SetValue(int): clamped, +0x48, its text printed with it (iten:
//                its own text as the format, "Laps :%u"), 22 GetValue (0x5DB0B0, 0x5DB0F0, 0x5DD2A0, 0x5DD2E0)
//   sldi         slot 29 SetRange, 21 SetValue(int) (0x5DBF20)
//   edit         slot 16 SetText: copied into its buffer, 18 GetText: the buffer (+0x40, at most 63), 21 SetMaxLength
constexpr uint32_t kTagSlider = 0x736C6964, kTagBar = 0x70626172, kTagScroll = 0x77696E73, kTagCheck = 0x6368636B,
                   kTagSwitch = 0x62736C62, kTagSelect = 0x6273656C, kTagNumber = 0x6974656E, kTagNumEdit = 0x6564696E,
                   kTagSteps = 0x736C6469, kTagEdit = 0x65646974,   // slid pbar wins chck bslb bsel iten edin sldi edit
                   kTagPicture = 0x72637474;   // rctt
// A picture ("rctt"): its text is an image file the engine loads as a texture, by the control's slot 17
// SetString(name) (0x5DAA70: the old texture released, IGraph's CreateTexture(name, 0, 2, &+0x40)); the load menu
// shows a save's shot this way (GM_Menu::SetString 0x5EB030 with "shot%03d.bmp"). Drawn as the game's filled
// rectangle with that texture (0x5DAAA0).
enum { kCtlSlotSetString = 17 };
using StrFn = void(__fastcall *)(uintptr_t, void *, const char *);
bool SetPicture(uintptr_t control, const char *file) {
    uintptr_t fn = R32(R32(control) + 4 * kCtlSlotSetString);
    auto call = [&] { reinterpret_cast<StrFn>(fn)(control, nullptr, file); };
    return fn && !Guarded(call);
}
enum { kCtlSlotSetTextId = 14, kCtlSlotGetText = 18, kCtlSlotSetValue = 19, kCtlSlotSetInt = 21, kCtlSlotGetInt = 22,
       kCtlSlotSetCheck = 23, kCtlSlotSetRange = 27, kCtlSlotSetPair = 29, kCtlValue = 0x40 };
bool HasValue(uint32_t tag) {
    return tag == kTagSlider || tag == kTagBar || tag == kTagScroll || tag == kTagCheck || tag == kTagSwitch || tag == kTagSelect ||
           tag == kTagNumber || tag == kTagNumEdit || tag == kTagSteps;
}
bool HasRange(uint32_t tag) { return tag == kTagNumber || tag == kTagNumEdit || tag == kTagSteps; }
bool HasOptions(uint32_t tag) { return tag == kTagSwitch || tag == kTagSelect; }

template <class Fn, class... A> bool Slot(uintptr_t control, int slot, A... a) {
    uintptr_t fn = R32(R32(control) + 4 * slot);
    auto call = [&] { reinterpret_cast<Fn>(fn)(control, nullptr, a...); };
    return fn && !Guarded(call);
}
using FloatFn = void(__fastcall *)(uintptr_t, void *, float);
using IntFn = void(__fastcall *)(uintptr_t, void *, int);
using PairFn = void(__fastcall *)(uintptr_t, void *, int, int);

bool SetValue(uintptr_t control, float value) {
    uint32_t tag = TagOf(control);
    float unit = value < 0 ? 0 : value > 1 ? 1 : value;
    int n = static_cast<int>(lroundf(value));
    switch (tag) {
    case kTagSlider: case kTagBar: return Slot<FloatFn>(control, kCtlSlotSetValue, unit);
    case kTagScroll: return Slot<FloatFn>(control, kCtlSlotSetValue, unit * RF(control + 0x40));
    case kTagCheck: case kTagSwitch: return Slot<IntFn>(control, kCtlSlotSetCheck, value != 0 ? 1 : 0);
    case kTagNumber: case kTagNumEdit: case kTagSteps: return Slot<IntFn>(control, kCtlSlotSetInt, n);
    case kTagSelect: {
        int first = static_cast<int>(R32(control + 0x40)), last = static_cast<int>(R32(control + 0x44));
        if (!first || last < first) return false;
        int id = first + n;
        return Slot<IntFn>(control, kCtlSlotSetTextId, id < first ? first : id > last ? last : id);
    }
    default: return false;
    }
}
bool GetValue(uintptr_t control, float *value) {
    uint32_t tag = TagOf(control);
    switch (tag) {
    case kTagSlider: case kTagBar: *value = RF(control + kCtlValue); return true;
    case kTagScroll: { float range = RF(control + 0x40); *value = range > 0 ? RF(control + 0x44) / range : 0; return true; }
    case kTagCheck: { uint8_t b = 0; MemRead(control + 0x40, &b, 1); *value = b ? 1.0f : 0.0f; return true; }
    case kTagSwitch: { uint8_t b = 0; MemRead(control + 0x48, &b, 1); *value = b ? 1.0f : 0.0f; return true; }
    case kTagNumber: case kTagNumEdit: case kTagSteps: *value = static_cast<float>(static_cast<int32_t>(R32(control + 0x48))); return true;
    case kTagSelect: *value = static_cast<float>(static_cast<int32_t>(R32(control + 0xC) - R32(control + 0x40))); return true;
    default: return false;
    }
}
bool SetRange(uintptr_t control, int lo, int hi) {
    return HasRange(TagOf(control)) && lo <= hi && Slot<PairFn>(control, kCtlSlotSetPair, lo, hi);
}
// A switch's or a selector's options: text ids of the loader's for the strings, the control given their range.
bool SetOptions(uintptr_t control, const std::vector<std::string> &options) {
    uint32_t tag = TagOf(control);
    if (!HasOptions(tag) || options.empty() || (tag == kTagSwitch && options.size() != 2)) return false;
    uint32_t first = TextIds(options);
    if (!first) return false;
    int last = static_cast<int>(first + options.size() - 1);
    if (!Slot<PairFn>(control, kCtlSlotSetPair, static_cast<int>(first), last)) return false;
    // the shown text: the switch's state's, the selector's first option
    return tag == kTagSwitch ? Slot<IntFn>(control, kCtlSlotSetCheck, 0) : Slot<IntFn>(control, kCtlSlotSetTextId, static_cast<int>(first));
}
std::string ControlText(uintptr_t control) {
    if (TagOf(control) == kTagEdit) {
        uintptr_t text = 0, fn = R32(R32(control) + 4 * kCtlSlotGetText);
        auto call = [&] { text = reinterpret_cast<This0Fn>(fn)(control, nullptr); };
        return fn && !Guarded(call) ? GameString(text) : std::string();
    }
    return GameString(R32(control + kCtlText));
}
std::vector<std::string> SplitOptions(const std::string &text) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        size_t bar = text.find('|', at);
        out.push_back(text.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    return out;
}

// A scrolled window ("wins") scrolls as far as its children reach: what the load menu sets after filling its list
// (0x5E2531: the last row's bottom - the window's height + 4, pixels, by the window's slot 27, thiscall(w, 0, range)).
using SetRangeFn = void(__fastcall *)(uintptr_t control, void *edx, int zero, float range);
void SetScrollRanges(const std::vector<uintptr_t> &controls) {
    for (uintptr_t w : controls) {
        if (TagOf(w) != kTagScroll) continue;
        float bottom = 0;
        for (uintptr_t c : controls)
            if (R32(c + kCtlParent) == w) bottom = std::max(bottom, RF(c + kCtlRelY) + RF(c + kCtlH));
        float range = std::max(0.0f, bottom - RF(w + kCtlH) + 4.0f);
        uintptr_t fn = R32(R32(w) + 4 * kCtlSlotSetRange);
        auto call = [&] { reinterpret_cast<SetRangeFn>(fn)(w, nullptr, 0, range); };
        if (fn) Guarded(call);
    }
}

int __fastcall PageOnCreate(uintptr_t menu, void *) {
    auto it = g_live.find(menu);
    if (it == g_live.end() || !it->second.page) return 0;
    Page *page = it->second.page;
    std::vector<uintptr_t> controls = Controls(menu);
    for (size_t i = 0; i < controls.size() && i < page->texts.size(); i++) {
        if (!page->texts[i]) continue;
        if (HasOptions(page->tags[i])) SetOptions(controls[i], SplitOptions(page->texts[i]));   // "Off|On", "Low|Medium|High"
        else if (page->tags[i] == kTagPicture) SetPicture(controls[i], page->texts[i]);
        else if (R32(controls[i] + kCtlTextId) <= kTextBase) SetText(controls[i], page->texts[i]);   // (no text id: set here)
    }
    SetScrollRanges(controls);
    for (uintptr_t c : controls) {
        int id = static_cast<int>(R32(c + kCtlId));
        auto r = page->ranges.find(id);
        if (r != page->ranges.end()) SetRange(c, r->second.first, r->second.second);
        auto v = page->values.find(id);
        if (v != page->values.end()) SetValue(c, v->second);
    }
    return 0;
}

uint32_t TagOfKind(const std::string &kind) {
    if (kind.size() != 4) return 0;
    return static_cast<uint8_t>(kind[0]) << 24 | static_cast<uint8_t>(kind[1]) << 16 | static_cast<uint8_t>(kind[2]) << 8 | static_cast<uint8_t>(kind[3]);
}
std::string KindOfTag(uint32_t tag) {
    char k[5] = {static_cast<char>(tag >> 24), static_cast<char>(tag >> 16), static_cast<char>(tag >> 8), static_cast<char>(tag), 0};
    for (int i = 0; i < 4; i++) if (k[i] < 32 || k[i] > 126) return "";
    return k;
}
bool KindKnown(uint32_t tag) {   // the game has a factory for it
    int n = 0;
    for (uintptr_t node = R32(game::Exe().menuFactories); node && n < 256; node = R32(node + 0xC), n++)
        if (R32(node + 8) == tag) return true;
    return false;
}

// The page put over the active menu: made as the game makes its submenus.
bool PushNow(Page *page) {
    const game::ExeBuild &e = game::Exe();
    uintptr_t menu = 0;
    auto make = [&] {
        menu = reinterpret_cast<uintptr_t>(reinterpret_cast<NewFn>(e.gameNew)(kMenuSize));
        if (menu) reinterpret_cast<This0Fn>(e.menuCtor)(menu, nullptr);
    };
    if (Guarded(make) || !menu) { HWARN("menu: the page %s could not be made", page->def.c_str()); return false; }
    W32(menu, reinterpret_cast<uintptr_t>(&g_pageTable->slot[0]));
    g_live[menu].page = page;
    page->menu = menu;
    page->waiting = false;
    g_creating = page;
    auto push = [&] { reinterpret_cast<PushFn>(e.menuPush)(menu, 0); };
    DWORD ex = Guarded(push);
    g_creating = nullptr;
    if (ex) {
        // The push made it the active menu before its controls were made: the menu under it is made the active one
        // again, so that the half-made page is never drawn or ticked (its object is left as it is: not freed).
        if (R32(e.menuTop) == menu) W32(e.menuTop, R32(menu + kMenuUnder));
        g_live.erase(menu);
        page->menu = 0;
        HWARN("menu: opening the page %s faulted (%08X at %s): the page is dropped", page->def.c_str(), static_cast<unsigned>(ex),
              DescribeAddress(g_faultAt).c_str());
        return false;
    }
    size_t made = Controls(menu).size();
    if (made != page->texts.size())
        HWARN("menu: the page %s: the game made %zu of %zu controls", page->def.c_str(), made, page->texts.size());
    return true;
}

Page *PageOf(MmlMod *mod, bool bridge) {
    for (auto &p : g_pages)
        if (p->bridge == bridge && p->mod == mod && (bridge || mod)) return p.get();
    return nullptr;
}

void ClosePage(Page *page) {
    if (!page->menu) {   // it waits for a menu: forgotten
        for (size_t i = 0; i < g_pages.size(); i++)
            if (g_pages[i].get() == page) { g_pages.erase(g_pages.begin() + i); break; }
        return;
    }
    uintptr_t menu = page->menu;
    auto call = [&] { reinterpret_cast<CloseFn>(game::Exe().menuClose)(menu, nullptr, -1); };
    Guarded(call);
}

int OpenPage(MmlMod *mod, bool bridge, const char *name, const MmlMenuControl *controls, int count, MmlMenuFn fn, void *user, std::string *err) {
    if (!g_ok) { *err = "the menus are not known for this build"; return 0; }
    if (!name || !controls || count < 1 || count > 256) { *err = "a name and 1..256 controls"; return 0; }
    std::string n = Lower(name);
    if (n.empty() || n.size() > 32 || n.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
        *err = "name: letters, digits, _ (at most 32)";
        return 0;
    }
    if (PageOf(mod, bridge)) { *err = "a page of this owner is open already (one at a time)"; return 0; }
    if (!HookFiles(err)) return 0;
    auto page = std::make_unique<Page>();
    page->mod = mod;
    page->bridge = bridge;
    page->fn = fn;
    page->user = user;
    page->def = Lower(std::string(bridge ? "bridge" : ModIdOf(mod)) + "_" + n);
    // The definition as the game's loader reads it (0x5EA370): the first control is the page's window; a container
    // (wind, winc, wins, grid: their vtable's slot 4 says so) holds the controls that follow it; "end" returns to the
    // page's window (the loader walks up to the topmost parent), so containers inside containers are refused.
    // A scrolled window (wins) clips to itself when it is drawn (0x5E98A0 at the end of its Draw) and leaves the
    // clip set for what is drawn after it: its block goes last among the page's controls, whatever order it was
    // given in (the keyboard walks the controls by id, not by this order).
    struct Record { int id; uint32_t tag; float x, y, w, h; uint32_t textId, flags; };
    struct Entry { Record rec; const char *text; int block; };   // block: -1 the page's window, else a wins' number
    std::vector<Entry> entries;
    int depth = 0, block = -1, blocks = 0;
    for (int i = 0; i < count; i++) {
        const MmlMenuControl &c = controls[i];
        std::string kind = Lower(c.kind ? c.kind : "");
        Record rec{};
        if (kind == "end") {
            if (depth == 0) { *err = "control " + std::to_string(i) + ": \"end\" with no window open"; return 0; }
            if (depth == 1) continue;   // (the page's own window: nothing to close)
            depth = 1;
            rec.tag = kTagEnd;
            entries.push_back({rec, nullptr, block});
            block = -1;
            continue;
        }
        rec.tag = TagOfKind(kind);
        if (!rec.tag || rec.tag == kTagEnd || !KindKnown(rec.tag)) {
            *err = "control " + std::to_string(i) + ": the game has no control kind \"" + kind + "\" (wind, wins, stxt, item, butt, line, slid, pbar, chck, bslb, bsel, iten, edit ...)";
            return 0;
        }
        if (!(c.x == c.x && c.y == c.y && c.w == c.w && c.h == c.h)) { *err = "control " + std::to_string(i) + ": x, y, w, h must be numbers"; return 0; }
        bool container = kind == "wind" || kind == "winc" || kind == "wins" || kind == "grid", window = kind == "wind" || kind == "winc",
             clicked = kind == "item" || kind == "butt" || kind == "butc" || kind == "bsel" || kind == "bslb" || kind == "iten";
        if (i == 0 && !window) { *err = "the first control must be a window (\"wind\")"; return 0; }
        if (i > 0 && container && depth > 1) { *err = "control " + std::to_string(i) + ": a container in a container (\"end\" the one before: the game's \"end\" returns to the page's window)"; return 0; }
        rec.id = c.id;
        // A text is given as the game's files give it: a text id (of the loader's), so the control has it from its
        // making on (a number field formats its text in its constructor). Options ("A|B") are set once it is made.
        if (c.text && *c.text && !HasOptions(rec.tag) && rec.tag != kTagPicture) rec.textId = TextIds({c.text});
        rec.x = c.x; rec.y = c.y; rec.w = c.w; rec.h = c.h;
        // The kind's usual flags, as the game's own definitions have them (seen in gamemenu, optionsa, optionsc1).
        uint32_t usual = window ? 0xB2u : clicked ? 0xB1u : 0x30u;
        rec.flags = (c.flags ? (c.flags & 0xFFFF) | 0x30u : usual) | (c.flags & 0xFFFF0000u ? c.flags & 0xFFFF0000u : 0x00FF0000u);
        if (rec.tag == kTagScroll) block = blocks++;
        entries.push_back({rec, c.text && *c.text ? Intern(c.text) : nullptr, block});
        if (container) depth++;
    }
    page->blob.assign(16, 0);
    const uint32_t magic[2] = {0x756E654D, 1};   // "Menu", version 1: the game's own files' header
    memcpy(page->blob.data(), magic, 8);
    for (int pass = -1; pass < blocks; pass++)
        for (const Entry &en : entries) {
            if (en.block != pass) continue;
            if (en.rec.tag != kTagEnd) {
                page->texts.push_back(en.text);
                page->ids.push_back(en.rec.id);
                page->tags.push_back(en.rec.tag);
            }
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&en.rec);
            page->blob.insert(page->blob.end(), bytes, bytes + sizeof en.rec);
        }
    uint32_t records = static_cast<uint32_t>((page->blob.size() - 16) / 32);
    memcpy(page->blob.data() + 8, &records, 4);
    Page *p = page.get();
    g_pages.push_back(std::move(page));
    if (R32(game::Exe().menuTop)) {
        if (!PushNow(p)) { *err = "the game could not make the page (the log tells)"; if (!p->menu) g_pages.pop_back(); return 0; }
        return 1;
    }
    // No menu is open: the pause menu is opened first (the game's own Esc), the page goes over it.
    if (MissionState() != MML_MISSION_RUNNING) { g_pages.pop_back(); *err = "no menu is open and no mission runs: there is nothing to put the page over"; return 0; }
    p->waiting = true;
    p->askedAt = GetTickCount();
    InputTapKey(1 /* Esc */, 2);
    return 1;
}

// A definition of the game's (tables\menu\<name>.mnu, the language's first) read through its file layer, as its
// loader reads it: what the game's pages are made of, for agents to copy (menu.def).
json Definition(const std::string &name, std::string *err) {
    HMODULE rw = GetModuleHandleA("rw_data.dll");
    auto open = rw ? reinterpret_cast<DtaOpenFn>(GetProcAddress(rw, "_dtaOpen@8")) : nullptr;
    auto read = rw ? reinterpret_cast<DtaReadFn>(GetProcAddress(rw, "_dtaRead@12")) : nullptr;
    auto close = rw ? reinterpret_cast<DtaCloseFn>(GetProcAddress(rw, "_dtaClose@4")) : nullptr;
    if (!open || !read || !close) { *err = "rw_data.dll's dtaOpen, dtaRead, dtaClose are not found"; return json(); }
    uint16_t lang = 0;
    if (game::g_exe && game::Exe().menuLang) MemRead(game::Exe().menuLang, &lang, 2);
    char path[300];
    int32_t h = -1;
    if (lang) {
        snprintf(path, sizeof path, "tables\\menu\\%s_%c%c.mnu", name.c_str(), static_cast<char>(lang), static_cast<char>(lang >> 8));
        h = static_cast<int32_t>(open(path, 0));
    }
    if (h < 0) {
        snprintf(path, sizeof path, "tables\\menu\\%s.mnu", name.c_str());
        h = static_cast<int32_t>(open(path, 0));
    }
    if (h < 0) { *err = std::string("no ") + path + " in the game's files"; return json(); }
    uint32_t header[4] = {};
    read(static_cast<uint32_t>(h), header, 16);
    std::vector<uint8_t> recs(header[2] <= 4096 ? header[2] * 32 : 0);
    if (!recs.empty()) read(static_cast<uint32_t>(h), recs.data(), static_cast<uint32_t>(recs.size()));
    close(static_cast<uint32_t>(h));
    json out = {{"file", path}, {"header", {header[0], header[1], header[2], header[3]}}, {"controls", json::array()}};
    using GetFn = const char *(__fastcall *)(uintptr_t db, void *edx, unsigned id);
    int depth = 0;
    for (size_t at = 0; at + 32 <= recs.size(); at += 32) {
        struct Record { int id; uint32_t tag; float x, y, w, h; uint32_t textId, flags; } r;
        memcpy(&r, recs.data() + at, 32);
        if (r.tag == kTagEnd) {
            out["controls"].push_back({{"kind", "end"}, {"depth", depth > 0 ? --depth : 0}});
            continue;
        }
        std::string kind = KindOfTag(r.tag);
        char f[16];
        snprintf(f, sizeof f, "0x%08X", r.flags);
        json c = {{"id", r.id}, {"kind", kind}, {"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}, {"flags", f}, {"depth", depth}};
        if (r.textId && game::g_exe) {
            c["text_id"] = r.textId;
            const char *t = nullptr;
            const game::ExeBuild &e = game::Exe();
            auto get = [&] { t = reinterpret_cast<GetFn>(e.textGetOrNull)(e.textDb, nullptr, r.textId); };
            if (e.textGetOrNull && e.textDb && !Guarded(get) && t) c["text"] = Utf8(t);
        }
        out["controls"].push_back(c);
        if (kind.compare(0, 3, "win") == 0) depth++;
    }
    return out;
}

json Described(uintptr_t menu) {
    json out = {{"name", ""}, {"controls", json::array()}};
    auto it = g_live.find(menu);
    out["name"] = it != g_live.end() && !it->second.name.empty() ? it->second.name : DefNameOf(menu);
    if (it != g_live.end() && it->second.page) out["page_of"] = it->second.page->bridge ? "bridge" : ModIdOf(it->second.page->mod);
    float sx = 1, sy = 1;   // the menus' own scale (set by their loop; the HUD's where it is not known)
    if (game::Exe().menuScale) {
        sx = RF(game::Exe().menuScale);
        sy = RF(game::Exe().menuScale + 4);
    } else if (game::Exe().hud && game::Exe().hudScale) {
        sx = RF(game::Exe().hud + game::Exe().hudScale);
        sy = RF(game::Exe().hud + game::Exe().hudScale + 4);
    }
    if (!(sx > 0.01f)) sx = 1;
    if (!(sy > 0.01f)) sy = 1;
    auto unit = [](float v) { return std::round(v * 10.0f) / 10.0f; };
    for (uintptr_t c : Controls(menu)) {
        uint32_t flags = R32(c + kCtlFlags), parent = R32(c + kCtlParent);
        char f[16];
        snprintf(f, sizeof f, "0x%08X", flags);
        json j = {{"id", static_cast<int>(R32(c + kCtlId))}, {"kind", KindOfTag(TagOf(c))}, {"flags", f},
                  {"text", Utf8(GameString(R32(c + kCtlText)).c_str())}, {"text_id", R32(c + kCtlTextId)},
                  {"x", unit(RF(c + kCtlRelX) / sx)}, {"y", unit(RF(c + kCtlRelY) / sy)}, {"w", unit(RF(c + kCtlW) / sx)}, {"h", unit(RF(c + kCtlH) / sy)},
                  {"parent", parent ? json(static_cast<int>(R32(parent + kCtlId))) : json()}, {"focused", (flags & 0x80000000u) != 0},
                  {"screen", {unit(RF(c + kCtlX) / sx), unit(RF(c + kCtlY) / sy), unit(RF(c + kCtlW) / sx), unit(RF(c + kCtlH) / sy)}}};
        float v = 0;
        if (GetValue(c, &v)) j["value"] = std::round(v * 1000) / 1000;
        if (TagOf(c) == kTagEdit) j["text"] = Utf8(ControlText(c).c_str());
        if (it != g_live.end())
            for (const Shown &s : it->second.items)
                if (s.id == static_cast<int>(R32(c + kCtlId)))
                    for (const ItemReg &r : g_items) if (r.handle == s.handle) j["item_of"] = r.mod ? ModIdOf(r.mod) : "bridge";
        out["controls"].push_back(j);
    }
    return out;
}

} // namespace

// ---- what menus10.cpp shares (menus_shared.h)
const char *MenuIntern(const std::string &s) { return Intern(s); }
uint32_t MenuTextIds(const std::vector<std::string> &strings) { return TextIds(strings); }
bool MenuTextsHook() {
    static bool tried;
    if (tried || !game::g_exe) return g_textsOk;
    tried = true;
    const game::ExeBuild &e = game::Exe();
    if (!e.textGet || !e.textGetOrNull) return false;
    std::string err;
    g_textsOk = HookAdd(nullptr, e.textGet, reinterpret_cast<void *>(&hkGetText), reinterpret_cast<void **>(&oGetText), &err) &&
                HookAdd(nullptr, e.textGetOrNull, reinterpret_cast<void *>(&hkGetTextOrNull), reinterpret_cast<void **>(&oGetTextOrNull), &err);
    if (!g_textsOk) HWARN("menus: cannot hook the text database (no options for switches and selectors): %s", err.c_str());
    return g_textsOk;
}
// A text of the loader's own in the game's text database (UTF-8 in, the game's ANSI kept): its id, 0 when the
// database is not hooked. The same text again: the same id (interact.cpp's labels, the help lines).
uint32_t GameTextId(const std::string &utf8) {
    if (!MenuTextsHook()) return 0;
    return TextIds({Ansi(utf8)});
}
// The game's own text of an id (its database's, not the loader's), or null.
const char *GameTextOwn(uint32_t id) {
    if (!MenuTextsHook()) return nullptr;
    const char *t = nullptr;
    __try { t = oGetTextOrNull(game::Exe().textDb, nullptr, id); } __except (EXCEPTION_EXECUTE_HANDLER) { t = nullptr; }
    return t;
}
bool GameTextsReady() { return MenuTextsHook(); }
// One of the game's own ids answered with this text from now on (UTF-8; "" gives it back to the game).
bool GameTextSet(uint32_t id, const std::string &utf8) {
    if (!MenuTextsHook()) return false;
    if (utf8.empty()) g_setTexts.erase(id);
    else g_setTexts[id] = Intern(Ansi(utf8));
    return true;
}
std::string GameAnsi(const std::string &utf8) { return Ansi(utf8); }
std::string GameUtf8(const std::string &ansi) { return Utf8(ansi.c_str()); }
std::string MenuUtf8(const char *ansi) { return Utf8(ansi); }
std::string MenuAnsi(const std::string &utf8) { return Ansi(utf8); }
std::vector<std::string> MenuSplitOptions(const std::string &text) { return SplitOptions(text); }
void MenuDeliver(MmlMod *mod, MmlMenuFn fn, void *user, int id, const std::string &what) { Deliver(mod, fn, user, id, what); }
std::vector<MenuItemInfo> MenuItemsFor(const std::string &menu) {
    std::vector<MenuItemInfo> out;
    for (const ItemReg &r : g_items)
        if (r.menu == menu && (!r.mod || OwnerAlive(r.mod))) out.push_back({r.handle, r.mod, r.menu, r.after, r.text, r.fn, r.user});
    return out;
}
bool MenuItemByHandle(int handle, MenuItemInfo *out) {
    for (const ItemReg &r : g_items)
        if (r.handle == handle) { *out = {r.handle, r.mod, r.menu, r.after, r.text, r.fn, r.user}; return true; }
    return false;
}

void MenusInit() {
    if (game::g_exe && game::Exe().m10Tick) { m10::Init(); return; }   // 1.0's screens (menus10.cpp)
    if (!game::g_exe || !game::Exe().menuCreate) return;
    const game::ExeBuild &e = game::Exe();
    std::string err;
    if (!HookAdd(nullptr, e.menuCreate, reinterpret_cast<void *>(&hkMenuCreate), reinterpret_cast<void **>(&oMenuCreate), &err) ||
        !HookAdd(nullptr, e.menuDestroy, reinterpret_cast<void *>(&hkMenuDestroy), reinterpret_cast<void **>(&oMenuDestroy), &err)) {
        HWARN("menus: cannot hook the menus' making: %s", err.c_str());
        return;
    }
    // the pages' class: GM_Menu's own vtable with the definition's name, OnCreate and the command handler of ours
    g_pageTable = new VTable{};
    if (!MemRead(e.menuBaseVt - 4, &g_pageTable->rtti, 4 + 4 * kSlots)) { HWARN("menus: GM_Menu's vtable cannot be read"); return; }
    g_pageTable->orig = e.menuBaseVt;
    g_pageTable->slot[kSlotName] = reinterpret_cast<uintptr_t>(&PageDefName);
    g_pageTable->slot[kSlotOnCreate] = reinterpret_cast<uintptr_t>(&PageOnCreate);
    g_pageTable->slot[kSlotCommand] = reinterpret_cast<uintptr_t>(&MenuCommand);
    g_tables.push_back(g_pageTable);
    g_ok = true;
    MenuTextsHook();
}

void HideControl(uintptr_t menu, int id) {
    for (uintptr_t c : Controls(menu))
        if (static_cast<int>(R32(c + kCtlId)) == id) W32(c + kCtlFlags, R32(c + kCtlFlags) & ~0x80000030u);   // not shown, not focused
}

void MenusFrame() {
    if (m10::Known()) { m10::Frame(); return; }
    if (!g_ok) return;
    for (const Orphan &o : g_orphans)
        if (g_live.count(o.menu)) HideControl(o.menu, o.id);
    g_orphans.clear();
    for (size_t i = 0; i < g_pages.size(); i++) {
        Page *p = g_pages[i].get();
        if (!p->waiting || p->menu) continue;
        if (R32(game::Exe().menuTop)) {
            if (!PushNow(p) && !p->menu) g_pages.erase(g_pages.begin() + i);
            return;
        }
        if (GetTickCount() - p->askedAt > 2000) {
            HWARN("menu: no menu opened for the page %s: dropped", p->def.c_str());
            g_pages.erase(g_pages.begin() + i);
            return;
        }
    }
}

int MenuItemAdd(MmlMod *mod, const char *menu, int afterId, const char *text, MmlMenuFn fn, void *user) {
    if (!menu || !*menu || !text || !*text || (mod && (!OwnerAlive(mod) || !fn))) return 0;
    std::string name = Lower(menu);
    if (mod)
        for (ItemReg &r : g_items)
            if (r.mod == mod && r.menu == name && r.fn == fn && r.user == user) {   // asked again: the text and the place
                r.after = afterId;
                r.text = Intern(text);
                return r.handle;
            }
    g_items.push_back({g_nextHandle++, mod, name, afterId, Intern(text), fn, user});
    int handle = g_items.back().handle;
    if (m10::Known()) m10::ItemAsked(handle);
    for (size_t i = 0; mod && i < g_orphans.size();) {   // its own item, still in an open menu, from before its reload
        const Orphan &o = g_orphans[i];
        auto live = g_live.find(o.menu);
        if (o.mod == ModIdOf(mod) && o.menuName == name && o.after == afterId && o.text == text && live != g_live.end()) {
            live->second.items.push_back({o.id, handle});
            g_orphans.erase(g_orphans.begin() + i);
        } else {
            i++;
        }
    }
    return handle;
}

int MenuPageOpen(MmlMod *mod, const char *name, const MmlMenuControl *controls, int count, MmlMenuFn fn, void *user) {
    if (!OwnerAlive(mod) || !fn) return 0;
    std::string err;
    if (m10::Known() ? m10::PageOpen(mod, false, name, controls, count, fn, user, &err) != 0 : OpenPage(mod, false, name, controls, count, fn, user, &err) != 0)
        return 1;
    Log(MML_LOG_WARN, ModIdOf(mod), "menu_open %s: %s", name ? name : "", err.c_str());
    return 0;
}

void MenuPageClose(MmlMod *mod) {
    if (m10::Known()) { if (mod) m10::PageClose(mod, false); return; }
    if (Page *p = mod ? PageOf(mod, false) : nullptr) ClosePage(p);
}

namespace {
// A page's text or value: on its control when the game has made it, and kept for when it makes it (a page waits
// for the pause menu first; it is made again by nothing else).
int PageText(Page *p, int id, const char *text) {
    if (!p || !text) return 0;
    const char *t = Intern(text);
    bool known = false;
    for (size_t i = 0; i < p->ids.size(); i++)
        if (p->ids[i] == id) { p->texts[i] = t; known = true; }
    if (!known) return 0;
    if (!p->menu) return 1;
    for (uintptr_t c : Controls(p->menu))
        if (static_cast<int>(R32(c + kCtlId)) == id) return (TagOf(c) == kTagPicture ? SetPicture(c, t) : SetText(c, t)) ? 1 : 0;
    return 0;
}
int PageValue(Page *p, int id, float value) {
    if (!p || !(value == value) || value < -1e9f || value > 1e9f) return 0;
    bool known = false;
    for (size_t i = 0; i < p->ids.size(); i++)
        if (p->ids[i] == id && HasValue(p->tags[i])) known = true;
    if (!known) return 0;
    p->values[id] = value;
    if (!p->menu) return 1;
    for (uintptr_t c : Controls(p->menu))
        if (static_cast<int>(R32(c + kCtlId)) == id) return SetValue(c, value) ? 1 : 0;
    return 0;
}
int PageRange(Page *p, int id, int lo, int hi) {
    if (!p || lo > hi) return 0;
    bool known = false;
    for (size_t i = 0; i < p->ids.size(); i++)
        if (p->ids[i] == id && HasRange(p->tags[i])) known = true;
    if (!known) return 0;
    p->ranges[id] = {lo, hi};
    if (!p->menu) return 1;
    for (uintptr_t c : Controls(p->menu))
        if (static_cast<int>(R32(c + kCtlId)) == id) return SetRange(c, lo, hi) ? 1 : 0;
    return 0;
}
int PageGetText(Page *p, int id, std::string *text) {
    if (!p || !p->menu) return 0;
    for (uintptr_t c : Controls(p->menu))
        if (static_cast<int>(R32(c + kCtlId)) == id) { *text = ControlText(c); return 1; }
    return 0;
}
int PageGetValue(Page *p, int id, float *value) {
    if (!p || !value) return 0;
    if (p->menu)
        for (uintptr_t c : Controls(p->menu)) {
            if (static_cast<int>(R32(c + kCtlId)) != id) continue;
            return GetValue(c, value) ? 1 : 0;
        }
    auto v = p->values.find(id);
    if (v == p->values.end()) return 0;
    *value = v->second;
    return 1;
}
} // namespace

int MenuPageText(MmlMod *mod, int id, const char *text) {
    if (m10::Known()) return mod ? m10::PageText(mod, false, id, text) : 0;
    return mod ? PageText(PageOf(mod, false), id, text) : 0;
}
int MenuPageValue(MmlMod *mod, int id, float value) {
    if (m10::Known()) return mod ? m10::PageValue(mod, false, id, value) : 0;
    return mod ? PageValue(PageOf(mod, false), id, value) : 0;
}
int MenuPageGetValue(MmlMod *mod, int id, float *value) {
    if (m10::Known()) return mod && value ? m10::PageGetValue(mod, false, id, value) : 0;
    return mod ? PageGetValue(PageOf(mod, false), id, value) : 0;
}
int MenuPageRange(MmlMod *mod, int id, int lo, int hi) {
    if (m10::Known()) return mod ? m10::PageRange(mod, false, id, lo, hi) : 0;
    return mod ? PageRange(PageOf(mod, false), id, lo, hi) : 0;
}
int MenuPageGetText(MmlMod *mod, int id, char *buf, int size) {
    std::string text;
    if (!mod || !buf || size < 1) return 0;
    if (m10::Known() ? !m10::PageGetText(mod, false, id, &text) : !PageGetText(PageOf(mod, false), id, &text)) return 0;
    snprintf(buf, static_cast<size_t>(size), "%s", text.c_str());
    return 1;
}

// A mod unloads: its items leave the menus that show them (hidden at the next frame, unless its reload asks for the
// same item: the game owns the controls), its page closes.
void MenusRemove(MmlMod *mod) {
    if (m10::Known()) m10::ModRemoved(mod);
    for (size_t i = 0; i < g_items.size();) {
        if (g_items[i].mod != mod) { i++; continue; }
        for (auto &kv : g_live)
            for (size_t k = 0; k < kv.second.items.size();) {
                if (kv.second.items[k].handle != g_items[i].handle) { k++; continue; }
                g_orphans.push_back({kv.first, kv.second.items[k].id, ModIdOf(mod), g_items[i].menu, g_items[i].after, g_items[i].text});
                kv.second.items.erase(kv.second.items.begin() + k);
            }
        g_items.erase(g_items.begin() + i);
    }
    if (Page *p = PageOf(mod, false)) {
        p->fn = nullptr;
        ClosePage(p);
        for (auto &q : g_pages) if (q->mod == mod && !q->bridge) q->mod = nullptr;   // (it lives until the game deletes its menu)
    }
}

// A control's value as its kind keeps it (a progress bar has no getter of its own: the menu's GetValue answers 0 for
// it; a check, a switch, a number, a selector are read from their own fields). False: another kind, or no such control.
bool MenuControlValue(uintptr_t menu, int id, float *value) {
    if (m10::Known()) return m10::ControlValue(menu, id, value);
    for (uintptr_t c : Controls(menu))
        if (static_cast<int>(R32(c + kCtlId)) == id) return GetValue(c, value);
    return false;
}

std::string MenuDefName(uintptr_t menu) { return m10::Known() ? m10::Name(menu) : DefNameOf(menu); }

// The game's texts are bytes its fonts map through its language's table (tables\lang_<ll>.def, read into a record of
// 0x238 bytes whose +4 is the language's two letters: menuLang). The text database of that language
// (tables\textdb_<ll>.def) is written in that language's Windows code page: Czech in 1250 (the game's own), the
// western ones in 1252 (seen in the French 1.0 lab: 0xE8 is the "è" of "Piège", which 1250 read as "č").
unsigned GameCodePage() {
    uint16_t lang = 0;
    if (game::g_exe && game::Exe().menuLang) MemRead(game::Exe().menuLang, &lang, 2);
    char ll[3] = {static_cast<char>(lang & 0xFF), static_cast<char>(lang >> 8), 0};
    for (char &c : ll) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (!strcmp(ll, "cz") || !strcmp(ll, "cs") || !strcmp(ll, "pl") || !strcmp(ll, "hu") || !strcmp(ll, "sk")) return 1250;
    if (!strcmp(ll, "ru")) return 1251;
    return 1252;
}

uintptr_t MenuClassOf(uintptr_t menu) {
    uintptr_t vptr = R32(menu);
    VTable *t = Ours(vptr);
    return t ? t->orig : vptr;
}

std::string MenuDescribe(uintptr_t menu) {
    if (m10::Known()) return m10::Describe(menu);
    return Described(menu).dump(-1, ' ', false, json::error_handler_t::replace);
}

bool MenusMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "menu.item" && method != "menu.items" && method != "menu.item.remove" && method != "menu.page" && method != "menu.page.set" &&
        method != "menu.page.close" && method != "menu.def" && method != "menu.click")
        return false;
    if (m10::Known() && m10::Method(method, paramsJson, reply, ok)) return true;
    *ok = false;
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "menu.click") {
            if (!g_ok) { *reply = "the menus are not known for this build"; return true; }
            // a control of an open menu (the active one unless `menu` names another) clicked in its middle by the
            // injected mouse: the game's own hit test, focus and click handling
            uintptr_t found = 0, top = R32(game::Exe().menuTop);
            std::string want = Lower(p.value("menu", ""));
            int id = p.value("id", 0);
            for (uintptr_t m = top; m && !found; m = R32(m + kMenuUnder)) {
                if (!want.empty() && DefNameOf(m) != want) continue;
                for (uintptr_t c : Controls(m)) if (static_cast<int>(R32(c + kCtlId)) == id) { found = c; break; }
                if (!want.empty() || found) break;
            }
            if (!found) { *reply = "id: a control of the open menu (menu.state lists them); menu: another open menu's name"; return true; }
            float sx = game::Exe().menuScale ? RF(game::Exe().menuScale) : 1, sy = game::Exe().menuScale ? RF(game::Exe().menuScale + 4) : 1;
            if (!(sx > 0.01f) || !(sy > 0.01f)) { *reply = "the menus' scale is not known"; return true; }
            float x = (RF(found + kCtlX) + RF(found + kCtlW) / 2) / sx, y = (RF(found + kCtlY) + RF(found + kCtlH) / 2) / sy;
            std::string err;
            if (!InputClick(x, y, &err)) { *reply = err; return true; }
            *reply = dump({{"id", id}, {"at", {std::round(x * 10) / 10, std::round(y * 10) / 10}},
                           {"note", "the cursor moved there and the left button pressed 80 ms later, for 100 ms"}});
        } else if (method == "menu.def") {
            std::string name = Lower(p.value("name", ""));
            if (name.empty() || name.find_first_of("\\/.:") != std::string::npos) {
                *reply = "name: a menu's definition name (\"gamemenu\", \"mainmenu\", \"optionsa\", \"loadgame\", \"messagebox\"...)";
                return true;
            }
            std::string err;
            json d = Definition(name, &err);
            if (d.is_null()) { *reply = err; return true; }
            *reply = dump(d);
        } else if (method == "menu.items") {
            json items = json::array(), clicks = json::array(), pages = json::array();
            for (const ItemReg &r : g_items)
                items.push_back({{"handle", r.handle}, {"owner", r.mod ? ModIdOf(r.mod) : "bridge"}, {"menu", r.menu}, {"after", r.after}, {"text", Utf8(r.text)}});
            DWORD now = GetTickCount();
            for (const Click &c : g_clicks) clicks.push_back({{"id", c.id}, {"what", Utf8(c.what.c_str())}, {"ms_ago", now - c.at}});
            for (auto &pg : g_pages) pages.push_back({{"name", pg->def}, {"owner", pg->bridge ? "bridge" : ModIdOf(pg->mod)}, {"open", pg->menu != 0}});
            if (m10::Known())
                for (const json &pg : json::parse(m10::PagesJson())) pages.push_back(pg);
            *reply = dump({{"known", g_ok || m10::Known()}, {"items", items}, {"pages", pages}, {"bridge_clicks", clicks}});
        } else if (method == "menu.item") {
            std::string menu = p.value("menu", ""), text = p.value("text", "");
            if (menu.empty() || text.empty() || !p.contains("after") || !p["after"].is_number_integer()) {
                *reply = "menu: a menu's definition name (\"gamemenu\": the pause menu; menu.state shows the open ones); after: the id of the control the item goes under; text";
                return true;
            }
            int handle = MenuItemAdd(nullptr, menu.c_str(), p["after"].get<int>(), Ansi(text).c_str(), nullptr, nullptr);
            json out = {{"handle", handle}, {"menu", Lower(menu)}, {"after", p["after"]}, {"text", text},
                        {"note", "in the menu from its next opening; a click is logged and listed by menu.items"}};
            if (!g_ok && !m10::Known()) out["note"] = "the menus are not known for this build: the item is kept, it will not be shown";
            *reply = dump(out);
        } else if (method == "menu.item.remove") {
            int handle = p.value("handle", 0);
            size_t before = g_items.size();
            for (size_t i = 0; i < g_items.size();)
                if (g_items[i].handle == handle && !g_items[i].mod) g_items.erase(g_items.begin() + i);
                else i++;
            if (before == g_items.size()) { *reply = "handle: one of the bridge's items (menu.items)"; return true; }
            *reply = dump({{"removed", handle}, {"note", "gone from the menu at its next opening"}});
        } else if (method == "menu.page") {
            if (!p.contains("controls") || !p["controls"].is_array() || p["controls"].empty()) {
                *reply = "name; controls: [{id, kind (\"wind\" first; \"wins\", \"stxt\", \"item\", \"butt\", \"line\", \"slid\", \"pbar\", \"chck\", \"bslb\", \"bsel\", \"iten\", \"edit\", \"end\"...), x, y, w, h, text (options: \"A|B|C\"), flags, value, range}] in 800x600 units";
                return true;
            }
            std::vector<MmlMenuControl> controls;
            std::vector<std::string> kinds, texts;
            kinds.reserve(p["controls"].size());
            texts.reserve(p["controls"].size());
            for (const json &c : p["controls"]) {
                if (!c.is_object()) { *reply = "controls: objects"; return true; }
                kinds.push_back(c.value("kind", ""));
                texts.push_back(Ansi(c.value("text", "")));
                uint32_t flags = 0;
                if (c.contains("flags")) flags = c["flags"].is_string() ? static_cast<uint32_t>(strtoul(c["flags"].get<std::string>().c_str(), nullptr, 0)) : c["flags"].get<uint32_t>();
                controls.push_back({c.value("id", 0), kinds.back().c_str(), c.value("x", 0.0f), c.value("y", 0.0f), c.value("w", 0.0f), c.value("h", 0.0f),
                                    texts.back().c_str(), flags});
            }
            std::string err;
            if (!OpenPage(nullptr, true, p.value("name", "page").c_str(), controls.data(), static_cast<int>(controls.size()), nullptr, nullptr, &err)) { *reply = err; return true; }
            Page *pg = PageOf(nullptr, true);
            for (const json &c : p["controls"]) {
                const json &r = c.contains("range") ? c["range"] : json();
                if (r.is_array() && r.size() == 2 && !PageRange(pg, c.value("id", 0), r[0].get<int>(), r[1].get<int>()))
                    HWARN("menu: the bridge's page: control %d takes no range (iten, edin, sldi do)", c.value("id", 0));
                if (c.contains("value") && c["value"].is_number() && !PageValue(pg, c.value("id", 0), c["value"].get<float>()))
                    HWARN("menu: the bridge's page: control %d takes no value", c.value("id", 0));
            }
            *reply = dump({{"name", pg ? pg->def : ""}, {"open", pg && pg->menu != 0},
                           {"note", pg && pg->menu ? "over the open menu; clicks are logged and listed by menu.items; Esc or menu.page.close closes it"
                                                   : "no menu was open: the pause menu is being opened, the page goes over it"}});
        } else if (method == "menu.page.set") {
            Page *pg = PageOf(nullptr, true);
            if (!pg) { *reply = "the bridge has no page open"; return true; }
            int id = p.value("id", 0);
            json out = {{"id", id}};
            if (p.contains("text")) out["text"] = PageText(pg, id, Ansi(p.value("text", "")).c_str()) != 0;
            if (p.contains("range") && p["range"].is_array() && p["range"].size() == 2)
                out["range"] = PageRange(pg, id, p["range"][0].get<int>(), p["range"][1].get<int>()) != 0;
            if (p.contains("value") && p["value"].is_number()) out["value"] = PageValue(pg, id, p["value"].get<float>()) != 0;
            float v = 0;
            if (PageGetValue(pg, id, &v)) out["now"] = v;
            std::string text;
            if (PageGetText(pg, id, &text)) out["now_text"] = Utf8(text.c_str());
            if (!p.contains("text") && !p.contains("value") && !p.contains("range") && !out.contains("now_text")) {
                *reply = "id; text, value, range [min, max] (or none: read them)";
                return true;
            }
            *reply = dump(out);
        } else {
            Page *pg = PageOf(nullptr, true);
            if (!pg) { *reply = "the bridge has no page open"; return true; }
            ClosePage(pg);
            *reply = dump({{"closed", true}});
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
