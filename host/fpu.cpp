// The x87's precision around the mods' code (docs/MODDING.md "Doubles on the game thread").
//
// The game's Direct3D device is made without D3DCREATE_FPU_PRESERVE, so Direct3D leaves the game thread's x87 at 24
// bits of precision (measured: fpu.state). 32-bit code computes part of its double arithmetic on the x87 (what the
// CRT's maths functions return through ST0, and what the compiler keeps there): at 24 bits a mod's doubles silently
// come out as floats, and the same code is exact in the offline tests (no Direct3D) and not in the game. So the
// loader runs every call into a mod (its events, commands, posted calls, load, unload: mods.cpp's Safe* gates) and
// its own menu pages (panel.cpp) with the x87 at 53 bits, and gives the game its own word back after, a fault too.
// The game's code keeps its own: native.call calls a game function with the game's word. A mod's detour on a game
// function (mml.h hook) is called by the game itself and runs with the game's word (a detour that needs doubles sets
// 53 bits itself and puts the game's back before it returns: _controlfp_s with _MCW_PC).
//
// Only the precision control (bits 8 and 9) is touched: rounding, the exception masks and SSE's MXCSR stay as they
// are. A word already at 53 bits (or 64) is left alone, so the scopes nest for nothing.
//
// Bridge method (game thread): fpu.state.
#include "host.h"
#include <cstdio>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {
constexpr uint16_t kPcMask = 0x0300, kPc24 = 0x0000, kPc53 = 0x0200, kPc64 = 0x0300;
uint16_t g_gameWord, g_frameExitWord;   // as the game entered the loader's frame, and as the frame gave it back
bool g_seenGame;
unsigned g_raised, g_entries;           // calls that needed 53 bits set / all the scopes entered

int Bits(uint16_t w) {
    switch (w & kPcMask) {
    case kPc24: return 24;
    case kPc53: return 53;
    case kPc64: return 64;
    default: return 0;
    }
}
std::string Hex(uint16_t w) {
    char b[8];
    snprintf(b, sizeof b, "0x%04X", w);
    return b;
}
} // namespace

uint16_t FpuWord() {
    uint16_t w;
    __asm fnstcw w
    return w;
}

void FpuSet(uint16_t w) {
    __asm fldcw w
}

uint16_t FpuDouble() {
    uint16_t w = FpuWord();
    g_entries++;
    if ((w & kPcMask) == kPc24) {   // (53 or 64 already: nothing to do)
        uint16_t d = static_cast<uint16_t>((w & ~kPcMask) | kPc53);
        FpuSet(d);
        g_raised++;
    }
    return w;
}

void FpuRestore(uint16_t w) {
    if (FpuWord() != w) FpuSet(w);
}

uint16_t FpuGameWord() { return g_seenGame ? g_gameWord : FpuWord(); }

void FpuFrameEnter() {
    g_gameWord = FpuWord();
    g_seenGame = true;
}
void FpuFrameExit() { g_frameExitWord = FpuWord(); }

bool FpuMethod(const std::string &method, const std::string &, std::string *reply, bool *ok) {
    if (method != "fpu.state") return false;
    uint16_t now = FpuWord();
    uint16_t in = FpuDouble();   // (as a mod's callback sees it)
    uint16_t mods = FpuWord();
    FpuRestore(in);
    json out = {{"game", {{"word", Hex(g_gameWord)}, {"bits", Bits(g_gameWord)}, {"seen", g_seenGame}}},
                {"frame_exit", {{"word", Hex(g_frameExitWord)}, {"bits", Bits(g_frameExitWord)}}},
                {"bridge_call", {{"word", Hex(now)}, {"bits", Bits(now)}}},
                {"mods", {{"word", Hex(mods)}, {"bits", Bits(mods)}}},
                {"scopes", g_entries}, {"raised", g_raised}};
    *reply = out.dump();
    *ok = true;
    return true;
}

} // namespace host
