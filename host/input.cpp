// Input from inside the game: actions (what the controls give the game) and keys (what the keyboard gives LS3D).
//
// Actions. The game reads its controls from one object (game_build.h `controls`): a list of binding slots, one per
// action and binding (the controls menu's rows: Forwards primary, Forwards secondary, ...), and two float arrays
// parallel to it, "held" and "pressed this frame", which controlsUpdate fills from the devices once a frame, before
// the mission's tick reads them. After that update the loader writes the values it was asked for: an action is
// pressed whatever its bindings, even one bound to nothing. When an action is let go, its slots are written 0 once
// (an unbound slot is never written by the game, so the value would stay).
//
// Keys, the fallback for whatever does not go through the actions (menus, cheats, text): LS3D reads the keyboard
// once a frame into a 256-byte DirectInput state (`keyboardUpdate`, `keyboardState`); after that read the loader sets
// 0x80 on the keys it was asked to hold, so every reader of the keyboard sees them pressed.
//
// The mouse, for the menus: LS3D reads it once a frame into a DirectInput state (`mouseUpdate`, `mouseState`:
// moves since the read before, the wheel, the buttons); the menus' input tick adds the moves to its cursor in pixels
// and gives the buttons to the control under it. After that read the loader writes what it was asked for: a move to
// a point (the difference to the menus' cursor, so the cursor lands there exactly, and the control under it gets
// the focus as under a real mouse), buttons held, wheel notches. A move is made only while a menu is open (outside
// one the mouse turns the player).
//
// The user's real keyboard and mouse are never touched: everything is injected in the game's own memory.
//
// A button's value is 1. A mouse axis' ("Aim X", "Aim Y": right and down are positive) is what the game makes of the
// mouse: its counts in that update / 400. On foot the player turns by sensitivity * 5 * value radians an update, so
// values are small (0.01: about 1.4 degrees at the lab's sensitivity); "turn" does that sum for an angle.
//
// Bridge methods (game thread): input.actions, input.act {action, ms|frames, value, after_ms|after_frames} or
// {turn: degrees, frames}, input.key {key, ms|frames, after_ms|after_frames}, input.seq {steps: [the same, each with
// "action", "key" or "turn"]} (all of them on one clock, or none when one is wrong), input.release {what:
// "all"|action|key}, input.state; input.mouse {mouse: [x, y] | button: "left"|"right"|"middle" | wheel: notches,
// ms|frames, after_ms|after_frames} (also steps of input.seq); input.text {text, every_ms} (typed as keys: the text
// fields of the menus take characters from LS3D's own translation of the keys it read); text.get {id}, text.find
// {text, from, to}.
#include "host.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include "json.hpp"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

struct Hold {
    bool key = false;          // a key (DIK code in `slot`) or an action slot
    int mouse = 0;             // or the mouse: kMove (to mx, my), kButton (`slot` 0 left, 1 right, 2 middle), kWheel (value)
    float mx = 0, my = 0;      // the menus' 800x600 units
    int slot = 0;
    float value = 1.0f;
    float turn = 0;            // degrees, when it was asked for as a turn (the value an update is worked out of it)
    int afterFrames = 0;      // the delay before it starts: updates, or milliseconds from `asked`
    DWORD afterMs = 0;
    int frames = 0;            // how long: updates left, or milliseconds from `began` (when ms is not 0)
    DWORD ms = 0;
    DWORD asked = 0, began = 0;   // GetTickCount
    bool started = false;
    bool edgeSent = false;     // "pressed this frame" given once
    std::string name;
};
enum { kMove = 1, kButton = 2, kWheel = 3 };
std::vector<Hold> g_holds;
std::vector<int> g_clearSlots;   // action slots to write 0 once (let go)
bool g_actionsOk, g_keysOk, g_mouseOk;

using UpdateFn = void(__fastcall *)(uintptr_t controls, void *edx, int flag);   // thiscall, ret 4
UpdateFn g_update;
using KeyboardFn = int(__stdcall *)(void *igraph);
KeyboardFn g_keyboard, g_mouse;

uint32_t Rd(uintptr_t a) {
    uint32_t v = 0;
    MemRead(a, &v, 4);
    return v;
}

// One update of a hold: false while it still waits for its start.
bool Begun(Hold &h) {
    if (h.started) return true;
    if (h.afterMs ? GetTickCount() - h.asked < h.afterMs : h.afterFrames-- > 0) return false;
    h.started = true;
    h.began = GetTickCount();
    return true;
}

bool Expired(const Hold &h) { return h.ms ? GetTickCount() - h.began >= h.ms : h.frames <= 0; }

int SlotCount() {
    uintptr_t c = game::Exe().controls;
    uint32_t b = Rd(c + game::Exe().controlsBind), e = Rd(c + game::Exe().controlsBind + 4);
    return b && e > b ? static_cast<int>((e - b) / 12) : 0;
}

void WriteSlot(int slot, float held, float pressed) {
    uintptr_t c = game::Exe().controls;
    if (slot < 0 || slot >= SlotCount()) return;
    MemWrite(Rd(c + game::Exe().controlsHeld) + 4 * slot, &held, 4);
    MemWrite(Rd(c + game::Exe().controlsPressed) + 4 * slot, &pressed, 4);
}

