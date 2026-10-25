// Bugs of the game itself, fixed by the loader where the game goes wrong, always (knowledge/notes/game.md "The game's
// own bugs"; game_build.h has the places, 0 in 1.0 for the two of 1.2 alone; tools/check_builds.py checks their bytes).
//
//   The options kept (1.2)   1.2 reads LS3D_setup (the options' details, draw distance, effects, sounds, shadows,
//                            particles; the screen's settings Setup.exe chose) from HKEY_CURRENT_USER at its start,
//                            where its Setup.exe writes it, but writes it at the quit to HKEY_LOCAL_MACHINE, as 1.0 does
//                            both: the options set in the game came back as they were after every restart (Windows
//                            keeps the write in its VirtualStore, which 1.2 never reads). The write's root made
//                            HKEY_CURRENT_USER: one byte of MafiaEntryPoint's push. The game writes it only when it
//                            quits by its menu's Quit (start.cpp gives a run's own screen its registry's values back).
//   Sniper Mode's key (1.2)  G_LoadSaveClass::ProfileLoad checks Sniper Mode's binding (slot 29, new in 1.2) against
//                            the others with loadsave_binding_find_conflict, which returns the conflicting slot or -1,
//                            and tests the result against 0: with no conflict the binding was reset, to R when R is
//                            free (the test of that is the same) or to none. Each `test eax, eax` made `inc eax; nop`:
//                            -1 is then 0 and a slot is not.
//   The controls' names      A binding's name in Options > Controls (controlsKeyName) is drawn by the game's font, in
//                            its language's code page (1252 for the English game). 1.2 names a key by DirectInput
//                            (IGraph's GetKeyName: the key's character in the keyboard layout of the moment) and turns
//                            it into the system's code page: in a Russian layout W is "ц", 1251's 0xF6, drawn as "ö".
//                            A joystick's axes and buttons are DirectInput's names in the system's language in both
//                            builds ("Ось Y"). A name the font cannot draw as it is becomes English: a key's as 1.0
//                            names it (its own table, read from 1.0's controlsKeyName), an axis "X Axis", a button
//                            "Button 3", a hat "POV 1". A key's name is taken from DirectInput's own (Unicode) in 1.2,
//                            so a layout's letter the font has (an AZERTY "A", a German "Ü") stays.
#include "host.h"
#include <cstdio>
#include <cstring>
#include <string>
#include "mml/game/game_build.h"
#include "mml/game/game.h"

namespace host {
namespace {

// ---- the controls' names
// 1.0's names of the keys (its controlsKeyName's table, DirectInput codes; Num Enter added: 1.0 has none)
struct KeyName { uint8_t dik; const char *name; };
constexpr KeyName kKeys10[] = {
    {0x01, "Escape"}, {0x02, "1"}, {0x03, "2"}, {0x04, "3"}, {0x05, "4"}, {0x06, "5"}, {0x07, "6"}, {0x08, "7"},
    {0x09, "8"}, {0x0A, "9"}, {0x0B, "0"}, {0x0C, "-"}, {0x0D, "="}, {0x0E, "Backspace"}, {0x0F, "Tab"}, {0x10, "Q"},
    {0x11, "W"}, {0x12, "E"}, {0x13, "R"}, {0x14, "T"}, {0x15, "Y"}, {0x16, "U"}, {0x17, "I"}, {0x18, "O"},
    {0x19, "P"}, {0x1A, "["}, {0x1B, "]"}, {0x1C, "Return"}, {0x1D, "Left Ctrl"}, {0x1E, "A"}, {0x1F, "S"},
    {0x20, "D"}, {0x21, "F"}, {0x22, "G"}, {0x23, "H"}, {0x24, "J"}, {0x25, "K"}, {0x26, "L"}, {0x27, ";"},
    {0x28, "'"}, {0x29, "`"}, {0x2A, "Left Shift"}, {0x2B, "\\"}, {0x2C, "Z"}, {0x2D, "X"}, {0x2E, "C"}, {0x2F, "V"},
    {0x30, "B"}, {0x31, "N"}, {0x32, "M"}, {0x33, ","}, {0x34, "."}, {0x35, "/"}, {0x36, "Right Shift"},
    {0x37, "Num *"}, {0x38, "Left Alt"}, {0x39, "Space"}, {0x3A, "Caps Lock"}, {0x3B, "F1"}, {0x3C, "F2"},
    {0x3D, "F3"}, {0x3E, "F4"}, {0x3F, "F5"}, {0x40, "F6"}, {0x41, "F7"}, {0x42, "F8"}, {0x43, "F9"}, {0x44, "F10"},
    {0x45, "Num Lock"}, {0x46, "Scroll Lock"}, {0x47, "Num 7"}, {0x48, "Num 8"}, {0x49, "Num 9"}, {0x4A, "Num -"},
    {0x4B, "Num 4"}, {0x4C, "Num 5"}, {0x4D, "Num 6"}, {0x4E, "Num +"}, {0x4F, "Num 1"}, {0x50, "Num 2"},
    {0x51, "Num 3"}, {0x52, "Num 0"}, {0x53, "Num ."}, {0x57, "F11"}, {0x58, "F12"}, {0x9C, "Num Enter"},
    {0x9D, "Right Ctrl"}, {0xB5, "Num /"}, {0xB7, "Print Screen"}, {0xB8, "Right Alt"}, {0xC5, "Pause"},
    {0xC7, "Home"}, {0xC8, "Cursor Up"}, {0xC9, "Page Up"}, {0xCB, "Cursor Left"}, {0xCD, "Cursor Right"},
    {0xCF, "End"}, {0xD0, "Cursor Down"}, {0xD1, "Page Down"}, {0xD2, "Insert"}, {0xD3, "Delete"},
    {0xDB, "Windows L"}, {0xDC, "Windows R"}, {0xDD, "Menu"},
};
// IJoy's axes, by index (its AxisOffset: X, Y, Z, the three rotations, the two sliders)
constexpr const char *kAxes[] = {"X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation", "Slider", "Slider 2"};

using KeyNameFn = const char *(__fastcall *)(void *controls, void *edx, const uint8_t *binding);   // thiscall, ret 4
KeyNameFn oKeyName;
bool g_keys12;     // 1.2: keys named by DirectInput
char g_name[64];   // (the callers copy a name at once, as they do the game's own buffer)

// A wide name in the game's text code page, when every character of it is there (no best fit: "ц" is not "c").
bool InGameCodePage(const wchar_t *w, char *out, int size) {
    BOOL lost = FALSE;
    int n = WideCharToMultiByte(game::TextCodePage(), WC_NO_BEST_FIT_CHARS, w, -1, out, size, nullptr, &lost);
    return n > 0 && !lost;
}

// A name in the system's code page (what the game turned IJoy's into) the font draws as it is: its characters are the
// same bytes in the game's code page.
bool Drawn(const char *s) {
    bool ascii = true;
    for (const char *p = s; *p; p++) ascii = ascii && static_cast<uint8_t>(*p) < 0x80;
    if (ascii) return true;
    wchar_t w[128];
    char back[128];
    if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, s, -1, w, 128)) return false;
    return InGameCodePage(w, back, sizeof back) && !strcmp(back, s);
}

