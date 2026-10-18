// DebugText.asi - shows the script command debug_text on screen for Mafia: The City of Lost Heaven (Game.exe 1.2 GOG)
//
// The retail game knows the keyword debug_text but never compiles or draws it (it was a developer tool).
// This mod compiles it and draws the lines in a separate panel on the middle-left side of the screen
// (Segoe UI on a semi-transparent black box) - NOT the HUD console (where weapon names etc. appear).
//
//   debug_text "Salieri je v aute"            plain text
//   debug_text "Hodnota = %n", 12              %n = value of flt[12]
//   debug_text "Ridic: %a", 3                  %a = name of the actor in act[3]
//
// How:  C_program::DecodeInstruction (0x463960) learns the keyword -> custom opcode 0xB1B,
//       C_program::ParseArgs (0x461C70) stores the text in our own table, the opcode dispatch
//       in C_program::Process (0x46D3FE) runs it. The text is rendered with GDI into our own texture
//       and drawn through LS3D's Direct3D 8 device (LS3DF.dll+0x1C597C) at IGraph::Present, only on
//       frames where the game rendered its HUD (render call 0x5BD8E8 -> 0x5FB060 is hooked for that).
//       The three script hooks are chain-aware: MafiaExtras.asi (blackbars) hooks the same places
//       and both mods work together in any load order.
//
// Build (clang via zig, no CRT):
//   zig c++ -target x86-windows-gnu -mcpu=pentium4 -O2 -fno-exceptions -fno-rtti -fno-builtin -fno-stack-protector \
//       -c DebugText.cpp -o DebugText.o
//   zig c++ -target x86-windows-gnu -shared -nostdlib -s -Wl,--entry=DllMain@12 DebugText.o \
//       -o DebugText.asi -lkernel32 -lgdi32
// Install (by the user): DebugText.asi + DebugText.ini next to Game.exe (ASI loader).

#include <stdint.h>
#include <stddef.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>

#define THISC __attribute__((thiscall))

// ------------------------------------------------------------------ addresses (Game.exe 1.2)
static const uint32_t A_DECODE     = 0x00463960;   // C_program::DecodeInstruction(char*, int&)
static const uint32_t A_PARSEARGS  = 0x00461C70;   // C_program::ParseArgs(char*, instr*)
static const uint32_t A_DISPATCH   = 0x0046D3FE;   // mov eax,[eax]; dec eax; cmp eax,0x18b
static const uint32_t A_DISP_JA    = 0x0046D406;   // ja default / jmp [table]
static const uint32_t A_DEFAULT    = 0x00479996;   // default case: next instruction
static const uint32_t A_HUD        = 0x006BF980;   // G_IndicatorsClass (HUD) singleton
static const uint32_t A_HUD_CALL   = 0x005BD8E8;   // call HUD render (0x5FB060)
static const uint32_t A_HUD_RENDER = 0x005FB060;
static const uint32_t A_MISSION    = 0x0063788C;
enum { MISSION_GAME = 0x24 };
enum { OP_DEBUGTEXT = 0xB1B };
enum { PROG_FLT = 0x48, PROG_FLTN = 0x4C, PROG_ACT = 0x58, PROG_ACTN = 0x5C, ACTOR_FRAME = 0x68, FRAME_NAME = 0x100 };

static const uint8_t kDecodeBytes[]   = { 0xB8, 0x00, 0x38, 0x00, 0x00 };                       // mov eax,0x3800
static const uint8_t kParseBytes[]    = { 0x8B, 0x54, 0x24, 0x04, 0x83, 0xEC, 0x40 };           // mov edx,[esp+4]; sub esp,0x40
static const uint8_t kDispatchBytes[] = { 0x8B, 0x00, 0x48, 0x3D, 0x8B, 0x01, 0x00, 0x00, 0x0F, 0x87 };

// ------------------------------------------------------------------ tiny CRT
#ifndef DT_EMBED
extern "C" void* memset(void* d, int c, size_t n) { uint8_t* p = (uint8_t*)d; while (n--) *p++ = (uint8_t)c; return d; }
extern "C" void* memcpy(void* d, const void* s, size_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; return d; }
extern "C" int _fltused = 0;
#endif