void __fastcall UpdateHook(uintptr_t controls, void *edx, int flag) {
    g_update(controls, edx, flag);
    for (int s : g_clearSlots) WriteSlot(s, 0.0f, 0.0f);
    g_clearSlots.clear();
    for (size_t i = 0; i < g_holds.size();) {
        Hold &h = g_holds[i];
        if (h.key || h.mouse || !Begun(h)) { i++; continue; }
        if (Expired(h)) {
            g_clearSlots.push_back(h.slot);
            g_holds.erase(g_holds.begin() + i);
            continue;
        }
        WriteSlot(h.slot, h.value, h.edgeSent ? 0.0f : 1.0f);
        h.edgeSent = true;
        h.frames--;
        i++;
    }
}

// ---- the keyboard as mods see it (mml.h key_down, key_pressed, hotkey): the state of the last read, the injected
// keys in it, and its edges against the read before (kept here: one read, one "pressed").
uint8_t g_keyNow[256], g_keyEdge[256];
struct Hotkey {
    MmlMod *mod;
    int key;                    // the DirectInput code of the key that fires it
    bool ctrl, shift, alt;      // exactly these modifiers held (either side)
    MmlHotkeyFn fn;
    void *user;
    std::string combo;
    bool pending;               // went down at the last read: called at the next loader frame
};
std::vector<Hotkey> g_hotkeys;
enum { kDikLCtrl = 0x1D, kDikRCtrl = 0x9D, kDikLShift = 0x2A, kDikRShift = 0x36, kDikLAlt = 0x38, kDikRAlt = 0xB8 };

// The keys held for agents, set in a keyboard state.
void ApplyKeyHolds(uint8_t *state) {
    for (size_t i = 0; i < g_holds.size();) {
        Hold &h = g_holds[i];
        if (!h.key || !Begun(h)) { i++; continue; }
        if (Expired(h)) { g_holds.erase(g_holds.begin() + i); continue; }
        state[h.slot & 0xFF] |= 0x80;
        h.frames--;
        i++;
    }
}

// One read of the keyboard (the game's, with the injected keys in it): the state and its edges for mods.
void KeysRead(const uint8_t *state) {
    for (int i = 0; i < 256; i++) {
        uint8_t now = state[i] & 0x80 ? 1 : 0;
        g_keyEdge[i] = now && !g_keyNow[i];
        g_keyNow[i] = now;
    }
    bool ctrl = g_keyNow[kDikLCtrl] || g_keyNow[kDikRCtrl], shift = g_keyNow[kDikLShift] || g_keyNow[kDikRShift],
         alt = g_keyNow[kDikLAlt] || g_keyNow[kDikRAlt];
    for (Hotkey &h : g_hotkeys)
        if (g_keyEdge[h.key] && h.ctrl == ctrl && h.shift == shift && h.alt == alt) h.pending = true;
}

int __stdcall KeyboardHook(void *igraph) {
    int r = g_keyboard(igraph);
    uint8_t *state = reinterpret_cast<uint8_t *>(ModuleBase("LS3DF.dll") + game::Ls3d().keyboardState);
    ApplyKeyHolds(state);
    KeysRead(state);
    return r;
}

// ---- the mouse
// The cursor: 1.2's two ints (menuCursor), 1.0's two floats (m10Cursor); both pixels, moved by the mouse's moves.
bool MenusMouseKnown() {
    return game::g_exe && (game::Exe().menuCursor || game::Exe().m10Cursor) && game::Exe().menuScale && game::Exe().menuTop;
}
struct Cursor { bool menu; int px, py; float sx, sy; };
Cursor MenuCursor() {
    Cursor c{false, 0, 0, 1, 1};
    if (!MenusMouseKnown()) return c;
    c.menu = Rd(game::Exe().menuTop) != 0;
    if (game::Exe().menuCursor) {
        MemRead(game::Exe().menuCursor, &c.px, 4);
        MemRead(game::Exe().menuCursor + 4, &c.py, 4);
    } else {
        float x = 0, y = 0;
        MemRead(game::Exe().m10Cursor, &x, 4);
        MemRead(game::Exe().m10Cursor + 4, &y, 4);
        c.px = static_cast<int>(lroundf(x));
        c.py = static_cast<int>(lroundf(y));
    }
    MemRead(game::Exe().menuScale, &c.sx, 4);
    MemRead(game::Exe().menuScale + 4, &c.sy, 4);
    if (!(c.sx > 0.01f)) c.sx = 1;
    if (!(c.sy > 0.01f)) c.sy = 1;
    return c;
}