const char *English(uint16_t device, uint16_t code) {
    if (device == 1) {
        for (const KeyName &k : kKeys10)
            if (k.dik == code) return k.name;
        snprintf(g_name, sizeof g_name, "Key %u", code);
    } else if (code < 0x100) {
        if (code < sizeof kAxes / sizeof *kAxes) return kAxes[code];
        snprintf(g_name, sizeof g_name, "Axis %u", code + 1);
    } else if (code < 0x200) {
        snprintf(g_name, sizeof g_name, "Button %u", code - 0x100 + 1);
    } else {
        snprintf(g_name, sizeof g_name, "POV %u", (code & 0xFF) + 1);
    }
    return g_name;
}

// A binding: {u32, u16 device (1 a key, 2 the mouse, 3 a joystick), u16 code, u32 the joystick}.
const char *__fastcall hkKeyName(void *controls, void *edx, const uint8_t *binding) {
    const char *name = oKeyName(controls, edx, binding);
    if (!binding) return name;
    uint16_t device, code;
    memcpy(&device, binding + 4, 2);
    memcpy(&code, binding + 6, 2);
    if (device == 1 && g_keys12) {
        // 1.2's own: IGraph::GetKeyName (its vtable's +0xCC, stdcall(IGraph*, dik) -> DirectInput's wide name)
        void *graph = *reinterpret_cast<void **>(game::Exe().igraph);
        using GetKeyNameFn = const wchar_t *(__stdcall *)(void *, int);
        const wchar_t *w = graph ? (*reinterpret_cast<GetKeyNameFn **>(graph))[0xCC / 4](graph, code & 0xFF) : nullptr;
        if (w && *w && InGameCodePage(w, g_name, 32)) return g_name;
        return English(device, code & 0xFF);
    }
    if ((device == 1 || device == 3) && name && !Drawn(name)) return English(device, code);
    return name;
}

// A patch of the game's code: one place or two, all or none.
void Patch(const char *what, uintptr_t a, uintptr_t b, const void *original, const void *fixed, size_t n) {
    if (!a) return;   // (not in this build)
    std::string err;
    bool ok = PatchAdd(nullptr, a, original, fixed, n, &err) && (!b || PatchAdd(nullptr, b, original, fixed, n, &err));
    if (!ok) PatchRemove(nullptr, a);
    if (ok) HLOG("fixes: %s", what);
    else HWARN("fixes: %s: not done (%s)", what, err.c_str());
}
} // namespace

void FixesInit() {
    if (!game::g_exe) return;
    const game::ExeBuild &e = game::Exe();
    const uint8_t hklm = 0x02, hkcu = 0x01;   // (the root's first byte: push 0x80000002 -> 0x80000001)
    Patch("the options written where the game reads them (HKEY_CURRENT_USER)", e.settingsSaveRoot ? e.settingsSaveRoot + 1 : 0, 0,
          &hklm, &hkcu, 1);
    const uint8_t test[2] = {0x85, 0xC0}, inc[2] = {0x40, 0x90};   // test eax, eax -> inc eax; nop
    Patch("Sniper Mode's key kept at a profile's load", e.sniperConflictTest, e.sniperRTest, test, inc, 2);
    if (e.controlsKeyName) {
        g_keys12 = e.igraph && !strcmp(GameBuild(), "1.2");
        std::string err;
        if (HookAdd(nullptr, e.controlsKeyName, reinterpret_cast<void *>(&hkKeyName), reinterpret_cast<void **>(&oKeyName), &err))
            HLOG("fixes: the controls' names the font cannot draw given in English");
        else
            HWARN("fixes: the controls' names: controlsKeyName not hooked (%s)", err.c_str());
    }
}

} // namespace host
