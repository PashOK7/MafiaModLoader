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
//   An empty traffic slot    The traffic manager's AI (C_traffic_car::AI) keeps its cars in slots (+0x74, +0x220 of
//   (both builds)            them, empty while a car is not out: 16 of 25 filled in the tester's crash). When a
//                            policeman asks for a car (ai_police_action fills a list) and every car is out, one loop
//                            looks for a car to take back and reads each slot without testing for an empty one, as
//                            the other loops do: a crash at C_traffic_car::AI+0x2B8 (the tester, after 1.5 hours of
//                            the freeride, a chase on). Its `cmp byte ptr [eax], 0; jz <next>` jumps to a stub that
//                            takes an empty slot as an inactive car (the same `jz`), and else does the same.
#include "host.h"
#include <cstdio>
#include <cstring>
#include <string>
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include "index_owner.h"
#include "shadow_frame.h"
#include <mmsystem.h>
#include <dsound.h>

namespace host {
namespace {

// Native GetTimer sleeps for the remainder of its 16 ms frame. Windows can
// otherwise coalesce those waits into roughly 15.6 ms scheduler ticks, causing
// periodic 30 ms frames. Own a matched multimedia timer request for this process.
using TimerPeriodFn = unsigned(WINAPI *)(unsigned);
struct FrameTimerPeriod {
    bool tried = false;
    TimerPeriodFn end = nullptr;
    ~FrameTimerPeriod() { if (end) end(1); }
} g_frameTimerPeriod;

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

// ---- an empty traffic slot: `cmp byte ptr [eax], 0; jz next` (5 bytes) made `jmp stub`; the stub: test eax, eax;
// jz next; cmp byte ptr [eax], 0; jz next; jmp back (after the game's jz). Its page is the loader's, never freed.
void TrafficSlots(uintptr_t at) {
    if (!at) return;
    uint8_t game[5];
    if (!MemRead(at, game, 5) || game[0] != 0x80 || game[1] != 0x38 || game[2] != 0x00 || game[3] != 0x74) {
        HWARN("fixes: an empty traffic slot: the game's bytes are not as expected, not done");
        return;
    }
    const uintptr_t next = at + 5 + static_cast<int8_t>(game[4]), back = at + 5;
    auto *stub = static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) { HWARN("fixes: an empty traffic slot: no memory for its stub, not done"); return; }
    size_t n = 0;
    auto rel32 = [&](uintptr_t to) {
        const int32_t r = static_cast<int32_t>(to - (reinterpret_cast<uintptr_t>(stub) + n + 4));
        memcpy(stub + n, &r, 4);
        n += 4;
    };
    stub[n++] = 0x85, stub[n++] = 0xC0;                     // test eax, eax
    stub[n++] = 0x0F, stub[n++] = 0x84, rel32(next);        // jz next
    stub[n++] = 0x80, stub[n++] = 0x38, stub[n++] = 0x00;   // cmp byte ptr [eax], 0
    stub[n++] = 0x0F, stub[n++] = 0x84, rel32(next);        // jz next
    stub[n++] = 0xE9, rel32(back);                          // jmp back
    FlushInstructionCache(GetCurrentProcess(), stub, n);
    uint8_t jmp[5] = {0xE9};
    const int32_t r = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (at + 5));
    memcpy(jmp + 1, &r, 4);
    std::string err;
    if (PatchAdd(nullptr, at, game, jmp, 5, &err)) HLOG("fixes: an empty traffic slot skipped when a policeman asks for a car");
    else HWARN("fixes: an empty traffic slot: not done (%s)", err.c_str());
}