void ApplyMouseHolds(int32_t *axes, uint8_t *buttons) {
    for (size_t i = 0; i < g_holds.size();) {
        Hold &h = g_holds[i];
        if (!h.mouse || !Begun(h)) { i++; continue; }
        if (h.mouse == kMove) {
            Cursor c = MenuCursor();
            if (!c.menu) {   // waits for a menu (its time: how long it may wait)
                if (Expired(h)) {
                    HWARN("input: no menu open: the cursor was not moved to (%g, %g)", h.mx, h.my);
                    g_holds.erase(g_holds.begin() + i);
                    continue;
                }
                h.frames--;
                i++;
                continue;
            }
            axes[0] = static_cast<int32_t>(lroundf(h.mx * c.sx)) - c.px;
            axes[1] = static_cast<int32_t>(lroundf(h.my * c.sy)) - c.py;
            g_holds.erase(g_holds.begin() + i);
            continue;
        }
        if (h.mouse == kWheel) {
            axes[2] += static_cast<int32_t>(lroundf(h.value * 120.0f));
            g_holds.erase(g_holds.begin() + i);
            continue;
        }
        if (Expired(h)) { g_holds.erase(g_holds.begin() + i); continue; }
        buttons[h.slot & 3] |= 0x80;
        h.frames--;
        i++;
    }
}

int __stdcall MouseHook(void *igraph) {
    int r = g_mouse(igraph);
    uint8_t *state = reinterpret_cast<uint8_t *>(ModuleBase("LS3DF.dll") + game::Ls3d().mouseState);
    ApplyMouseHolds(reinterpret_cast<int32_t *>(state), state + 12);
    return r;
}

// DirectInput key codes by name (the ones agents will want; any other by its number).
struct KeyName { const char *name; int dik; };
const KeyName kKeys[] = {
    {"Esc", 0x01}, {"1", 0x02}, {"2", 0x03}, {"3", 0x04}, {"4", 0x05}, {"5", 0x06}, {"6", 0x07}, {"7", 0x08},
    {"8", 0x09}, {"9", 0x0A}, {"0", 0x0B}, {"Minus", 0x0C}, {"Equals", 0x0D}, {"Backspace", 0x0E}, {"Tab", 0x0F},
    {"Q", 0x10}, {"W", 0x11}, {"E", 0x12}, {"R", 0x13}, {"T", 0x14}, {"Y", 0x15}, {"U", 0x16}, {"I", 0x17},
    {"O", 0x18}, {"P", 0x19}, {"Enter", 0x1C}, {"Ctrl", 0x1D}, {"A", 0x1E}, {"S", 0x1F}, {"D", 0x20}, {"F", 0x21},
    {"G", 0x22}, {"H", 0x23}, {"J", 0x24}, {"K", 0x25}, {"L", 0x26}, {"Shift", 0x2A}, {"Z", 0x2C}, {"X", 0x2D},
    {"C", 0x2E}, {"V", 0x2F}, {"B", 0x30}, {"N", 0x31}, {"M", 0x32}, {"RightShift", 0x36}, {"Alt", 0x38},
    {"Space", 0x39}, {"CapsLock", 0x3A}, {"F1", 0x3B}, {"F2", 0x3C}, {"F3", 0x3D}, {"F4", 0x3E}, {"F5", 0x3F},
    {"F6", 0x40}, {"F7", 0x41}, {"F8", 0x42}, {"F9", 0x43}, {"F10", 0x44}, {"F11", 0x57}, {"F12", 0x58},
    {"NumEnter", 0x9C}, {"RightCtrl", 0x9D}, {"RightAlt", 0xB8}, {"Home", 0xC7}, {"Up", 0xC8}, {"PageUp", 0xC9},
    {"Left", 0xCB}, {"Right", 0xCD}, {"End", 0xCF}, {"Down", 0xD0}, {"PageDown", 0xD1}, {"Insert", 0xD2},
    {"Delete", 0xD3},
    {"LeftBracket", 0x1A}, {"RightBracket", 0x1B}, {"Semicolon", 0x27}, {"Apostrophe", 0x28}, {"Grave", 0x29},
    {"Backslash", 0x2B}, {"Comma", 0x33}, {"Period", 0x34}, {"Slash", 0x35}, {"NumMultiply", 0x37},
    {"Num7", 0x47}, {"Num8", 0x48}, {"Num9", 0x49}, {"NumMinus", 0x4A}, {"Num4", 0x4B}, {"Num5", 0x4C},
    {"Num6", 0x4D}, {"NumPlus", 0x4E}, {"Num1", 0x4F}, {"Num2", 0x50}, {"Num3", 0x51}, {"Num0", 0x52},
    {"NumPeriod", 0x53}, {"NumDivide", 0xB5}, {"Pause", 0xC5},
};

// A character as a key of the US layout and whether Shift is held for it (0: not on the keyboard).
int CharKey(char ch, bool *shift) {
    static const char *kPlain = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./ ";
    static const char *kShifted = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>? ";
    static const uint8_t kDik[] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
                                   0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
                                   0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x39};
    for (size_t i = 0; kPlain[i]; i++) {
        if (kPlain[i] == ch) { *shift = false; return kDik[i]; }
        if (kShifted[i] == ch && ch != ' ') { *shift = true; return kDik[i]; }
    }
    return 0;
}

// With no game (the offline tests' stand-in): no keyboard is read, the keys are what `input.key` holds.
bool VirtualKeys() { return !game::g_ls3d || !game::Ls3d().keyboardUpdate; }
bool ActionsKnown() { return game::g_exe && game::Exe().controls; }

// A key's name from its code (ours: the game's own names come from the keyboard layout and may not be ASCII).
std::string DikName(int dik) {
    for (const KeyName &n : kKeys) if (n.dik == dik) return n.name;
    char b[16];
    snprintf(b, sizeof b, "0x%02X", dik);
    return b;
}