#ifdef DT_EMBED
#define DT_GEN   "DebugText"
#define DT_PANEL "DebugTextPanel"
extern "C" int g_dtShow;                           // host menu switch: draw the panel or not
#else
#define DT_GEN   "General"
#define DT_PANEL "Panel"
static int g_dtShow = 1;
#endif
static size_t SLen(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
static void SCat(char* d, size_t cap, const char* s) { size_t n = SLen(d); while (*s && n + 1 < cap) d[n++] = *s++; d[n] = 0; }
static void SCatX(char* d, size_t cap, uint32_t v) {
    char t[11]; t[0] = '0'; t[1] = 'x';
    for (int i = 0; i < 8; ++i) t[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    t[10] = 0; SCat(d, cap, t);
}
static void SCatU(char* d, size_t cap, uint32_t v) {
    char t[12]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char o[12]; int i = 0; while (n) o[i++] = t[--n]; o[i] = 0; SCat(d, cap, o);
}
static void SCatF(char* d, size_t cap, float f) {
    if (f < 0) { SCat(d, cap, "-"); f = -f; }
    uint32_t i = (uint32_t)f; uint32_t fr = (uint32_t)((f - (float)i) * 100.0f + 0.5f);
    if (fr > 99) { fr = 99; }
    SCatU(d, cap, i); SCat(d, cap, "."); if (fr < 10) SCat(d, cap, "0"); SCatU(d, cap, fr);
}
static int Lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

// ------------------------------------------------------------------ log + ini
static char g_ini[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;
static void Log(const char* s) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char b[400]; b[0] = 0; SCat(b, sizeof b, "["); SCatU(b, sizeof b, GetTickCount()); SCat(b, sizeof b, "] ");
    SCat(b, sizeof b, s); SCat(b, sizeof b, "\r\n");
    DWORD w; WriteFile(g_log, b, (DWORD)SLen(b), &w, NULL);
}
static int IniInt(const char* sec, const char* key, int def) { return (int)GetPrivateProfileIntA(sec, key, def, g_ini); }
static float IniFloat(const char* sec, const char* key, float def) {
    char buf[32]; GetPrivateProfileStringA(sec, key, "", buf, sizeof buf, g_ini);
    const char* s = buf; while (*s == ' ') ++s;
    if (!*s) return def;
    bool neg = false; if (*s == '-') { neg = true; ++s; }
    float v = 0, frac = 0.1f; bool dot = false, any = false;
    for (; *s; ++s) {
        if (*s == '.' || *s == ',') { dot = true; continue; }
        if (*s < '0' || *s > '9') break;
        any = true;
        if (!dot) v = v * 10 + (*s - '0'); else { v += (*s - '0') * frac; frac *= 0.1f; }
    }
    if (!any) return def;
    return neg ? -v : v;
}
static uint32_t IniHex(const char* sec, const char* key, uint32_t def) {
    char b[32]; GetPrivateProfileStringA(sec, key, "", b, sizeof b, g_ini);
    const char* p = b; while (*p == ' ') ++p;
    if (!*p) return def;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    uint32_t v = 0; bool any = false;
    for (; *p; ++p) {
        int c = Lower((uint8_t)*p), d;
        if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else break;
        v = v * 16 + (uint32_t)d; any = true;
    }
    return any ? v : def;
}

struct Cfg {
    int log, logLines;
    float x, y, size, width, pad, background;
    int maxLines, showMs, fadeMs, bold, codePage;
    uint32_t colour;
    wchar_t fontName[64];
} C;

static void LoadCfg() {
    C.log        = IniInt(DT_GEN, "Log", 1);
    C.logLines   = IniInt(DT_GEN, "LogLines", 1);
    C.x          = IniFloat(DT_PANEL, "X", 15.0f);       // virtual 800x600 coordinates
    C.y          = IniFloat(DT_PANEL, "Y", 200.0f);      // top of the panel
    C.width      = IniFloat(DT_PANEL, "Width", 380.0f);  // max width, longer lines wrap
    C.size       = IniFloat(DT_PANEL, "TextSize", 12.0f);
    C.pad        = IniFloat(DT_PANEL, "Padding", 3.0f);
    C.background = IniFloat(DT_PANEL, "Background", 0.6f);
    C.maxLines   = IniInt(DT_PANEL, "MaxLines", 9);
    C.showMs     = IniInt(DT_PANEL, "ShowMs", 3000);     // 0 = lines stay until pushed out
    C.fadeMs     = IniInt(DT_PANEL, "FadeMs", 1000);
    C.bold       = IniInt(DT_PANEL, "Bold", 0);
    C.codePage   = IniInt(DT_PANEL, "CodePage", 1250);
    C.colour     = IniHex(DT_PANEL, "Colour", 0xFFFFFF) & 0xFFFFFF;
    char fn[64]; GetPrivateProfileStringA(DT_PANEL, "Font", "Segoe UI", fn, sizeof fn, g_ini);
    if (!fn[0] || (fn[0] >= '0' && fn[0] <= '9' && !fn[1])) { fn[0] = 0; SCat(fn, sizeof fn, "Segoe UI"); }  // old ini: Font=1
    if (MultiByteToWideChar(CP_ACP, 0, fn, -1, C.fontName, 64) <= 0) C.fontName[0] = 0;
    if (C.maxLines < 1) C.maxLines = 1; if (C.maxLines > 60) C.maxLines = 60;
    if (C.size < 4) C.size = 4; if (C.size > 60) C.size = 60;
    if (C.pad < 0) C.pad = 0;
    if (C.background < 0) C.background = 0; if (C.background > 1) C.background = 1;
    if (C.showMs < 0) C.showMs = 0; if (C.fadeMs < 0) C.fadeMs = 0;
}

// ------------------------------------------------------------------ patch helpers (chain-aware)
static uint8_t* g_exec = NULL; static size_t g_execUsed = 0;
static uint8_t* ExecAlloc(size_t n) {
    if (!g_exec) { g_exec = (uint8_t*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); if (!g_exec) return NULL; }
    if (g_execUsed + n > 4096) return NULL;
    uint8_t* p = g_exec + g_execUsed; g_execUsed += (n + 15) & ~15u; return p;
}
static bool Readable(const void* p, size_t n) { return !IsBadReadPtr(p, n); }
static bool Same(uint32_t addr, const uint8_t* b, size_t n) {
    if (!Readable((void*)addr, n)) return false;
    for (size_t i = 0; i < n; ++i) if (((uint8_t*)addr)[i] != b[i]) return false;
    return true;
}
static void Rel32(uint8_t* at, uint32_t target) { *(int32_t*)at = (int32_t)(target - ((uint32_t)at + 4)); }
static uint32_t JmpTarget(uint32_t addr) { return addr + 5 + *(int32_t*)(addr + 1); }
static void* Trampoline(uint32_t addr, size_t n) {
    uint8_t* t = ExecAlloc(n + 5); if (!t) return NULL;
    memcpy(t, (void*)addr, n); t[n] = 0xE9; Rel32(t + n + 1, addr + n);
    return t;
}
// What to call as "the original": a trampoline when the code is untouched, or the hook
// another mod already placed there (it ends in the real original itself). NULL = unknown code.
static void* NextInChain(uint32_t addr, const uint8_t* orig, size_t n) {
    if (Same(addr, orig, n)) return Trampoline(addr, n);
    if (Readable((void*)addr, 5) && *(uint8_t*)addr == 0xE9) return (void*)JmpTarget(addr);
    return NULL;
}
static bool JmpPatch(uint32_t addr, size_t n, uint32_t target) {
    DWORD old; if (!VirtualProtect((void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t* p = (uint8_t*)addr; p[0] = 0xE9; Rel32(p + 1, target);
    for (size_t i = 5; i < n; ++i) p[i] = 0x90;
    VirtualProtect((void*)addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, n);
    return true;
}

// ------------------------------------------------------------------ script command
typedef int  (THISC *Fn_Decode)(void* self, char* text, int* len);
typedef char (THISC *Fn_Parse)(void* self, char* args, int* instr);
static Fn_Decode g_origDecode = NULL;
static Fn_Parse  g_origParse  = NULL;

static int MatchKw(const char* text, const char* kw) {
    int i = 0;
    while (kw[i] && Lower((uint8_t)text[i]) == kw[i]) ++i;
    if (!kw[i] && (text[i] == 0 || text[i] == ' ' || text[i] == '\t')) return i;
    return 0;
}
static int THISC DecodeHook(void* self, char* text, int* len) {
    int n;
    if (text && (n = MatchKw(text, "debug_text")) != 0) { if (len) *len = n; return OP_DEBUGTEXT; }
    return g_origDecode(self, text, len);
}
static bool ParseInt(const char*& s, int& out) {
    while (*s == ' ' || *s == '\t' || *s == ',') ++s;
    bool neg = false; if (*s == '-') { neg = true; ++s; }
    if (*s < '0' || *s > '9') return false;
    int v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    out = neg ? -v : v; return true;
}

struct DbgLine { char text[100]; int reg; };
enum { DBG_MAX = 2048 };
static DbgLine g_dbg[DBG_MAX];
static int g_dbgN = 0;

static int DbgStore(const char* args) {
    const char* s = args ? args : "";
    while (*s == ' ' || *s == '\t') ++s;
    char t[100]; int n = 0;
    if (*s == '"') {
        ++s;
        while (*s && *s != '"') { if (n + 1 < (int)sizeof t) t[n++] = *s; ++s; }
        if (*s == '"') ++s;
    } else {
        while (*s && *s != '\r' && *s != '\n') { if (n + 1 < (int)sizeof t) t[n++] = *s; ++s; }
        while (n && (t[n - 1] == ' ' || t[n - 1] == '\t')) --n;
    }
    t[n] = 0;
    int reg = -1; if (!ParseInt(s, reg)) reg = -1;
    for (int i = 0; i < g_dbgN; ++i) {          // mission reloads - reuse identical lines
        if (g_dbg[i].reg != reg) continue;
        int j = 0; while (j <= n && g_dbg[i].text[j] == t[j]) ++j;
        if (j > n) return i + 1;
    }
    if (g_dbgN >= DBG_MAX) return 0;
    memcpy(g_dbg[g_dbgN].text, t, (size_t)n + 1); g_dbg[g_dbgN].reg = reg;
    return ++g_dbgN;
}
// The game frees every instruction's payload with its CRT free() when a script is destroyed
// (leaving the mission, an actor's script ending...). So the payload must be a real block from the
// game's own allocator - a plain number there (as before) made free() crash inside ntdll.
static const uint32_t A_GAME_NEW = 0x006106FC;   // _nh_malloc(size, 1) - pairs with free 0x0060FCF6
typedef void* (__cdecl *Fn_GameNew)(uint32_t size);
static uint32_t GameBox(uint32_t v) {
    uint32_t* p = (uint32_t*)((Fn_GameNew)A_GAME_NEW)(4);
    if (!p) return 0;
    *p = v; return (uint32_t)p;
}
static uint32_t Unbox(uint32_t arg) {
    return (arg && !IsBadReadPtr((void*)arg, 4)) ? *(uint32_t*)arg : 0;
}

static char THISC ParseHook(void* self, char* args, int* instr) {
    if (instr && instr[0] == OP_DEBUGTEXT) {
        int id = DbgStore(args);
        if (!id) Log("debug_text: table full, line ignored");
        instr[1] = GameBox((uint32_t)id);   // boxed id (0 = do nothing at run time)
        return 1;
    }
    return g_origParse(self, args, instr);
}

// ------------------------------------------------------------------ on-screen panel
// The game clock (C_game +0x2B0C, ms) - advanced only by game_tick (0x5B4D59), so it stands still
// while the ESC menu / any modal menu is open: the lines freeze there together with the world.
static const uint32_t A_MISSION_ = 0x0063788C;
enum { GAME_CLOCK = 0x2B0C };
static DWORD GameClock() {
    uint8_t** pm = (uint8_t**)A_MISSION_;
    if (IsBadReadPtr(pm, 4) || !*pm || IsBadReadPtr(*pm + 0x24, 4)) return 0;
    uint8_t* g = *(uint8_t**)(*pm + 0x24);
    if (!g || IsBadReadPtr(g + GAME_CLOCK, 4)) return 0;
    return *(DWORD*)(g + GAME_CLOCK);
}

enum { PANEL_MAX = 30 };
struct ShownLine { char text[160]; DWORD t; };
static ShownLine g_shown[PANEL_MAX];
static int g_shownN = 0;
static CRITICAL_SECTION g_cs;
static volatile LONG g_ver = 1;                  // bumped whenever the shown lines change

static void PanelAdd(const char* s) {
    EnterCriticalSection(&g_cs);
    if (g_shownN >= C.maxLines) {
        for (int i = 1; i < g_shownN; ++i) g_shown[i - 1] = g_shown[i];
        g_shownN = C.maxLines - 1;
    }
    ShownLine& L = g_shown[g_shownN++];
    L.text[0] = 0; SCat(L.text, sizeof L.text, s); L.t = GameClock();
    InterlockedIncrement(&g_ver);
    LeaveCriticalSection(&g_cs);
}

static void DebugRun(uint32_t id, uint8_t* prog) {
    if (!id || id > (uint32_t)g_dbgN) return;
    const DbgLine& L = g_dbg[id - 1];
    char out[160]; out[0] = 0;
    for (const char* p = L.text; *p; ++p) {
        if (*p == '%' && (p[1] == 'n' || p[1] == 'a')) {
            char kind = p[1]; ++p;
            int r = L.reg; bool done = false;
            if (kind == 'n' && prog && r >= 0 && Readable(prog + PROG_FLT, 8)) {
                float* f = *(float**)(prog + PROG_FLT); int cnt = *(int*)(prog + PROG_FLTN);
                if (f && r < cnt && Readable(f + r, 4)) {
                    float v = f[r];
                    if (v == (float)(int)v && v > -1e9f && v < 1e9f) {
                        int iv = (int)v; if (iv < 0) { SCat(out, sizeof out, "-"); iv = -iv; }
                        SCatU(out, sizeof out, (uint32_t)iv);
                    } else SCatF(out, sizeof out, v);
                    done = true;
                }
            } else if (kind == 'a' && prog && r >= 0 && Readable(prog + PROG_ACT, 8)) {
                uint8_t** a = *(uint8_t***)(prog + PROG_ACT); int cnt = *(int*)(prog + PROG_ACTN);
                if (a && r < cnt && Readable(a + r, 4)) {
                    uint8_t* act = a[r];
                    if (!act) { SCat(out, sizeof out, "(null)"); done = true; }
                    else if (Readable(act + ACTOR_FRAME, 4)) {
                        uint8_t* fr = *(uint8_t**)(act + ACTOR_FRAME);
                        if (fr && Readable(fr + FRAME_NAME, 4)) {
                            const char* nm = *(const char**)(fr + FRAME_NAME);
                            if (nm && Readable(nm, 1)) { SCat(out, sizeof out, nm); done = true; }
                        }
                    }
                }
            }
            if (!done) SCat(out, sizeof out, "?");
        } else {
            char c[2] = { *p, 0 }; SCat(out, sizeof out, c);
        }
    }
    if (C.log && C.logLines) { char l[200]; l[0] = 0; SCat(l, sizeof l, "debug_text: "); SCat(l, sizeof l, out); Log(l); }
    PanelAdd(out);
}

// called from the dispatch stub (game thread, inside C_program::Process)
extern "C" void __cdecl ScriptRun(uint32_t op, uint32_t arg, uint8_t* prog) {
    if (op == OP_DEBUGTEXT) DebugRun(Unbox(arg), prog);
}

static bool InstallScript() {
    g_origDecode = (Fn_Decode)NextInChain(A_DECODE, kDecodeBytes, sizeof kDecodeBytes);
    g_origParse  = (Fn_Parse)NextInChain(A_PARSEARGS, kParseBytes, sizeof kParseBytes);
    bool dispOrig = Same(A_DISPATCH, kDispatchBytes, sizeof kDispatchBytes);
    bool dispHooked = !dispOrig && Readable((void*)A_DISPATCH, 5) && *(uint8_t*)A_DISPATCH == 0xE9;
    if (!g_origDecode || !g_origParse || (!dispOrig && !dispHooked)) {
        Log("ERROR: script code differs - not Game.exe 1.2 or patched by an unknown mod, debug_text disabled");
        return false;
    }
    uint8_t* s = ExecAlloc(64);
    if (!s) { Log("ERROR: VirtualAlloc failed"); return false; }
    // dispatch stub (eax = instruction {opcode, arg}, ebp = the C_program)
    int k = 0;
    s[k++] = 0x81; s[k++] = 0x38; *(uint32_t*)(s + k) = OP_DEBUGTEXT; k += 4;   // cmp dword [eax], OP_DEBUGTEXT
    s[k++] = 0x74; int je = k++;                                                // je mine
    if (dispOrig) {
        s[k++] = 0x8B; s[k++] = 0x00;                                           // mov eax,[eax]
        s[k++] = 0x48;                                                          // dec eax
        s[k++] = 0x3D; *(uint32_t*)(s + k) = 0x18B; k += 4;                     // cmp eax,0x18b
        s[k++] = 0xE9; Rel32(s + k, A_DISP_JA); k += 4;                         // jmp back to ja
    } else {
        s[k++] = 0xE9; Rel32(s + k, JmpTarget(A_DISPATCH)); k += 4;             // the other mod's stub
    }
    s[je] = (uint8_t)(k - (je + 1));
    s[k++] = 0x60;                                                              // mine: pushad
    s[k++] = 0x55;                                                              // push ebp (program)
    s[k++] = 0xFF; s[k++] = 0x70; s[k++] = 0x04;                                // push dword [eax+4] (arg)
    s[k++] = 0xFF; s[k++] = 0x30;                                               // push dword [eax] (opcode)
    s[k++] = 0xE8; Rel32(s + k, (uint32_t)&ScriptRun); k += 4;                  // call ScriptRun
    s[k++] = 0x83; s[k++] = 0xC4; s[k++] = 0x0C;                                // add esp,12
    s[k++] = 0x61;                                                              // popad
    s[k++] = 0x8B; s[k++] = 0x00; s[k++] = 0x48;                                // mov eax,[eax]; dec eax
    s[k++] = 0xE9; Rel32(s + k, A_DEFAULT); k += 4;                            // jmp default (next instruction)
    if (!JmpPatch(A_DECODE, 5, (uint32_t)&DecodeHook) || !JmpPatch(A_PARSEARGS, 7, (uint32_t)&ParseHook)
        || !JmpPatch(A_DISPATCH, 8, (uint32_t)s)) { Log("ERROR: VirtualProtect failed"); return false; }
    Log(dispOrig ? "debug_text command installed" : "debug_text command installed (chained after another mod)");
    return true;
}

// ------------------------------------------------------------------ drawing
// The text is rendered with Windows GDI (Segoe UI, antialiased) into our own texture and drawn
// straight through the game's Direct3D 8 device right before the frame is presented (IGraph::Present
// hook, chain-aware - MafiaExtras / WheelInventory hook the same slot). The panel is only drawn on
// frames where the game rendered its HUD (so not in menus / loading screens).
typedef void (THISC *Fn_HudRender)(void* hud);
typedef void (__stdcall *Fn_GVoid)(void* g);
typedef int  (__stdcall *Fn_GInt)(void* g);
static const uint32_t LS3DF_VT_RVA  = 0x0009D500;   // IGraph vtable
static const uint32_t LS3DF_DPL_RVA = 0x0006D640;   // DrawPrimitiveList (vtable sanity check)
static const uint32_t A_IGRAPH = 0x00647EE0;        // IGraph* (LS3DF)
static const uint32_t LS3DF_DEV_RVA = 0x001C597C;   // IDirect3DDevice8* used by LS3D
enum { VT_BEGIN = 0x34, VT_END = 0x38, VT_PRESENT = 0x3C, VT_DPL = 0x58, VT_SX = 0x70, VT_SY = 0x74 };
static Fn_GInt o_sx, o_sy;
static uint32_t g_ls3d = 0;

static void* g_lastGame = NULL;

static void* CurGame() {
    uint8_t** pm = (uint8_t**)A_MISSION;
    if (!Readable(pm, 4) || !*pm || !Readable(*pm + MISSION_GAME, 4)) return NULL;
    return *(void**)(*pm + MISSION_GAME);
}

// ---- crash guard: if drawing faults, jump back out of the draw instead of taking the whole game
// down, log where it happened and stay quiet until the next mission.
struct JmpBuf { uint32_t ebx, esi, edi, ebp, esp, eip; };
static JmpBuf g_jb;
static volatile LONG g_inDraw = 0;
static DWORD g_drawTid = 0;
static uint32_t g_faultAt = 0, g_faultAddr = 0, g_faultCode = 0;
static bool g_drawOff = false;

extern "C" int __cdecl GuardSave(JmpBuf* jb) __attribute__((returns_twice));
__asm__(
    ".text\n"
    ".globl _GuardSave\n"
    "_GuardSave:\n"
    "  movl 4(%esp), %eax\n"
    "  movl %ebx, 0(%eax)\n"
    "  movl %esi, 4(%eax)\n"
    "  movl %edi, 8(%eax)\n"
    "  movl %ebp, 12(%eax)\n"
    "  leal 4(%esp), %ecx\n"
    "  movl %ecx, 16(%eax)\n"
    "  movl (%esp), %ecx\n"
    "  movl %ecx, 20(%eax)\n"
    "  xorl %eax, %eax\n"
    "  ret\n");

static LONG CALLBACK DrawGuard(EXCEPTION_POINTERS* e) {
    if (!g_inDraw || GetCurrentThreadId() != g_drawTid) return EXCEPTION_CONTINUE_SEARCH;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION
        && code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    g_inDraw = 0;
    g_faultCode = code;
    g_faultAt = (uint32_t)e->ExceptionRecord->ExceptionAddress;
    g_faultAddr = e->ExceptionRecord->NumberParameters >= 2 ? (uint32_t)e->ExceptionRecord->ExceptionInformation[1] : 0;
    CONTEXT* c = e->ContextRecord;
    c->Ebx = g_jb.ebx; c->Esi = g_jb.esi; c->Edi = g_jb.edi; c->Ebp = g_jb.ebp;
    c->Esp = g_jb.esp; c->Eip = g_jb.eip; c->Eax = 1;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ---- text -> texture (GDI)
enum { ROW_MAX = 64, ROW_CHARS = 200 };
struct Row { int line; int w; };                  // source line index, box width in pixels
static Row g_rows[ROW_MAX];
static int g_rowN = 0, g_rowH = 0, g_texW = 0, g_texH = 0, g_builtW = 0, g_builtH = 0;
static LONG g_builtVer = 0;
static IDirect3DDevice8* g_dev = NULL;
static IDirect3DTexture8* g_tex = NULL;
static ShownLine g_draw[PANEL_MAX];               // copy taken under the lock
static int g_drawN = 0;
static wchar_t g_wbuf[ROW_MAX][ROW_CHARS];
static int g_wlen[ROW_MAX];

static int Pow2(int v) { int p = 64; while (p < v && p < 2048) p <<= 1; return p; }

// word-wrap one line into rows that fit maxW pixels
static void WrapLine(HDC dc, int line, const wchar_t* s, int n, int maxW) {
    int start = 0;
    while (start < n && g_rowN < ROW_MAX) {
        while (start < n && s[start] == L' ') ++start;
        if (start >= n) break;
        int end = n, lastFit = start;
        SIZE sz;
        // grow word by word while it fits
        int i = start;
        while (i < n) {
            int j = i; while (j < n && s[j] != L' ') ++j;     // end of this word
            GetTextExtentPoint32W(dc, s + start, j - start, &sz);
            if (sz.cx > maxW && lastFit > start) break;
            lastFit = j;
            if (sz.cx > maxW) break;                          // a single over-long word
            i = j; while (i < n && s[i] == L' ') ++i;
        }
        end = lastFit > start ? lastFit : n;
        if (end - start >= ROW_CHARS) end = start + ROW_CHARS - 1;
        Row& r = g_rows[g_rowN]; r.line = line;
        int len = end - start;
        for (int k = 0; k < len; ++k) g_wbuf[g_rowN][k] = s[start + k];
        g_wlen[g_rowN] = len;
        GetTextExtentPoint32W(dc, s + start, len, &sz);
        r.w = sz.cx;
        ++g_rowN;
        start = end;
    }
}

static bool BuildTexture(IDirect3DDevice8* dev, int scrW, int scrH) {
    float sy = scrH / 600.0f, sx = scrW / 800.0f;
    int fontPx = (int)(C.size * sy + 0.5f); if (fontPx < 8) fontPx = 8;
    int pad = (int)(C.pad * sy + 0.5f); if (pad < 1) pad = 1;
    int panelW = (int)(C.width * sx); if (panelW < 64) panelW = 64; if (panelW > 2048) panelW = 2048;
    g_rowH = fontPx + fontPx / 4 + 2 * pad;

    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return false;
    HFONT font = CreateFontW(-fontPx, 0, 0, 0, C.bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, C.fontName);
    HGDIOBJ oldFont = SelectObject(dc, font);

    // all shown lines -> rows (newest rows win if there are too many)
    g_rowN = 0;
    for (int i = 0; i < g_drawN; ++i) {
        wchar_t w[ROW_CHARS * 2];
        int n = MultiByteToWideChar(C.codePage, 0, g_draw[i].text, -1, w, ROW_CHARS * 2);
        if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, g_draw[i].text, -1, w, ROW_CHARS * 2);
        if (n > 0) --n; else n = 0;
        WrapLine(dc, i, w, n, panelW - 2 * pad);
    }
    if (g_rowN > C.maxLines) {
        int drop = g_rowN - C.maxLines;
        for (int i = drop; i < g_rowN; ++i) {
            g_rows[i - drop] = g_rows[i]; g_wlen[i - drop] = g_wlen[i];
            for (int k = 0; k < g_wlen[i]; ++k) g_wbuf[i - drop][k] = g_wbuf[i][k];
        }
        g_rowN -= drop;
    }
    for (int i = 0; i < g_rowN; ++i) { g_rows[i].w += 2 * pad; if (g_rows[i].w > panelW) g_rows[i].w = panelW; }

    int needW = Pow2(panelW), needH = Pow2(g_rowN * g_rowH);
    if (g_rowN * g_rowH > 2048) g_rowN = 2048 / g_rowH;
    bool ok = g_rowN > 0;
    if (ok && (!g_tex || needW != g_texW || needH != g_texH)) {
        if (g_tex) { g_tex->Release(); g_tex = NULL; }
        if (FAILED(dev->CreateTexture(needW, needH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex))) {
            g_tex = NULL; ok = false; Log("ERROR: CreateTexture failed");
        } else { g_texW = needW; g_texH = needH; }
    }
    if (ok) {
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = g_texW; bi.bmiHeader.biHeight = -g_texH;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!bmp || !bits) ok = false;
        else {
            HGDIOBJ oldBmp = SelectObject(dc, bmp);
            memset(bits, 0, (size_t)g_texW * g_texH * 4);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
            for (int i = 0; i < g_rowN; ++i)
                TextOutW(dc, pad, i * g_rowH + pad + fontPx / 8, g_wbuf[i], g_wlen[i]);
            GdiFlush();
            D3DLOCKED_RECT lr;
            if (FAILED(g_tex->LockRect(0, &lr, NULL, 0))) ok = false;
            else {
                uint32_t col = C.colour, bg = (uint32_t)(C.background * 255.0f + 0.5f); if (bg > 255) bg = 255;
                uint32_t cr = (col >> 16) & 255, cg = (col >> 8) & 255, cb = col & 255;
                for (int y = 0; y < g_texH; ++y) {
                    uint32_t* dst = (uint32_t*)((uint8_t*)lr.pBits + y * lr.Pitch);
                    const uint32_t* src = (const uint32_t*)bits + y * g_texW;
                    int row = y / g_rowH, inRow = y - row * g_rowH;
                    int boxW = (row < g_rowN && inRow < g_rowH - 1) ? g_rows[row].w : 0;   // 1 px gap between boxes
                    for (int x = 0; x < g_texW; ++x) {
                        uint32_t s = src[x];
                        uint32_t r = (s >> 16) & 255, g = (s >> 8) & 255, b = s & 255;
                        uint32_t cov = r > g ? r : g; if (b > cov) cov = b;            // text coverage 0..255
                        uint32_t ba = x < boxW ? bg : 0;
                        uint32_t a = cov + ba * (255 - cov) / 255;                     // text over black box
                        uint32_t o = 0;
                        if (a) {
                            uint32_t k = cov * 255 / a;                                // colour share of the text
                            o = (a << 24) | ((cr * k / 255) << 16) | ((cg * k / 255) << 8) | (cb * k / 255);
                        }
                        dst[x] = o;
                    }
                }
                g_tex->UnlockRect(0);
            }
            SelectObject(dc, oldBmp);
            DeleteObject(bmp);
        }
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

struct TVtx { float x, y, z, rhw; DWORD diffuse; float u, v; };
static const DWORD TVTX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
static TVtx g_tv[ROW_MAX * 6];

static void DrawRows(IDirect3DDevice8* dev, int scrW, int scrH) {
    float sx = scrW / 800.0f, sy = scrH / 600.0f;
    float x0 = (float)(int)(C.x * sx) - 0.5f, y0 = (float)(int)(C.y * sy) - 0.5f;
    DWORD now = GameClock();
    int nv = 0;
    for (int i = 0; i < g_rowN; ++i) {
        const Row& r = g_rows[i];
        float a = 1.0f;
        if (C.showMs > 0 && r.line < g_drawN) {
            DWORD age = now - g_draw[r.line].t;
            if (age > (DWORD)C.showMs) a = C.fadeMs ? 1.0f - (float)(age - C.showMs) / (float)C.fadeMs : 0.0f;
        }
        if (a <= 0.0f) continue; if (a > 1.0f) a = 1.0f;
        DWORD d = ((DWORD)(a * 255.0f) << 24) | 0xFFFFFF;
        float l = x0, t = y0 + (float)(i * g_rowH), rr = l + (float)r.w, b = t + (float)g_rowH;
        float u1 = (float)r.w / g_texW, v0 = (float)(i * g_rowH) / g_texH, v1 = (float)((i + 1) * g_rowH) / g_texH;
        TVtx q[4] = { { l, t, 0, 1, d, 0, v0 }, { rr, t, 0, 1, d, u1, v0 }, { rr, b, 0, 1, d, u1, v1 }, { l, b, 0, 1, d, 0, v1 } };
        g_tv[nv++] = q[0]; g_tv[nv++] = q[1]; g_tv[nv++] = q[2];
        g_tv[nv++] = q[0]; g_tv[nv++] = q[2]; g_tv[nv++] = q[3];
    }
    if (!nv) return;

    // save exactly what we touch, draw, put it back (LS3D caches render states)
    static const D3DRENDERSTATETYPE rs[] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ZENABLE,
        D3DRS_ZWRITEENABLE, D3DRS_CULLMODE, D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_ALPHATESTENABLE,
        D3DRS_STENCILENABLE, D3DRS_FILLMODE, D3DRS_SPECULARENABLE };
    static const DWORD rsVal[] = { TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA, D3DZB_FALSE,
        FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, FALSE, D3DFILL_SOLID, FALSE };
    static const D3DTEXTURESTAGESTATETYPE ts[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
        D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_MAGFILTER, D3DTSS_MINFILTER, D3DTSS_MIPFILTER,
        D3DTSS_ADDRESSU, D3DTSS_ADDRESSV, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS };
    static const DWORD tsVal[] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE,
        D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE,
        D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0, D3DTTFF_DISABLE };
    const int NRS = sizeof rs / sizeof rs[0], NTS = sizeof ts / sizeof ts[0];
    DWORD rsOld[NRS], tsOld[NTS], ts1Color = 0, ts1Alpha = 0, vs = 0;
    for (int i = 0; i < NRS; ++i) dev->GetRenderState(rs[i], &rsOld[i]);
    for (int i = 0; i < NTS; ++i) dev->GetTextureStageState(0, ts[i], &tsOld[i]);
    dev->GetTextureStageState(1, D3DTSS_COLOROP, &ts1Color); dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &ts1Alpha);
    dev->GetVertexShader(&vs);
    IDirect3DBaseTexture8* oldTex = NULL; dev->GetTexture(0, &oldTex);
    // DrawPrimitiveUP clears stream 0 - put back what LS3D had bound
    IDirect3DVertexBuffer8* oldVB = NULL; UINT oldStride = 0; dev->GetStreamSource(0, &oldVB, &oldStride);
    IDirect3DIndexBuffer8* oldIB = NULL; UINT oldBase = 0; dev->GetIndices(&oldIB, &oldBase);

    for (int i = 0; i < NRS; ++i) dev->SetRenderState(rs[i], rsVal[i]);
    for (int i = 0; i < NTS; ++i) dev->SetTextureStageState(0, ts[i], tsVal[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetVertexShader(TVTX_FVF);
    dev->SetTexture(0, g_tex);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, nv / 3, g_tv, sizeof(TVtx));

    dev->SetTexture(0, oldTex); if (oldTex) oldTex->Release();
    dev->SetStreamSource(0, oldVB, oldStride); if (oldVB) oldVB->Release();
    dev->SetIndices(oldIB, oldBase); if (oldIB) oldIB->Release();
    dev->SetVertexShader(vs);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, ts1Color); dev->SetTextureStageState(1, D3DTSS_ALPHAOP, ts1Alpha);
    for (int i = 0; i < NTS; ++i) dev->SetTextureStageState(0, ts[i], tsOld[i]);
    for (int i = 0; i < NRS; ++i) dev->SetRenderState(rs[i], rsOld[i]);
}

static void DrawPanel(void* g) {
    void* game = CurGame();
    if (game != g_lastGame) {                     // mission changed / left - old lines are gone
        EnterCriticalSection(&g_cs); g_shownN = 0; LeaveCriticalSection(&g_cs);
        InterlockedIncrement(&g_ver);
        g_lastGame = game;
        g_drawOff = false;
    }
    if (!game || g_drawOff) return;
    IDirect3DDevice8** pdev = (IDirect3DDevice8**)(g_ls3d + LS3DF_DEV_RVA);
    if (!Readable(pdev, 4) || !*pdev) return;
    IDirect3DDevice8* dev = *pdev;
    if (dev != g_dev) {                           // device (re)created - our texture belongs to the old one
        if (g_tex && g_dev) g_tex->Release();
        g_tex = NULL; g_dev = dev; g_builtVer = 0;
    }
    int w = o_sx(g), h = o_sy(g);
    if (w < 64 || h < 64) return;

    DWORD now = GameClock();
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_shownN; ++i)            // clock went back (mission restarted) - restart the line
        if ((LONG)(now - g_shown[i].t) < 0) g_shown[i].t = now;
    if (C.showMs > 0) {                           // drop expired lines (oldest are first)
        int drop = 0;
        while (drop < g_shownN && now - g_shown[drop].t > (DWORD)(C.showMs + C.fadeMs)) ++drop;
        if (drop) { for (int i = drop; i < g_shownN; ++i) g_shown[i - drop] = g_shown[i]; g_shownN -= drop; InterlockedIncrement(&g_ver); }
    }
    LONG ver = g_ver;
    bool rebuild = ver != g_builtVer || w != g_builtW || h != g_builtH;
    if (rebuild) { for (g_drawN = 0; g_drawN < g_shownN; ++g_drawN) g_draw[g_drawN] = g_shown[g_drawN]; }
    LeaveCriticalSection(&g_cs);
    if (!g_drawN) { g_builtVer = ver; g_rowN = 0; return; }

    uint16_t fpucw; __asm__ volatile("fnstcw %0" : "=m"(fpucw));
    g_drawTid = GetCurrentThreadId();
    if (GuardSave(&g_jb) == 0) {
        g_inDraw = 1;
        if (rebuild) {
            if (!BuildTexture(dev, w, h)) g_rowN = 0;
            g_builtVer = ver; g_builtW = w; g_builtH = h;
        }
        if (g_rowN && g_tex) DrawRows(dev, w, h);   // already inside the game's scene
        g_inDraw = 0;
    } else {                                      // came back from DrawGuard - the draw faulted
        __asm__ volatile("fninit\n\tfldcw %0" : : "m"(fpucw));
        g_drawOff = true;
        EnterCriticalSection(&g_cs); g_shownN = 0; LeaveCriticalSection(&g_cs);
        char l[160]; l[0] = 0;
        SCat(l, sizeof l, "WARNING: drawing faulted (code "); SCatX(l, sizeof l, g_faultCode);
        SCat(l, sizeof l, " at "); SCatX(l, sizeof l, g_faultAt);
        SCat(l, sizeof l, ", address "); SCatX(l, sizeof l, g_faultAddr);
        SCat(l, sizeof l, ") - panel off until the next mission, game kept running");
        Log(l);
    }
}

// The game's 2D render callback (0x5BD8D0) draws the HUD and right after it the open menu
// ([0x6BD890] -> 0x5EA7E0), the ESC menu included (its modal loop renders the same callback).
// Drawing the panel straight after the HUD puts it above the HUD but under every .mnu GUI.
static void THISC HudRenderHook(void* hud) {
    ((Fn_HudRender)A_HUD_RENDER)(hud);
    void** pg = (void**)A_IGRAPH;
    if (g_dtShow && !IsBadReadPtr(pg, 4) && *pg) DrawPanel(*pg);
}

static bool InitDraw() {
    g_ls3d = (uint32_t)GetModuleHandleA("LS3DF.dll");
    if (!g_ls3d) { Log("ERROR: LS3DF.dll not loaded, panel disabled"); return false; }
    void** vt = (void**)(g_ls3d + LS3DF_VT_RVA);
    if (IsBadReadPtr(vt, 0x80) || vt[VT_DPL / 4] != (void*)(g_ls3d + LS3DF_DPL_RVA)) {
        Log("ERROR: IGraph vtable not where expected (other LS3DF.dll version?), panel disabled"); return false;
    }
    o_sx = (Fn_GInt)vt[VT_SX / 4]; o_sy = (Fn_GInt)vt[VT_SY / 4];
    return true;
}
#ifdef DT_EMBED
// host (DeveloperTools) owns the HUD call and calls this right after the HUD
static bool g_dtOn = false;                       // script hook installed
static int  g_drawInit = 0;                       // 0 = not yet, 1 = ok, -1 = failed
extern "C" void DT_AfterHud() {
    if (!g_dtOn) return;
    if (!g_drawInit) {
        if (!GetModuleHandleA("LS3DF.dll")) return;   // too early, try next frame
        g_drawInit = InitDraw() ? 1 : -1;
        if (g_drawInit > 0) Log("panel draw ready (Segoe UI via Direct3D 8, under the menus)");
    }
    if (g_drawInit < 0 || !g_dtShow) return;
    void** pg = (void**)A_IGRAPH;
    if (!IsBadReadPtr(pg, 4) && *pg) DrawPanel(*pg);
}
#else
static bool InstallDraw() {
    uint8_t* p = (uint8_t*)A_HUD_CALL;
    if (!Readable(p, 5)) return false;
    if (p[0] != 0xE8 || JmpTarget(A_HUD_CALL) != A_HUD_RENDER) {
        Log("ERROR: HUD render call differs - not Game.exe 1.2 or patched by another mod, panel disabled");
        return false;
    }
    if (!InitDraw()) return false;
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    Rel32(p + 1, (uint32_t)&HudRenderHook);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    Log("panel draw installed (Segoe UI via Direct3D 8, under the menus)");
    return true;
}
#endif

// ------------------------------------------------------------------ entry
#ifdef DT_EMBED
// embedded in developer_tools.asi: 'base' = host path without extension -> settings in <base>.ini
// ([DebugText] / [DebugTextPanel]), own log <base>_DebugText.log. Returns false = nothing installed.
extern "C" bool DT_Start(const char* base) {
    g_ini[0] = 0; SCat(g_ini, MAX_PATH, base); SCat(g_ini, MAX_PATH, ".ini");
    char lp[MAX_PATH]; lp[0] = 0; SCat(lp, MAX_PATH, base); SCat(lp, MAX_PATH, "_DebugText.log");
    InitializeCriticalSection(&g_cs);
    LoadCfg();
    if (C.log) g_log = CreateFileA(lp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    Log("DebugText (inside DeveloperTools) for Mafia 1.2");
    if (!InstallScript()) return false;
    AddVectoredExceptionHandler(1, DrawGuard);
    g_dtOn = true;
    return true;
}
#else
extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    char path[MAX_PATH]; DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return TRUE;
    size_t dot = n; while (dot && path[dot] != '.' && path[dot] != '\\') --dot;
    if (path[dot] != '.') dot = n;
    path[dot] = 0;
    g_ini[0] = 0; SCat(g_ini, MAX_PATH, path); SCat(g_ini, MAX_PATH, ".ini");
    char lp[MAX_PATH]; lp[0] = 0; SCat(lp, MAX_PATH, path); SCat(lp, MAX_PATH, ".log");
    InitializeCriticalSection(&g_cs);
    LoadCfg();
    if (C.log) g_log = CreateFileA(lp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    Log("DebugText for Mafia 1.2");
    if (InstallScript()) { AddVectoredExceptionHandler(1, DrawGuard); InstallDraw(); }
    return TRUE;
}
#endif