uintptr_t g_pathGeneralDetour;
int g_pathNormalBegin;
void SingleWaypointDetour(MmlMidContext *context, void *) {
    uint32_t begin = 0;
    int32_t node = 0;
    if (MemRead(context->esi + g_pathNormalBegin, &begin, sizeof begin) &&
        MemRead(begin + 0x34, &node, sizeof node) && node == -1)
        context->eip = static_cast<uint32_t>(g_pathGeneralDetour);
}
void PathDetours(const game::ExeBuild &e) {
    if (!e.pathSingleDetour || !e.pathGeneralDetour) return;
    // Both builds reach this point only after testing the normal waypoint count against one.
    const uint8_t countTest[] = {0x83,0xFA,0x01,0x0F,0x85};
    const uint8_t arm12[] = {0x8B,0x46,0x1C,0x88,0x5E,0x70};
    const uint8_t arm10[] = {0x88,0x5E,0x70,0x83,0xEC,0x0C};
    const uint8_t general[] = {0x8B,0x46,0x58,0x3B,0xC3};
    uint8_t before[9], arm[6], destination[sizeof general];
    int32_t relative = 0;
    const uint8_t *expected = e.pathNormalBegin == 0x1C ? arm12 : arm10;
    if (!MemRead(e.pathSingleDetour - sizeof before, before, sizeof before) ||
        memcmp(before, countTest, sizeof countTest) ||
        !MemRead(e.pathSingleDetour, arm, sizeof arm) || memcmp(arm, expected, sizeof arm) ||
        !MemRead(e.pathGeneralDetour, destination, sizeof destination) || memcmp(destination, general, sizeof general)) {
        HWARN("fixes: single-waypoint detours: native code differs, not installed");
        return;
    }
    memcpy(&relative, before + 5, sizeof relative);
    if (e.pathSingleDetour + relative != e.pathGeneralDetour) {
        HWARN("fixes: single-waypoint detours: general arm differs, not installed");
        return;
    }
    g_pathGeneralDetour = e.pathGeneralDetour;
    g_pathNormalBegin = e.pathNormalBegin;
    std::string error;
    if (HookMid(nullptr, e.pathSingleDetour, SingleWaypointDetour, nullptr, &error))
        HLOG("fixes: single-waypoint routes without a web node use the native plane-based detour");
    else HWARN("fixes: single-waypoint detours: not installed (%s)", error.c_str());
}

uintptr_t g_ibOwnerClear, g_ibShutdownReturn;
void IndexOwnerAtShutdown(MmlMidContext *context, void *) {
    uint32_t caller = 0;
    // Verified 1.2 destructor/FreeAll stack at the three stores. Ordinary destruction keeps native behavior.
    if (!MemRead(context->esp + 0x44, &caller, sizeof caller) || caller != g_ibShutdownReturn) return;
    uint32_t direct3dBuffer = 0, index = 0;
    if (!MemRead(context->esi + 0x10, &direct3dBuffer, sizeof direct3dBuffer) ||
        !MemRead(context->ecx + 4, &index, sizeof index)) return;
    const bool cleared = ClearIndexOwner(reinterpret_cast<uint32_t *>(context->edx), context->esi, direct3dBuffer, index);
    // Native block deletion, buffer release and the remaining manager cleanup still run.
    context->eip = static_cast<uint32_t>(g_ibOwnerClear + 8);
    if (!cleared) HWARN("fixes: shutdown index slice has a stale owner %08X (buffer %08X, index %u); owner slot left alone",
                       context->edx, context->esi, index);
}
void IndexOwnersAtShutdown() {
    if (!game::g_ls3d) return;
    const auto &ls = game::Ls3d();
    if (!ls.ibShutdownClear || !ls.ibShutdownReturn) return;
    const uintptr_t base = ModuleBase("LS3DF.dll"), clear = base + ls.ibShutdownClear;
    const uint8_t expected[] = {0x8B,0x0F,0x8B,0x11,0x33,0xC0,0x89,0x02,0x89,0x42,0x04,0x89,0x42,0x08};
    const uint8_t shutdownCall[] = {0xE8,0xCC,0xB7,0xFE,0xFF};
    uint8_t actual[sizeof expected], caller[sizeof shutdownCall];
    if (!MemRead(clear - 6, actual, sizeof actual) || memcmp(actual, expected, sizeof actual) ||
        !MemRead(base + ls.ibShutdownReturn - sizeof caller, caller, sizeof caller) || memcmp(caller, shutdownCall, sizeof caller)) {
        HWARN("fixes: shutdown index owners: native code differs, not installed");
        return;
    }
    g_ibOwnerClear = clear;g_ibShutdownReturn = base + ls.ibShutdownReturn;
    std::string error;
    if (HookMid(nullptr, clear, IndexOwnerAtShutdown, nullptr, &error)) HLOG("fixes: stale index owners checked during final driver cleanup");
    else HWARN("fixes: shutdown index owners: not installed (%s)", error.c_str());
}