int KeyCode(const json &k) {
    if (k.is_number_integer()) return k.get<int>() & 0xFF;
    if (!k.is_string()) return -1;
    std::string s = k.get<std::string>();
    for (const KeyName &n : kKeys) if (!_stricmp(n.name, s.c_str())) return n.dik;
    char *end;
    long v = strtol(s.c_str(), &end, 0);
    return *end || s.empty() ? -1 : static_cast<int>(v & 0xFF);
}

const char *TextOrNull(unsigned id) {
    using GetFn = const char *(__fastcall *)(uintptr_t db, void *edx, unsigned id);
    const game::ExeBuild &e = game::Exe();
    __try { return reinterpret_cast<GetFn>(e.textGetOrNull)(e.textDb, nullptr, id); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// The game's strings are in the system's ANSI code page; the bridge speaks UTF-8.
std::string Utf8(const char *ansi) {
    wchar_t w[512];
    int n = MultiByteToWideChar(CP_ACP, 0, ansi, -1, w, 512);
    if (n <= 0) return "";
    char out[1536];
    int m = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof out, nullptr, nullptr);
    return m > 0 ? std::string(out) : std::string();
}

// A string the game keeps somewhere (read without faulting, at most 511 bytes).
std::string GameString(const char *p) {
    char buf[512] = {};
    if (p) MemRead(reinterpret_cast<uintptr_t>(p), buf, sizeof buf - 1);
    return Utf8(buf);
}

std::string Text(unsigned id) { return GameString(TextOrNull(id)); }

const char *KeyNameOf(int slot) {
    using NameFn = const char *(__fastcall *)(uintptr_t controls, void *edx, uintptr_t binding);
    const game::ExeBuild &e = game::Exe();
    uintptr_t b = Rd(e.controls + e.controlsBind) + 12 * slot;
    __try { return reinterpret_cast<NameFn>(e.controlsKeyName)(e.controls, nullptr, b); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// The actions, in slot order: the controls menu's rows (two slots: the primary and the secondary binding) and the
// ones with one slot. On foot (0..31): the 1.2 menu's rows (the owner's screenshot, 2026-10-13) and the code that
// reads them (C_player::AI 0x5937B0, its on-foot input 0x58F4B0); the menu's one row "Aiming" is two axes here.
// Driving (33..59): the code that reads them (C_player's driving input 0x5917F0: the pairs it tests, what each
// does) under the menu's texts (330..344). Slot 32 is read by none of them. Names are matched without case, spaces
// or punctuation. Slots past the list are named by number.
struct Action { const char *name; int slots; };
const Action kActions[] = {
    {"Forwards", 2}, {"Backwards", 2}, {"Left", 2}, {"Right", 2}, {"Walk/Run", 2}, {"Action", 2}, {"Fire", 2},
    {"Crouch", 2}, {"Jump", 2}, {"Aim X", 1}, {"Aim Y", 1}, {"Cycle Up", 2}, {"Cycle Down", 2}, {"Inventory", 1},
    {"Hide Weapon", 1}, {"Drop Weapon", 1}, {"Toggle Run/Walk", 1}, {"Reload", 1}, {"Sniper Mode", 1},
    {"Objectives", 1}, {"City Map", 1},
    {"", 1},
    {"Accelerate", 2}, {"Brake", 2}, {"Steer Left", 2}, {"Steer Right", 2}, {"Handbrake", 2}, {"Speed Limiter", 2},
    {"Gear Up", 2}, {"Gear Down", 2}, {"Horn", 2}, {"Look Left", 2}, {"Look Right", 2}, {"Clutch", 2},
    {"Manual/Auto Gearing", 1}, {"Change Camera", 1}, {"Replace Car", 1},
};

struct SlotInfo { std::string action; int index; };   // index: 0 primary, 1 secondary
std::vector<SlotInfo> SlotNames(int n) {
    std::vector<SlotInfo> out(n);
    int s = 0;
    for (const Action &a : kActions)
        for (int k = 0; k < a.slots && s < n; k++, s++) out[s] = {*a.name ? a.name : "slot " + std::to_string(s), k};
    for (; s < n; s++) out[s] = {"slot " + std::to_string(s), 0};
    return out;
}

// A name as it is compared: letters and digits only, lower case ("Walk/Run", "walk run", "WalkRun" are one).
std::string Plain(const std::string &s) {
    std::string out;
    for (unsigned char ch : s) if (isalnum(ch)) out += static_cast<char>(tolower(ch));
    return out;
}

// An action by name (its primary slot) or a slot by number; -1 when unknown.
int SlotOf(const json &a, std::string *name) {
    int n = SlotCount();
    if (a.is_number_integer()) {
        int s = a.get<int>();
        if (s < 0 || s >= n) return -1;
        *name = "slot " + std::to_string(s);
        return s;
    }
    if (!a.is_string()) return -1;
    std::string want = Plain(a.get<std::string>());
    std::vector<SlotInfo> names = SlotNames(n);
    for (int s = 0; s < n; s++)
        if (Plain(names[s].action) == want) { *name = names[s].action; return s; }
    return -1;
}

// A hold from a request ({action|key, ms|frames, value, after_ms|after_frames}); false and why when it is wrong.
bool MakeHold(const json &p, DWORD now, Hold *h, std::string *err) {
    try {
        if (p.contains("mouse") || p.contains("button") || p.contains("wheel")) {
            if (!g_mouseOk) { *err = "the mouse's read is not hooked (not known for this build)"; return false; }
            if (p.contains("mouse")) {
                const json &m = p["mouse"];
                if (!MenusMouseKnown()) { *err = "mouse: the menus' cursor is not known for this build"; return false; }
                if (m.is_array() && m.size() == 2 && m[0].is_number() && m[1].is_number()) { h->mx = m[0].get<float>(); h->my = m[1].get<float>(); }
                else if (m.is_object() && m.contains("x") && m.contains("y")) { h->mx = m["x"].get<float>(); h->my = m["y"].get<float>(); }
                else { *err = "mouse: [x, y] in the menus' 800x600 units"; return false; }
                if (!(h->mx >= -100 && h->mx <= 900 && h->my >= -100 && h->my <= 700)) { *err = "mouse: x 0..800, y 0..600"; return false; }
                h->mouse = kMove;
                h->name = "mouse";
                if (!p.contains("ms") && !p.contains("frames")) h->ms = 2000;   // how long it may wait for a menu
            } else if (p.contains("button")) {
                const json &b = p["button"];
                std::string n = b.is_string() ? Plain(b.get<std::string>()) : "";
                h->slot = b.is_number_integer() ? b.get<int>() : n == "left" ? 0 : n == "right" ? 1 : n == "middle" ? 2 : -1;
                if (h->slot < 0 || h->slot > 3) { *err = "button: \"left\", \"right\", \"middle\" (or 0..3)"; return false; }
                h->mouse = kButton;
                h->name = std::string("mouse button ") + (h->slot == 0 ? "left" : h->slot == 1 ? "right" : h->slot == 2 ? "middle" : "4");
            } else {
                h->value = p["wheel"].get<float>();
                if (!(h->value >= -100 && h->value <= 100)) { *err = "wheel: notches, -100..100 (up / away is positive)"; return false; }
                h->mouse = kWheel;
                h->name = "mouse wheel";
            }
        } else if (p.contains("key")) {
            if (!g_keysOk && !VirtualKeys()) { *err = "the keyboard's read is not hooked"; return false; }
            h->key = true;
            if ((h->slot = KeyCode(p["key"])) <= 0) { *err = "key: a name (W, Space, Esc, F1, Up...) or a DIK code"; return false; }
            h->name = p["key"].is_string() ? p["key"].get<std::string>() : DikName(h->slot);
        } else if (p.contains("turn")) {
            // The player on foot, turned by an angle: the "Aim X" axis given, over some updates, the sum the player's
            // code makes that angle of (game_build.h mouseSensitivity). At most 5 degrees an update.
            float sens = 0;
            if (!g_actionsOk || !game::Exe().mouseSensitivity || !MemRead(game::Exe().mouseSensitivity, &sens, 4) || sens < 0.001f) {
                *err = "turn: the mouse sensitivity is not known (or the controls' update is not hooked)";
                return false;
            }
            float deg = p["turn"].get<float>();
            if (!(deg >= -3600 && deg <= 3600)) { *err = "turn: degrees, -3600..3600 (to the right when positive)"; return false; }
            int frames = p.value("frames", 10), least = static_cast<int>(fabsf(deg) / 5.0f) + 1;
            if (frames < least) frames = least;
            if (frames > 100000) { *err = "frames: 1..100000"; return false; }
            h->slot = SlotOf("Aim X", &h->name);
            h->value = deg * 3.14159265f / 180.0f / (5.0f * sens) / static_cast<float>(frames);
            h->frames = frames;
            h->turn = deg;
        } else if (p.contains("action")) {
            if (!g_actionsOk) { *err = ActionsKnown() ? "the controls' update is not hooked" : "the game's actions are not known for this build"; return false; }
            if ((h->slot = SlotOf(p["action"], &h->name)) < 0) {
                *err = "action: a name (input.actions lists them: \"Forwards\", \"Fire\", \"Accelerate\"...) or a slot number";
                return false;
            }
            h->value = p.value("value", 1.0f);
        } else {
            *err = "action, key, turn, mouse, button or wheel: what to do";
            return false;
        }
        if (p.contains("turn") || h->mouse == kWheel || (h->mouse == kMove && h->ms)) {
            // its updates are counted above (a move and the wheel are made once)
        } else if (p.contains("ms")) {
            int ms = p["ms"].get<int>();
            if (ms <= 0 || ms > 600000) { *err = "ms: 1..600000"; return false; }
            h->ms = static_cast<DWORD>(ms);
        } else {
            h->frames = p.value("frames", 1);
            if (h->frames <= 0 || h->frames > 100000) { *err = "frames: 1..100000"; return false; }
        }
        int afterMs = p.value("after_ms", 0), afterFrames = p.value("after_frames", 0);
        if (afterMs < 0 || afterMs > 600000 || afterFrames < 0 || afterFrames > 100000) {
            *err = "after_ms: 0..600000, after_frames: 0..100000";
            return false;
        }
        h->afterMs = static_cast<DWORD>(afterMs);
        h->afterFrames = afterFrames;
        h->asked = now;
    } catch (const json::exception &e) {
        *err = e.what();
        return false;
    }
    return true;
}

json Described(const Hold &h) {
    if (h.mouse == kMove) return json{{"mouse", {h.mx, h.my}}};
    if (h.mouse == kWheel) return json{{"wheel", h.value}};
    if (h.mouse == kButton) return json{{"button", h.name.substr(13)}};
    json j = {{h.key ? "key" : "action", h.name}, {h.key ? "dik" : "slot", h.slot}};
    if (!h.key) j["value"] = h.value;
    if (h.turn != 0) { j["turn"] = h.turn; j["frames"] = h.frames; }
    return j;
}

json Actions() {
    int n = SlotCount();
    uintptr_t c = game::Exe().controls;
    uint32_t bind = Rd(c + game::Exe().controlsBind), held = Rd(c + game::Exe().controlsHeld), pressed = Rd(c + game::Exe().controlsPressed);
    std::vector<SlotInfo> names = SlotNames(n);
    json out = json::array();
    static const char *kDevice[] = {"none", "key", "mouse", "joystick"};
    for (int s = 0; s < n; s++) {
        uint16_t dev = 0, code = 0;
        MemRead(bind + 12 * s + 4, &dev, 2);
        MemRead(bind + 12 * s + 6, &code, 2);
        float h = 0, p = 0;
        MemRead(held + 4 * s, &h, 4);
        MemRead(pressed + 4 * s, &p, 4);
        std::string bound = dev == 1 ? DikName(code) : dev ? GameString(KeyNameOf(s)) : "";
        out.push_back({{"slot", s}, {"action", names[s].action}, {"binding", names[s].index ? "secondary" : "primary"},
                       {"device", dev < 4 ? kDevice[dev] : "?"}, {"code", code}, {"bound_to", bound},
                       {"held", h}, {"pressed", p}});
    }
    return out;
}

json State() {
    json out = json::array();
    for (const Hold &h : g_holds)
        out.push_back({{"what", h.name}, {"key", h.key}, {"slot", h.slot}, {"value", h.value}, {"started", h.started},
                       {"frames_left", h.ms ? json() : json(h.frames)},
                       {"ms_left", !h.ms ? json() : json(static_cast<int>(h.started ? h.ms - (GetTickCount() - h.began) : h.ms))}});
    return out;
}

} // namespace

void InputInit() {
    if (!game::g_exe || !game::g_ls3d) return;
    const game::ExeBuild &e = game::Exe();
    std::string err;
    if (e.controlsUpdate) {
        g_actionsOk = HookAdd(nullptr, e.controlsUpdate, reinterpret_cast<void *>(&UpdateHook), reinterpret_cast<void **>(&g_update), &err);
        if (!g_actionsOk) HWARN("input: cannot hook the controls' update: %s", err.c_str());
    }
    if (game::Ls3d().keyboardUpdate) {
        uintptr_t at = ModuleBase("LS3DF.dll") + game::Ls3d().keyboardUpdate;
        g_keysOk = HookAdd(nullptr, at, reinterpret_cast<void *>(&KeyboardHook), reinterpret_cast<void **>(&g_keyboard), &err);
        if (!g_keysOk) HWARN("input: cannot hook the keyboard's read: %s", err.c_str());
    }
    if (game::Ls3d().mouseUpdate && game::Ls3d().mouseState) {
        uintptr_t at = ModuleBase("LS3DF.dll") + game::Ls3d().mouseUpdate;
        g_mouseOk = HookAdd(nullptr, at, reinterpret_cast<void *>(&MouseHook), reinterpret_cast<void **>(&g_mouse), &err);
        if (!g_mouseOk) HWARN("input: cannot hook the mouse's read: %s", err.c_str());
    }
}

// ---- keys and actions for mods (mml.h)
void InputFrame() {
    if (VirtualKeys()) {
        uint8_t state[256] = {};
        ApplyKeyHolds(state);
        KeysRead(state);
    }
    for (size_t i = 0; i < g_hotkeys.size(); i++) {   // (a hotkey's function may register hotkeys)
        if (!g_hotkeys[i].pending) continue;
        g_hotkeys[i].pending = false;
        Hotkey h = g_hotkeys[i];
        if (ModAlive(h.mod)) ModCall(h.mod, h.fn, h.user, "a hotkey");
    }
}

// A click for the menus: the cursor moved to (x, y) (800x600 units), the left button held 100 ms after 80 ms.
bool InputClick(float x, float y, std::string *err) {
    if (!g_mouseOk || !MenusMouseKnown()) { *err = "the menus' mouse is not known for this build"; return false; }
    Hold move, button;
    DWORD now = GetTickCount();
    move.mouse = kMove;
    move.mx = x;
    move.my = y;
    move.ms = 2000;
    move.asked = now;
    move.name = "mouse";
    button.mouse = kButton;
    button.slot = 0;
    button.ms = 100;
    button.afterMs = 80;
    button.asked = now;
    button.name = "mouse button left";
    g_holds.push_back(move);
    g_holds.push_back(button);
    return true;
}

void InputTapKey(int key, int frames) {
    Hold h;
    h.key = true;
    h.slot = key & 0xFF;
    h.frames = frames;
    h.asked = GetTickCount();
    h.name = DikName(h.slot);
    g_holds.push_back(h);
}

int KeyCodeOf(const char *name) {
    if (!name || !*name) return -1;
    for (const KeyName &n : kKeys) if (!_stricmp(n.name, name)) return n.dik;
    char *end;
    long v = strtol(name, &end, 0);
    return *end || v <= 0 || v > 255 ? -1 : static_cast<int>(v);
}
bool KeyDown(int key) { return key > 0 && key < 256 && g_keyNow[key]; }
bool KeyPressed(int key) { return key > 0 && key < 256 && g_keyEdge[key]; }

int ActionSlotOf(const char *name) {
    if (!name || !ActionsKnown()) return -1;
    std::string found;
    return SlotOf(json(std::string(name)), &found);
}

float ActionValue(int slot, bool pressed) {
    if (!ActionsKnown() || slot < 0 || slot >= SlotCount()) return 0.0f;
    float v = 0.0f;
    MemRead(Rd(game::Exe().controls + (pressed ? game::Exe().controlsPressed : game::Exe().controlsHeld)) + 4 * slot, &v, 4);
    return v;
}

// "F5", "Ctrl+F5", "Ctrl+Shift+R": modifiers, then the key (a modifier alone is a key too).
bool HotkeyAdd(MmlMod *mod, const char *combo, MmlHotkeyFn fn, void *user) {
    if (!ModAlive(mod) || !combo || !fn) return false;
    Hotkey h{mod, -1, false, false, false, fn, user, combo, false};
    std::string text = combo;
    for (size_t at = 0; at <= text.size();) {
        size_t plus = text.find('+', at);
        if (plus == std::string::npos) plus = text.size();
        std::string part = text.substr(at, plus - at);
        part.erase(0, part.find_first_not_of(" \t"));
        part.erase(part.find_last_not_of(" \t") + 1);
        bool last = plus == text.size();
        at = plus + 1;
        if (part.empty()) return false;
        if (!last) {
            if (!_stricmp(part.c_str(), "Ctrl") || !_stricmp(part.c_str(), "Control")) h.ctrl = true;
            else if (!_stricmp(part.c_str(), "Shift")) h.shift = true;
            else if (!_stricmp(part.c_str(), "Alt")) h.alt = true;
            else return false;
            continue;
        }
        if ((h.key = KeyCodeOf(part.c_str())) <= 0) return false;
    }
    if (h.key == kDikLCtrl || h.key == kDikRCtrl) h.ctrl = true;
    if (h.key == kDikLShift || h.key == kDikRShift) h.shift = true;
    if (h.key == kDikLAlt || h.key == kDikRAlt) h.alt = true;
    for (Hotkey &old : g_hotkeys)
        if (old.mod == mod && old.fn == fn && old.user == user) {   // bound again
            h.pending = old.pending && old.key == h.key && old.ctrl == h.ctrl && old.shift == h.shift && old.alt == h.alt;
            old = h;
            return true;
        }
    g_hotkeys.push_back(h);
    return true;
}

void HotkeysRemove(MmlMod *mod) {
    for (size_t i = 0; i < g_hotkeys.size();)
        if (g_hotkeys[i].mod == mod) g_hotkeys.erase(g_hotkeys.begin() + i);
        else i++;
}

bool InputMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method.compare(0, 6, "input.") && method.compare(0, 5, "text.")) return false;
    *ok = false;
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    if (!p.is_object()) p = json::object();
    // (keys need no game: with none, the keyboard is what input.key holds, and mods see it the same way)
    // (the actions need the controls' object; keys, the mouse and the sequences are checked step by step: a build
    // whose actions are not known may still know its keyboard)
    if ((method == "input.actions" || method == "input.act") && !ActionsKnown()) { *reply = "input: the game's actions are not known for this build"; return true; }
    if (method.compare(0, 5, "text.") == 0 && !(game::g_exe && game::Exe().textGetOrNull && game::Exe().textDb)) {
        *reply = "text: the text database is not known for this build";
        return true;
    }
    std::string err;
    if (method == "text.get") {
        if (!p.contains("id")) { *reply = "id: a text id"; return true; }
        unsigned id = p["id"].get<unsigned>();
        *reply = json{{"id", id}, {"text", TextOrNull(id) ? json(Text(id)) : json()}}.dump(-1, ' ', false, json::error_handler_t::replace);
    } else if (method == "text.find") {
        std::string want = p.value("text", "");
        unsigned from = p.value("from", 0u), to = p.value("to", 20000u);
        if (want.empty() || to < from || to - from > 200000) { *reply = "text: what to find; from, to: an id range (<= 200000)"; return true; }
        json hits = json::array();
        for (unsigned id = from; id <= to && hits.size() < 50; id++) {
            if (!TextOrNull(id)) continue;
            std::string t = Text(id);
            std::string a = t, b = want;
            for (char &ch : a) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            for (char &ch : b) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            if (a.find(b) != std::string::npos) hits.push_back({{"id", id}, {"text", t}});
        }
        *reply = hits.dump(-1, ' ', false, json::error_handler_t::replace);
    } else if (method == "input.actions") {
        *reply = Actions().dump(-1, ' ', false, json::error_handler_t::replace);
    } else if (method == "input.state") {
        json down = json::array(), hotkeys = json::array();
        for (int i = 0; i < 256; i++) if (g_keyNow[i]) down.push_back(DikName(i));
        for (const Hotkey &h : g_hotkeys) hotkeys.push_back({{"mod", ModIdOf(h.mod)}, {"combo", h.combo}});
        json out = {{"actions_hooked", g_actionsOk}, {"keys_hooked", g_keysOk}, {"mouse_hooked", g_mouseOk}, {"holds", State()},
                    {"keys_down", down}, {"hotkeys", hotkeys}};
        Cursor c = MenuCursor();
        if (MenusMouseKnown())
            out["cursor"] = {{"menu_open", c.menu}, {"x", std::round(c.px / c.sx * 10) / 10}, {"y", std::round(c.py / c.sy * 10) / 10}};
        *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
    } else if (method == "input.act" || method == "input.key" || method == "input.mouse") {
        bool act = method == "input.act", mouse = method == "input.mouse";
        if (mouse ? !p.contains("mouse") && !p.contains("button") && !p.contains("wheel")
                  : act ? !p.contains("action") && !p.contains("turn") : !p.contains("key")) {
            *reply = mouse ? "mouse: [x, y] (the menus' cursor moved there), button: \"left\" (held), or wheel: notches"
                     : act ? "action: what to hold (or turn: degrees)" : "key: what to hold";
            return true;
        }
        if (act || mouse) p.erase("key");
        if (!act) { p.erase("action"); p.erase("turn"); }
        if (!mouse) { p.erase("mouse"); p.erase("button"); p.erase("wheel"); }
        Hold h;
        if (!MakeHold(p, GetTickCount(), &h, &err)) { *reply = err; return true; }
        g_holds.push_back(h);
        *reply = Described(h).dump(-1, ' ', false, json::error_handler_t::replace);
    } else if (method == "input.text") {
        // each character a key press (held `ms`, one every `every_ms`), Shift around it where the US layout wants it
        std::string text = p.value("text", "");
        int every = p.value("every_ms", 120), ms = p.value("ms", 60);
        if (text.empty() || text.size() > 200 || every < 40 || every > 5000 || ms < 20 || ms >= every) {
            *reply = "text: up to 200 characters; every_ms 40..5000 (default 120), ms (each key held, default 60) below it";
            return true;
        }
        if (!g_keysOk && !VirtualKeys()) { *reply = "the keyboard's read is not hooked"; return true; }
        std::vector<Hold> holds;
        DWORD now = GetTickCount();
        for (size_t i = 0; i < text.size(); i++) {
            bool shift = false;
            int dik = CharKey(text[i], &shift);
            if (!dik) { *reply = std::string("text: '") + text[i] + "' is not a key of the US layout (letters, digits, space, punctuation)"; return true; }
            DWORD at = static_cast<DWORD>(i * every);
            Hold k;
            k.key = true; k.slot = dik; k.ms = static_cast<DWORD>(ms); k.afterMs = at + (shift ? 20 : 0); k.asked = now; k.name = DikName(dik);
            if (shift) {
                Hold s = k;
                s.slot = 0x2A; s.afterMs = at; s.ms = static_cast<DWORD>(ms + 40); s.name = DikName(0x2A);
                holds.push_back(s);
            }
            holds.push_back(k);
        }
        g_holds.insert(g_holds.end(), holds.begin(), holds.end());
        *reply = json{{"keys", holds.size()}, {"ms", text.size() * every}}.dump(-1, ' ', false, json::error_handler_t::replace);
    } else if (method == "input.seq") {
        if (!p.contains("steps") || !p["steps"].is_array() || p["steps"].empty() || p["steps"].size() > 256) {
            *reply = "steps: 1..256 of {action|key|turn, ms|frames, value, after_ms|after_frames}";
            return true;
        }
        std::vector<Hold> holds;
        json out = json::array();
        DWORD now = GetTickCount();
        for (const json &step : p["steps"]) {
            Hold h;
            if (!step.is_object() || !MakeHold(step, now, &h, &err)) {
                *reply = "step " + std::to_string(holds.size()) + ": " + (step.is_object() ? err : std::string("not an object"));
                return true;
            }
            holds.push_back(h);
            out.push_back(Described(h));
        }
        g_holds.insert(g_holds.end(), holds.begin(), holds.end());
        *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
    } else if (method == "input.release") {
        std::string what = p.value("what", "all");
        size_t before = g_holds.size();
        for (size_t i = 0; i < g_holds.size();) {
            if (what == "all" || Plain(g_holds[i].name) == Plain(what)) {
                if (!g_holds[i].key && !g_holds[i].mouse && g_holds[i].started) g_clearSlots.push_back(g_holds[i].slot);
                g_holds.erase(g_holds.begin() + i);
            } else i++;
        }
        *reply = json{{"released", before - g_holds.size()}}.dump(-1, ' ', false, json::error_handler_t::replace);
    } else {
        *reply = "unknown method '" + method + "' (input.actions, input.act, input.key, input.mouse, input.text, input.seq, input.release, input.state, text.get, text.find)";
        return true;
    }
    *ok = true;
    return true;
}

} // namespace host