void UpdateVehicleShadowReceiver(MmlMidContext *context, void *next) {
    if (CanUpdateShadowFrame(reinterpret_cast<const void *>(context->eax))) return;
    // Resume before the entry's name is freed and the native loop advances, with the stack unchanged.
    context->eip = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(next));
    HWARN("fixes: skipped Update on stale vehicle shadow receiver %08X during save cleanup", context->eax);
}

void VehicleShadowCleanup(const game::ExeBuild &e) {
    const uintptr_t at = e.doneVehicleShadowUpdate;
    constexpr uint8_t expected[] = {0x50,0x8B,0x08,0xFF,0x51,0x18,0x8B,0x95,0xA4,0x02,0,0,0x8B,0x44,0x32,0x10};
    uint8_t bytes[sizeof expected];
    if (!at || !MemRead(at, bytes, sizeof bytes) || memcmp(bytes, expected, sizeof bytes)) {
        HWARN("fixes: vehicle shadow cleanup: native code differs, not installed");
        return;
    }
    std::string error;
    if (HookMid(nullptr, at, UpdateVehicleShadowReceiver, reinterpret_cast<void *>(at + 6), &error))
        HLOG("fixes: vehicle shadow receivers checked during scene cleanup");
    else HWARN("fixes: vehicle shadow cleanup: not installed (%s)", error.c_str());
}
// The native shadow updater already unlinks a shadow when its texture update fails. The rasterizer
// trusts every 16-bit index as an offset into the projected vertices.
using ShadowTextureFn = bool(__fastcall *)(void *, void *);
ShadowTextureFn oShadowTexture;

struct ShadowIndexError {
    uint32_t triangle = 0, index = 0, vertices = 0, triangles = 0;
};

bool CanRasterizeShadow(void *frame, ShadowIndexError &error) {
    __try {
        const uint32_t flags = game::at<uint32_t>(frame, 0x208);
        if ((flags & 0x400) && (!(flags & 6) || game::at<int32_t>(frame, 0x1f4) >= 2))
            return true; // The native call reuses its cached texture.
        if (flags & 0x8000) return true; // The native call already fails before rasterizing.

        const uint32_t vertices = game::at<uint32_t>(frame, 0x154);
        const auto *indices = game::at<const uint16_t *>(frame, 0x158);
        const uint32_t triangles = game::at<uint32_t>(frame, 0x15c);
        if (!game::at<const void *>(frame, 0x150) || !vertices ||
            !indices || !triangles || triangles > UINT32_MAX / 6 ||
            reinterpret_cast<uintptr_t>(indices) > UINTPTR_MAX - triangles * 6)
            return false;
        for (uint32_t i = 0; i < triangles * 3; ++i) {
            const uint32_t index = indices[i];
            if (index >= vertices) {
                error = {i / 3 + 1, index, vertices, triangles};
                return false;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool __fastcall GuardShadowTexture(void *shadow, void *edx) {
    ShadowIndexError error;
    if (CanRasterizeShadow(shadow, error)) return oShadowTexture(shadow, edx);
    static bool reported = false; // This update runs on the game thread.
    if (!reported) {
        reported = true;
        if (error.triangle)
            HWARN("fixes: skipped a shadow with invalid raster geometry: triangle %u of %u, index %u, %u vertices",
                  error.triangle, error.triangles, error.index, error.vertices);
        else
            HWARN("fixes: skipped a shadow with invalid raster geometry: unreadable data or invalid counts");
    }
    return false;
}

void InstallShadowTextureGuard() {
    if (!game::g_ls3d) return;
    const uintptr_t at = ModuleBase("LS3DF.dll") + game::Ls3d().updateShadowTexture;
    constexpr uint8_t expected[] = {0x56,0x8B,0xF1,0x8B,0x86,0x08,0x02,0,0,0xF6,0xC4,0x04};
    uint8_t bytes[sizeof expected];
    if (!MemRead(at, bytes, sizeof bytes) || memcmp(bytes, expected, sizeof bytes)) {
        HWARN("fixes: shadow texture update: native code differs, not installed");
        return;
    }
    std::string error;
    if (HookAdd(nullptr, at, reinterpret_cast<void *>(&GuardShadowTexture),
                reinterpret_cast<void **>(&oShadowTexture), &error))
        HLOG("fixes: shadow triangle indices checked before texture updates");
    else HWARN("fixes: shadow texture update: not installed (%s)", error.c_str());
}
// With no hardware 3D capacity, deferred DirectSound buffers needlessly reallocate on Stop/Play.
// On the tested Windows backend that reapplies allocation-time gain AND rate, while getters still report
// the later values. Select LS3D's existing software-buffer path before any source is opened instead.
// Real hardware/EAX providers retain their native policy. No per-Play parameter edits are performed.
using SoundDriverInitFn = int(__stdcall *)(void *, void *, int, int, int, unsigned);
SoundDriverInitFn oSoundDriverInit;
bool g_soundInitHooked, g_soundInitialized, g_soundSoftware, g_soundPolicyChanged;
DWORD g_soundMaxHw3d, g_soundMaxHwMixing, g_soundNativeFlags;
HRESULT g_soundCapsResult = E_PENDING;
int __stdcall SoundDriverInit(void *driver, void *window, int frequency, int channels, int bits, unsigned flags) {
    const int result = oSoundDriverInit(driver, window, frequency, channels, bits, flags);
    g_soundInitialized = result == 0;
    g_soundPolicyChanged = false;
    g_soundCapsResult = E_PENDING;
    g_soundMaxHw3d = g_soundMaxHwMixing = 0;
    g_soundNativeFlags = g_soundSoftware = 0;
    if (result || !driver) return result;
    auto *directSound = game::at<IDirectSound8 *>(driver, 0x0C);
    DSCAPS caps = {}; caps.dwSize = sizeof(caps);
    g_soundCapsResult = directSound ? directSound->GetCaps(&caps) : E_POINTER;
    unsigned &nativeFlags = game::at<unsigned>(driver, 0x30);
    if (SUCCEEDED(g_soundCapsResult)) {
        g_soundMaxHw3d = caps.dwMaxHw3DAllBuffers;
        g_soundMaxHwMixing = caps.dwMaxHwMixingAllBuffers;
        constexpr unsigned kNoHardwareChannels = 0x20, kEaxProvider = 0x100000;
        // Windows can advertise one generic mixing slot while advertising no hardware 3D buffers.
        // The game's spatial voices need the latter; a generic slot is not evidence of 3D acceleration.
        if (!caps.dwMaxHw3DAllBuffers && !(nativeFlags & kEaxProvider)) {
            g_soundPolicyChanged = !(nativeFlags & kNoHardwareChannels);
            nativeFlags |= kNoHardwareChannels;
            HLOG("fixes: sound device has no hardware 3D capacity; native software buffers selected before source creation");
        }
    } else HWARN("fixes: sound capabilities unavailable (%08lX); native allocation policy retained", g_soundCapsResult);
    g_soundNativeFlags = nativeFlags;
    g_soundSoftware = (nativeFlags & 0x20) != 0;
    return result;
}
void InstallSoundAllocationPolicy() {
    if (!game::g_ls3d || !game::Ls3d().soundDriverInit) return;
    const uintptr_t at = ModuleBase("LS3DF.dll") + game::Ls3d().soundDriverInit;
    constexpr uint8_t expected[] = {0x8B,0x44,0x24,0x18,0x8B,0x4C,0x24,0x14,0x8B,0x54,0x24,0x10};
    uint8_t bytes[sizeof expected];
    if (!MemRead(at, bytes, sizeof bytes) || memcmp(bytes, expected, sizeof bytes)) {
        HWARN("fixes: native sound initialization differs; allocation policy not installed");
        return;
    }
    std::string error;
    g_soundInitHooked = HookAdd(nullptr, at, reinterpret_cast<void *>(&SoundDriverInit),
                               reinterpret_cast<void **>(&oSoundDriverInit), &error);
    if (g_soundInitHooked) HLOG("fixes: native sound allocation follows device hardware capacity");
    else HWARN("fixes: sound allocation policy not installed (%s)", error.c_str());
}
} // namespace

std::string SoundAllocationState() {
    char text[384];
    snprintf(text, sizeof text, "{\"hooked\":%s,\"initialized\":%s,\"software_buffers\":%s,\"changed\":%s,"
             "\"max_hw_3d\":%lu,\"max_hw_mixing\":%lu,\"caps_hresult\":%ld,\"native_flags\":%lu,\"gain_workaround\":false}",
             g_soundInitHooked ? "true" : "false", g_soundInitialized ? "true" : "false",
             g_soundSoftware ? "true" : "false", g_soundPolicyChanged ? "true" : "false",
             g_soundMaxHw3d, g_soundMaxHwMixing, g_soundCapsResult, g_soundNativeFlags);
    std::string result = text;
    result.pop_back();
    return result + ",\"ogg_shutdown\":" + OggFixState() + "}";
}

void FixesFrame() {
    if (g_frameTimerPeriod.tried) return;
    g_frameTimerPeriod.tried = true;
    // Run on the game thread after DLL initialization, before its first wait.
    // LS3DF imports winmm in both supported builds, so no module is loaded here.
    HMODULE winmm = GetModuleHandleW(L"winmm.dll");
    auto begin = reinterpret_cast<TimerPeriodFn>(GetProcAddress(winmm, "timeBeginPeriod"));
    auto end = reinterpret_cast<TimerPeriodFn>(GetProcAddress(winmm, "timeEndPeriod"));
    if (begin && end && begin(1) == 0) {
        g_frameTimerPeriod.end = end;
        HLOG("fixes: native frame waits use a process-owned 1 ms timer period");
    } else {
        HWARN("fixes: a 1 ms frame timer period is unavailable; native waits retained");
    }
}

void FixesInit() {
    if (!game::g_exe) return;
    const game::ExeBuild &e = game::Exe();
    const uint8_t hklm = 0x02, hkcu = 0x01;   // (the root's first byte: push 0x80000002 -> 0x80000001)
    Patch("the options written where the game reads them (HKEY_CURRENT_USER)", e.settingsSaveRoot ? e.settingsSaveRoot + 1 : 0, 0,
          &hklm, &hkcu, 1);
    const uint8_t test[2] = {0x85, 0xC0}, inc[2] = {0x40, 0x90};   // test eax, eax -> inc eax; nop
    Patch("Sniper Mode's key kept at a profile's load", e.sniperConflictTest, e.sniperRTest, test, inc, 2);
    TrafficSlots(e.trafficSlotTest);
    PathDetours(e);
    IndexOwnersAtShutdown();
    VehicleShadowCleanup(e);
    InstallShadowTextureGuard();
    InstallSoundAllocationPolicy();
    OggFixInit();
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
