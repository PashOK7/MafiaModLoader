// GENERATED from mafiatools.cpp (a separate wireframe + free camera plugin) - wireframe + free camera embedded in
// DeveloperTools, switched ONLY from the DEVELOPER TOOLS menu (no hotkeys, no console). Do not edit.
// mafiatools.asi - on-screen position / status console, wireframe, game-HUD hiding and a FREE CAMERA for Mafia (2002).
// Replaces mafiapos.asi and mafiawire.asi (remove those two).  No C runtime, only kernel32/user32.  No sounds.
//
// HOW
//   * LS3DF.dll draws through IDirect3DDevice8 (device pointer at LS3DF.dll+0x1C597C).  The device vtable is hooked:
//     Present (console, keys, camera update), SetTransform (fallback free camera), SetRenderState / SetTexture /
//     SetVertexShader (state tracking) and the 4 draw calls (wireframe, hiding the 2D game HUD).
//   * Player position: Game.exe 1.2 C_mission pointer 0x0063788C -> +0x24 (C_game) -> +0xE4 (player actor) -> +0x68 (frame) -> +0x40.
//   * FREE CAMERA, engine mode (default, EngineCamera=1): the camera the ENGINE renders with is moved, not only the
//     Direct3D view matrix, so world streaming (cache.bin), LOD, sector / portal visibility, traffic, sounds ... follow
//     the free camera instead of staying around Tommy.
//       C_mission+0x10 = I3D_scene, scene+0x17C = active I3D_camera.  C_game+0x4C = G_Camera: byte +0x20 freezes
//       G_Camera::Tick (the game stops driving the camera), byte +0x64 makes its next update cut instead of blend.
//       The pose is written the way G_Camera::ApplyDesiredPos writes it: local position +0x80, local 3x3 +0x50,
//       flag word +0xAC, then I3D_frame::Update (vtable +0x18); FOV through I3D_camera vtable +0x50.  LS3DF looks the
//       camera's sector up from its world position at the start of every render (I3D_scene vtable +0x54); that call
//       is hooked as well, so the pose is re-applied after cutscene playback (C_recorder writes the active camera).
//     If any of this does not verify, the old fallback is used: only the VIEW / PROJECTION matrices are replaced.
//   * WIREFRAME: LS3DF draws much of the world (lightmapped buildings, close-up / cutscene characters) from
//     ProcessVertices output (pre-transformed XYZRHW vertices) or through vertex-shader handles.  A draw counts as 2D HUD
//     only when its vertex format is pre-transformed AND depth testing is off; everything else gets wireframe.  The fill
//     mode and the texture are read back from the device right before the draw and restored exactly afterwards.
//   * While the free camera is on, DirectInput's GetDeviceState / GetDeviceData are hooked so the GAME sees no keyboard
//     or mouse input (player stands still, also works with player_lockcontrols 1); the plugin reads the mouse deltas
//     itself from that hook and the keys with GetAsyncKeyState.
//   * Console font: unscii-8-thin by Viznut (public domain), 8x8 cells drawn at an integer scale.
//
// KEYS  (see the in-game panel: hold F11)
//   F10 freecam on/off   F9 hide / show the whole Mafia Tools UI (HideUIKey)   Num/ freecam info panel
//   Num1 game HUD on/off   F7 wireframe   F6 wireframe textures   F11 (hold) shortcuts   F12 write position to the log
//   Esc leaves the freecam
//   Freecam: mouse = look, W/S A/D = move, Space/LCtrl = up/down, LShift = x3, wheel = speed, Q/E = tilt, R = reset tilt,
//            Num +/- = FOV, Num * = reset FOV, Num 8/2 4/6 7/9 = step move / rotate, Num 5 = mode, Num . = precision
// Settings: mafiatools.ini.   Log: mafiatools.log.
// Build (clang via zig, no CRT):
//   zig c++ -target x86-windows-gnu -mcpu=pentium4 -O2 -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin
//           -c mafiatools.cpp -o mafiatools.o
//   zig c++ -target x86-windows-gnu -shared -nostdlib -s -Wl,--entry=DllMain@12 mafiatools.o -lkernel32 -luser32
//           -o mafiatools.asi
//   (two steps: -nostdlib on the compile step would also drop the Windows headers)

#include <stdint.h>
#include <stddef.h>
#ifndef UNIT_TEST
#include <windows.h>
#endif

#ifdef _MSC_VER
#define STDCALL __stdcall
#else
#define STDCALL __attribute__((stdcall))
#endif

extern "C" { volatile int g_mtWire = 0, g_mtFreecam = 0, g_mtBypass = 0, g_mtMenuOpen = 0; volatile float g_mtFov = 0.0f; }
// ---------------------------------------------------------------------------------------------- small helpers / math (x87)
static size_t str_len(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
static void Append(char* out, size_t cap, const char* s) { size_t n = str_len(out); while (*s && n + 1 < cap) out[n++] = *s++; out[n] = 0; }
static float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline float f_sin(float x) { float r; __asm__ __volatile__("fsin" : "=t"(r) : "0"(x)); return r; }
static inline float f_cos(float x) { float r; __asm__ __volatile__("fcos" : "=t"(r) : "0"(x)); return r; }
static inline float f_sqrt(float x) { float r; __asm__ __volatile__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static inline float f_atan2(float y, float x) { float r; __asm__ __volatile__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static const float kPi = 3.14159265f;
static const float kDeg = 57.2957795f;

// float -> text with `dec` decimals (0..5); drops decimals when the value would not fit 32 bits
static void FmtN(char* out, float v, int dec) {
    if (v != v) { out[0] = 'n'; out[1] = 'a'; out[2] = 'n'; out[3] = 0; return; }
    if (v > 4e7f || v < -4e7f) { out[0] = 'b'; out[1] = 'i'; out[2] = 'g'; out[3] = 0; return; }
    if (dec < 0) dec = 0;
    if (dec > 5) dec = 5;
    bool neg = v < 0; if (neg) v = -v;
    float scale = 1.0f; uint32_t sc = 1;
    for (int i = 0; i < dec; ++i) { scale *= 10.0f; sc *= 10; }
    while (dec > 0 && v * scale > 4.0e9f) { --dec; scale = 1.0f; sc = 1; for (int i = 0; i < dec; ++i) { scale *= 10.0f; sc *= 10; } }
    uint32_t x = (uint32_t)(v * scale + 0.5f);
    int n = 0;
    if (neg && x != 0) out[n++] = '-';
    uint32_t ip = x / sc, fp = x % sc;
    char tmp[16]; int t = 0;
    if (ip == 0) tmp[t++] = '0';
    while (ip > 0) { tmp[t++] = (char)('0' + (int)(ip % 10)); ip /= 10; }
    while (t) out[n++] = tmp[--t];
    if (dec > 0) {
        out[n++] = '.';
        char fr[8]; for (int i = dec - 1; i >= 0; --i) { fr[i] = (char)('0' + (int)(fp % 10)); fp /= 10; }
        for (int i = 0; i < dec; ++i) out[n++] = fr[i];
    }
    out[n] = 0;
}

static void LogS(const char* s);
static bool Readable(const void* p, uint32_t n);
static uint32_t NowMs();
static bool KeyDown(int vk);
static bool Focused();
static void Patch(void** vt, int slot, void* hook, void** orig);

// ---------------------------------------------------------------------------------------------- 8x8 console font
// unscii-8-thin by Viznut (public domain), ASCII 32..126, one byte per row, bit 7 = leftmost pixel.
static const uint8_t kFont8[95][8] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },  //  
    { 0x10,0x10,0x10,0x10,0x00,0x00,0x10,0x00 },  // !
    { 0x24,0x24,0x24,0x00,0x00,0x00,0x00,0x00 },  // "
    { 0x24,0x24,0x7E,0x24,0x7E,0x24,0x24,0x00 },  // #
    { 0x10,0x3C,0x50,0x38,0x14,0x78,0x10,0x00 },  // $
    { 0x00,0x62,0x64,0x08,0x10,0x26,0x46,0x00 },  // %
    { 0x30,0x48,0x48,0x30,0x4A,0x44,0x3A,0x00 },  // &
    { 0x08,0x10,0x20,0x00,0x00,0x00,0x00,0x00 },  // quote
    { 0x08,0x10,0x20,0x20,0x20,0x10,0x08,0x00 },  // (
    { 0x20,0x10,0x08,0x08,0x08,0x10,0x20,0x00 },  // )
    { 0x10,0x54,0x38,0x10,0x38,0x54,0x10,0x00 },  // *
    { 0x00,0x10,0x10,0x7C,0x10,0x10,0x00,0x00 },  // +
    { 0x00,0x00,0x00,0x00,0x00,0x08,0x08,0x10 },  // ,
    { 0x00,0x00,0x00,0x00,0x7C,0x00,0x00,0x00 },  // -
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00 },  // .
    { 0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x00 },  // /
    { 0x38,0x44,0x4C,0x54,0x64,0x44,0x38,0x00 },  // 0
    { 0x10,0x30,0x50,0x10,0x10,0x10,0x7C,0x00 },  // 1
    { 0x38,0x44,0x04,0x08,0x30,0x40,0x7C,0x00 },  // 2
    { 0x38,0x44,0x04,0x18,0x04,0x44,0x38,0x00 },  // 3
    { 0x08,0x18,0x28,0x48,0x7C,0x08,0x08,0x00 },  // 4
    { 0x7C,0x40,0x78,0x04,0x04,0x44,0x38,0x00 },  // 5
    { 0x1C,0x20,0x40,0x78,0x44,0x44,0x38,0x00 },  // 6
    { 0x7C,0x04,0x08,0x10,0x20,0x20,0x20,0x00 },  // 7
    { 0x38,0x44,0x44,0x38,0x44,0x44,0x38,0x00 },  // 8
    { 0x38,0x44,0x44,0x3C,0x04,0x08,0x70,0x00 },  // 9
    { 0x00,0x00,0x10,0x00,0x00,0x10,0x00,0x00 },  // :
    { 0x00,0x00,0x10,0x00,0x00,0x10,0x10,0x20 },  // ;
    { 0x08,0x10,0x20,0x40,0x20,0x10,0x08,0x00 },  // <
    { 0x00,0x00,0x7C,0x00,0x7C,0x00,0x00,0x00 },  // =
    { 0x20,0x10,0x08,0x04,0x08,0x10,0x20,0x00 },  // >
    { 0x3C,0x42,0x04,0x08,0x08,0x00,0x08,0x00 },  // ?
    { 0x3C,0x42,0x4A,0x56,0x4C,0x40,0x3C,0x00 },  // @
    { 0x18,0x24,0x42,0x7E,0x42,0x42,0x42,0x00 },  // A
    { 0x7C,0x42,0x42,0x7C,0x42,0x42,0x7C,0x00 },  // B
    { 0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00 },  // C
    { 0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00 },  // D
    { 0x7E,0x40,0x40,0x78,0x40,0x40,0x7E,0x00 },  // E
    { 0x7E,0x40,0x40,0x78,0x40,0x40,0x40,0x00 },  // F
    { 0x3C,0x42,0x40,0x4E,0x42,0x42,0x3C,0x00 },  // G
    { 0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00 },  // H
    { 0x38,0x10,0x10,0x10,0x10,0x10,0x38,0x00 },  // I
    { 0x04,0x04,0x04,0x04,0x04,0x44,0x38,0x00 },  // J
    { 0x42,0x44,0x48,0x70,0x48,0x44,0x42,0x00 },  // K
    { 0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00 },  // L
    { 0x42,0x66,0x5A,0x42,0x42,0x42,0x42,0x00 },  // M
    { 0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00 },  // N
    { 0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00 },  // O
    { 0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00 },  // P
    { 0x3C,0x42,0x42,0x42,0x4A,0x44,0x3A,0x00 },  // Q
    { 0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00 },  // R
    { 0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00 },  // S
    { 0x7C,0x10,0x10,0x10,0x10,0x10,0x10,0x00 },  // T
    { 0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00 },  // U
    { 0x42,0x42,0x42,0x42,0x42,0x24,0x18,0x00 },  // V
    { 0x42,0x42,0x42,0x42,0x5A,0x66,0x42,0x00 },  // W
    { 0x42,0x42,0x24,0x18,0x24,0x42,0x42,0x00 },  // X
    { 0x44,0x44,0x44,0x38,0x10,0x10,0x10,0x00 },  // Y
    { 0x7C,0x04,0x08,0x10,0x20,0x40,0x7C,0x00 },  // Z
    { 0x38,0x20,0x20,0x20,0x20,0x20,0x38,0x00 },  // [
    { 0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x00 },  // backslash
    { 0x38,0x08,0x08,0x08,0x08,0x08,0x38,0x00 },  // ]
    { 0x10,0x28,0x44,0x00,0x00,0x00,0x00,0x00 },  // ^
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7C },  // _
    { 0x20,0x10,0x08,0x00,0x00,0x00,0x00,0x00 },  // `
    { 0x00,0x00,0x38,0x04,0x3C,0x44,0x3C,0x00 },  // a
    { 0x40,0x40,0x78,0x44,0x44,0x44,0x78,0x00 },  // b
    { 0x00,0x00,0x38,0x44,0x40,0x44,0x38,0x00 },  // c
    { 0x04,0x04,0x3C,0x44,0x44,0x44,0x3C,0x00 },  // d
    { 0x00,0x00,0x38,0x44,0x7C,0x40,0x38,0x00 },  // e
    { 0x0C,0x12,0x10,0x7C,0x10,0x10,0x10,0x00 },  // f
    { 0x00,0x00,0x3C,0x44,0x44,0x3C,0x04,0x38 },  // g
    { 0x40,0x40,0x78,0x44,0x44,0x44,0x44,0x00 },  // h
    { 0x10,0x00,0x30,0x10,0x10,0x10,0x38,0x00 },  // i
    { 0x04,0x00,0x0C,0x04,0x04,0x04,0x24,0x18 },  // j
    { 0x40,0x40,0x44,0x48,0x70,0x48,0x44,0x00 },  // k
    { 0x30,0x10,0x10,0x10,0x10,0x10,0x38,0x00 },  // l
    { 0x00,0x00,0x68,0x54,0x54,0x54,0x54,0x00 },  // m
    { 0x00,0x00,0x78,0x44,0x44,0x44,0x44,0x00 },  // n
    { 0x00,0x00,0x38,0x44,0x44,0x44,0x38,0x00 },  // o
    { 0x00,0x00,0x78,0x44,0x44,0x78,0x40,0x40 },  // p
    { 0x00,0x00,0x3C,0x44,0x44,0x3C,0x04,0x04 },  // q
    { 0x00,0x00,0x5C,0x60,0x40,0x40,0x40,0x00 },  // r
    { 0x00,0x00,0x3C,0x40,0x38,0x04,0x78,0x00 },  // s
    { 0x20,0x20,0x78,0x20,0x20,0x24,0x18,0x00 },  // t
    { 0x00,0x00,0x44,0x44,0x44,0x44,0x38,0x00 },  // u
    { 0x00,0x00,0x44,0x44,0x44,0x28,0x10,0x00 },  // v
    { 0x00,0x00,0x44,0x54,0x54,0x54,0x28,0x00 },  // w
    { 0x00,0x00,0x44,0x28,0x10,0x28,0x44,0x00 },  // x
    { 0x00,0x00,0x44,0x44,0x44,0x3C,0x04,0x38 },  // y
    { 0x00,0x00,0x7C,0x08,0x10,0x20,0x7C,0x00 },  // z
    { 0x18,0x20,0x20,0x40,0x20,0x20,0x18,0x00 },  // {
    { 0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10 },  // |
    { 0x30,0x08,0x08,0x04,0x08,0x08,0x30,0x00 },  // }
    { 0x20,0x54,0x08,0x00,0x00,0x00,0x00,0x00 },  // ~
};

// every glyph as a few solid rectangles (horizontal runs merged downwards) - built once
struct GRect { uint8_t x, y, w, h; };
static GRect g_gr[95][12];
static uint8_t g_grn[95];
static bool g_grReady;
static void BuildGlyphRects() {
    for (int g = 0; g < 95; ++g) {
        uint8_t used[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        int n = 0;
        for (int y = 0; y < 8; ++y) {
            int x = 0;
            while (x < 8) {
                uint8_t bit = (uint8_t)(0x80u >> x);
                if (!(kFont8[g][y] & bit) || (used[y] & bit)) { ++x; continue; }
                int x1 = x; uint8_t mask = 0;
                while (x1 < 8 && (kFont8[g][y] & (0x80u >> x1)) && !(used[y] & (0x80u >> x1))) { mask = (uint8_t)(mask | (0x80u >> x1)); ++x1; }
                int h = 1;
                while (y + h < 8 && (kFont8[g][y + h] & mask) == mask && (used[y + h] & mask) == 0) ++h;
                for (int k = 0; k < h; ++k) used[y + k] = (uint8_t)(used[y + k] | mask);
                if (n < 12) { g_gr[g][n].x = (uint8_t)x; g_gr[g][n].y = (uint8_t)y; g_gr[g][n].w = (uint8_t)(x1 - x); g_gr[g][n].h = (uint8_t)h; ++n; }
                x = x1;
            }
        }
        g_grn[g] = (uint8_t)n;
    }
    g_grReady = true;
}

struct Vtx { float x, y, z, rhw; uint32_t color; };
enum { MAXV = 150000 };
static Vtx g_v[MAXV];
static int g_nv;

static void Rect(float x, float y, float w, float h, uint32_t col) {
    if (g_nv + 6 > MAXV) return;
    Vtx a = { x, y, 0.0f, 1.0f, col }, b = { x + w, y, 0.0f, 1.0f, col }, c = { x, y + h, 0.0f, 1.0f, col }, d = { x + w, y + h, 0.0f, 1.0f, col };
    g_v[g_nv++] = a; g_v[g_nv++] = b; g_v[g_nv++] = c; g_v[g_nv++] = b; g_v[g_nv++] = d; g_v[g_nv++] = c;
}

static float g_fs = 2.0f;      // size of one font pixel on screen (integer)
static void Glyphs(float x, float y, const char* s, uint32_t col) {
    for (const char* p = s; *p; ++p, x += 8.0f * g_fs) {
        unsigned c = (unsigned char)*p;
        if (c <= 32 || c >= 127) continue;
        const GRect* r = g_gr[c - 32];
        for (int i = 0; i < g_grn[c - 32]; ++i) Rect(x + r[i].x * g_fs, y + r[i].y * g_fs, r[i].w * g_fs, r[i].h * g_fs, col);
    }
}

// console lines: text assembled at fixed character columns, drawn in one go
enum { MAXL = 40, LINEW = 112 };
static char g_ln[MAXL][LINEW];
static uint32_t g_lc[MAXL];
static int g_nl;
static char* Ln(uint32_t col) { if (g_nl >= MAXL) return NULL; g_lc[g_nl] = col; g_ln[g_nl][0] = 0; return g_ln[g_nl++]; }
static void Add(char* o, const char* s) { if (o) Append(o, LINEW, s); }
static void L(const char* s, uint32_t col) { Add(Ln(col), s); }
static void AddN(char* o, float v, int dec) { char t[32]; FmtN(t, v, dec); Add(o, t); }
static void Tab(char* o, int col) {        // pad to the column (at least one space)
    if (!o) return;
    size_t n = str_len(o);
    if ((int)n >= col) { if (n + 1 < LINEW) { o[n++] = ' '; o[n] = 0; } return; }
    while ((int)n < col && n + 1 < LINEW) o[n++] = ' ';
    o[n] = 0;
}
static void FlushLines(float x, float y, float lineH, bool box, uint32_t boxCol, bool shadow) {
    if (!g_nl) return;
    size_t w = 0;
    for (int i = 0; i < g_nl; ++i) { size_t n = str_len(g_ln[i]); if (n > w) w = n; }
    if (box) {
        float pad = 3.0f * g_fs;
        Rect(x - pad, y - pad, (float)w * 8.0f * g_fs + 2.0f * pad, (float)(g_nl - 1) * lineH + 8.0f * g_fs + 2.0f * pad, boxCol);
    }
    if (shadow) for (int i = 0; i < g_nl; ++i) Glyphs(x + g_fs, y + (float)i * lineH + g_fs, g_ln[i], 0xA0000000u);
    for (int i = 0; i < g_nl; ++i) Glyphs(x, y + (float)i * lineH, g_ln[i], g_lc[i]);
}

// ---------------------------------------------------------------------------------------------- D3D8 / DirectInput glue
enum { VT_TESTCOOP = 3, VT_RESET = 14, VT_PRESENT = 15, VT_GETBACKBUFFER = 16, VT_BEGINSCENE = 34, VT_ENDSCENE = 35, VT_SETTRANSFORM = 37,
       VT_SETRS = 50, VT_GETRS = 51, VT_APPLYSB = 54, VT_CAPTURESB = 55, VT_DELETESB = 56, VT_CREATESB = 57, VT_GETTEX = 60, VT_SETTEX = 61,
       VT_SETTSS = 63, VT_DRAWP = 70, VT_DRAWIP = 71, VT_DRAWUP = 72, VT_DRAWIUP = 73, VT_SETVS = 76, VT_GETVS = 77 };
enum { RS_ZENABLE = 7, RS_FILLMODE = 8, RS_ZWRITE = 14, RS_ALPHATEST = 15, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULL = 22, RS_ALPHABLEND = 27,
       RS_FOG = 28, RS_SPECULAR = 29, RS_STENCIL = 52, RS_LIGHTING = 137, RS_COLORWRITE = 168 };
enum { FVF_TL_DIFFUSE = 0x044 };

typedef long (STDCALL *fnRelease)(void*);
typedef long (STDCALL *fn1)(void*);
typedef long (STDCALL *fnObjF)(void*, float);
typedef long (STDCALL *fnReset)(void*, void*);
typedef long (STDCALL *fnPresent)(void*, const void*, const void*, void*, const void*);
typedef long (STDCALL *fnGetBB)(void*, uint32_t, uint32_t, void**);
typedef long (STDCALL *fnGetDesc)(void*, uint32_t*);
typedef long (STDCALL *fnSetXf)(void*, uint32_t, const float*);
typedef long (STDCALL *fnSetRS)(void*, uint32_t, uint32_t);
typedef long (STDCALL *fnGetRS)(void*, uint32_t, uint32_t*);
typedef long (STDCALL *fnSB)(void*, uint32_t);
typedef long (STDCALL *fnCreateSB)(void*, uint32_t, uint32_t*);
typedef long (STDCALL *fnSetTex)(void*, uint32_t, void*);
typedef long (STDCALL *fnGetTex)(void*, uint32_t, void**);
typedef long (STDCALL *fnSetTSS)(void*, uint32_t, uint32_t, uint32_t);
typedef long (STDCALL *fnSetVS)(void*, uint32_t);
typedef long (STDCALL *fnGetVS)(void*, uint32_t*);
typedef long (STDCALL *fnDrawP)(void*, uint32_t, uint32_t, uint32_t);
typedef long (STDCALL *fnDrawIP)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef long (STDCALL *fnDrawUP)(void*, uint32_t, uint32_t, const void*, uint32_t);
typedef long (STDCALL *fnDrawIUP)(void*, uint32_t, uint32_t, uint32_t, uint32_t, const void*, uint32_t, const void*, uint32_t);
static inline void** VT(void* o) { return *(void***)o; }
#define CALLV(type, obj, slot) ((type)VT(obj)[slot])

static void* oReset; static void* oPresent; static void* oSetXf; static void* oSetRS; static void* oSetTex; static void* oSetVS;
static void* oDrawP; static void* oDrawIP; static void* oDrawUP; static void* oDrawIUP;
static void* oScene54;

// ---------------------------------------------------------------------------------------------- state
struct Cfg { bool overlay; float px, py, scale, speed, sens; int keyUi; bool shadow, bg; int lineGap; bool engineCam; };
static Cfg g_cfg = { true, 10.0f, 10.0f, 0.0f, 10.0f, 0.15f, 0x78, true, false, 0, true };

static bool g_wire = false, g_wireTex = false, g_hideHud = false, g_fc = false, g_fcPanel = true, g_precision = false, g_modeCamera = false;
static bool g_uiHidden = false;
static bool g_inOverlay = false, g_seen3D = false;
static uint32_t g_fill = 3, g_zen = 1, g_fvf; static void* g_tex0;
static uint32_t g_sb;
static bool g_playerOk, g_havePlayer; static float g_ppos[3];
static uint32_t g_frame, g_mainFrame;

// free camera pose
static float g_cp[3], g_yaw, g_pitch, g_tilt;     // position, yaw/pitch/tilt in radians
static float g_fov = 0.0f, g_baseFov = 0.0f;      // radians (fallback mode: g_fov 0 = untouched)
static float g_speed = 10.0f;
static float g_viewMine[16];
static bool  g_haveMine;
static float g_lastMain[16]; static bool g_haveMain;       // the game's own main view matrix of the last frame
static float g_lastProj[16]; static bool g_haveProj;
static int   g_mdx, g_mdy, g_mdz;                          // mouse deltas collected by the DirectInput hook
static bool  g_diHooked;

// engine camera
static bool     g_camOk;              // Game.exe 1.2 camera layout verified
static bool     g_engCam;             // the free camera currently drives the engine camera
static uint8_t* g_gcam;               // the G_Camera that is frozen
static uint8_t  g_gcamFreeze0;        // its freeze byte before
static float    g_engFov0;            // engine FOV when the free camera started (0 = unknown)
static uintptr_t g_ls3dLo, g_ls3dHi;  // LS3DF.dll image range
static uint8_t* g_engScene;           // scene and camera the free camera took over (touch nothing else)
static uint8_t* g_engCamObj;
static bool     g_engLost;            // they were replaced (mission reload, menu camera): free camera goes off

static const uint32_t kMissionPtr = 0x0063788C;
static uint8_t* Deref(const void* slot) { if (!slot || !Readable(slot, 4)) return NULL; return *(uint8_t* const*)slot; }
static uint8_t* MissionObj() { return Deref((const void*)(uintptr_t)kMissionPtr); }
static uint8_t* GameObj() { uint8_t* m = MissionObj(); return m ? Deref(m + 0x24) : NULL; }
static uint8_t* SceneObj() { uint8_t* m = MissionObj(); uint8_t* s = m ? Deref(m + 0x10) : NULL; return (s && Readable(s, 0x180)) ? s : NULL; }

static bool ReadPlayerPos(float* out) {
    uint8_t* game = GameObj();
    if (!game) return false;
    uint8_t* actor = Deref(game + 0xE4);
    if (!actor) return false;
    uint8_t* frame = Deref(actor + 0x68);
    const float* p;
    if (frame && Readable(frame + 0x40, 12)) p = (const float*)(frame + 0x40);
    else if (Readable(actor + 0x24, 12)) p = (const float*)(actor + 0x24);
    else return false;
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
    for (int i = 0; i < 3; ++i) if (!(out[i] > -1e7f && out[i] < 1e7f)) return false;
    return true;
}

// ---------------------------------------------------------------------------------------------- camera math
static void Basis(float yaw, float pitch, float tilt, float* r, float* u, float* f) {
    float cy = f_cos(yaw), sy = f_sin(yaw), cp = f_cos(pitch), sp = f_sin(pitch), ct = f_cos(tilt), st = f_sin(tilt);
    f[0] = cp * sy; f[1] = sp; f[2] = cp * cy;
    float r0[3] = { cy, 0.0f, -sy };
    float u0[3] = { -sp * sy, cp, -sp * cy };
    for (int i = 0; i < 3; ++i) { r[i] = r0[i] * ct + u0[i] * st; u[i] = u0[i] * ct - r0[i] * st; }
}
static float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void BuildView(float* m, const float* r, const float* u, const float* f, const float* p, bool rotOnly) {
    m[0] = r[0]; m[1] = u[0]; m[2] = f[0]; m[3] = 0;
    m[4] = r[1]; m[5] = u[1]; m[6] = f[1]; m[7] = 0;
    m[8] = r[2]; m[9] = u[2]; m[10] = f[2]; m[11] = 0;
    m[12] = rotOnly ? 0.0f : -Dot3(p, r); m[13] = rotOnly ? 0.0f : -Dot3(p, u); m[14] = rotOnly ? 0.0f : -Dot3(p, f); m[15] = 1;
}
static void CamPosFromView(const float* m, float* out) {
    out[0] = -(m[12] * m[0] + m[13] * m[1] + m[14] * m[2]);
    out[1] = -(m[12] * m[4] + m[13] * m[5] + m[14] * m[6]);
    out[2] = -(m[12] * m[8] + m[13] * m[9] + m[14] * m[10]);
}
static bool IsIdentity(const float* m) {
    for (int i = 0; i < 16; ++i) { float e = (i % 5 == 0) ? 1.0f : 0.0f; float d = m[i] - e; if (d > 0.001f || d < -0.001f) return false; }
    return true;
}
static bool Orthonormal(const float* m) {      // the 3x3 columns have unit length
    for (int c = 0; c < 3; ++c) { float l = m[c] * m[c] + m[4 + c] * m[4 + c] + m[8 + c] * m[8 + c]; if (l < 0.9f || l > 1.1f) return false; }
    return true;
}
static void Quat(const float* r, const float* u, const float* f, float* q) {      // q = w,x,y,z of rows r,u,f
    float m00 = r[0], m11 = u[1], m22 = f[2];
    float tr = m00 + m11 + m22;
    if (tr > 0) { float s = f_sqrt(tr + 1.0f) * 2.0f; q[0] = 0.25f * s; q[1] = (u[2] - f[1]) / s; q[2] = (f[0] - r[2]) / s; q[3] = (r[1] - u[0]) / s; }
    else if (m00 > m11 && m00 > m22) { float s = f_sqrt(1.0f + m00 - m11 - m22) * 2.0f; q[0] = (u[2] - f[1]) / s; q[1] = 0.25f * s; q[2] = (u[0] + r[1]) / s; q[3] = (f[0] + r[2]) / s; }
    else if (m11 > m22) { float s = f_sqrt(1.0f + m11 - m00 - m22) * 2.0f; q[0] = (f[0] - r[2]) / s; q[1] = (u[0] + r[1]) / s; q[2] = 0.25f * s; q[3] = (f[1] + u[2]) / s; }
    else { float s = f_sqrt(1.0f + m22 - m00 - m11) * 2.0f; q[0] = (r[1] - u[0]) / s; q[1] = (f[0] + r[2]) / s; q[2] = (f[1] + u[2]) / s; q[3] = 0.25f * s; }
}
// bounded wraps - a loop of "d -= 360" never ends once the value is big enough to swallow the step
static float WrapPeriod(float v, float period) {
    if (!(v > -1e6f && v < 1e6f)) return 0.0f;                     // NaN / absurd input
    v -= period * (float)(int)(v / period);
    return v;
}
static float WrapDeg(float d) { d = WrapPeriod(d, 360.0f); if (d < 0) d += 360.0f; return d >= 360.0f ? 0.0f : d; }
static float WrapRad(float a) { a = WrapPeriod(a, 2.0f * kPi); if (a > kPi) a -= 2.0f * kPi; else if (a < -kPi) a += 2.0f * kPi; return a; }

// yaw / pitch / tilt + position from a camera basis (rows right, forward)
static void PoseFromBasis(const float* r, const float* f, const float* p) {
    g_cp[0] = p[0]; g_cp[1] = p[1]; g_cp[2] = p[2];
    float h = f_sqrt(f[0] * f[0] + f[2] * f[2]);
    g_yaw = f_atan2(f[0], f[2]); g_pitch = Clampf(f_atan2(f[1], h), -1.553f, 1.553f);
    if (!(g_yaw == g_yaw)) g_yaw = 0.0f;
    if (!(g_pitch == g_pitch)) g_pitch = 0.0f;
    float r0[3], u0[3], f0[3];
    Basis(g_yaw, g_pitch, 0.0f, r0, u0, f0);
    g_tilt = f_atan2(Dot3(r, u0), Dot3(r, r0));
    if (!(g_tilt > -kPi && g_tilt < kPi)) g_tilt = 0.0f;
}

// ---------------------------------------------------------------------------------------------- engine camera
static uint8_t* GCamera() { uint8_t* g = GameObj(); return (g && Readable(g + 0x4C, 0x70)) ? g + 0x4C : NULL; }
static uint8_t* ActiveCam() {
    uint8_t* s = SceneObj();
    uint8_t* c = s ? Deref(s + 0x17C) : NULL;
    if (!c || !Readable(c, 0x150) || *(const int*)(c + 0x110) != 3 /*I3D camera*/) return NULL;
    if (!Readable(VT(c), 0x54)) return NULL;
    return c;
}
// world pose -> the camera's local space (the camera normally hangs on an identity root / sector)
static void ToLocal(const uint8_t* cam, float* r, float* u, float* f, float* p) {
    const uint8_t* par = Deref(cam + 0x120);
    if (!par || !Readable(par, 0x50)) return;
    const float* P = (const float*)(par + 0x10);
    if (IsIdentity(P) || !Orthonormal(P)) return;
    float pr[3] = { P[0], P[1], P[2] }, pu[3] = { P[4], P[5], P[6] }, pf[3] = { P[8], P[9], P[10] };
    float* v[3] = { r, u, f };
    for (int k = 0; k < 3; ++k) { float a = Dot3(v[k], pr), b = Dot3(v[k], pu), c = Dot3(v[k], pf); v[k][0] = a; v[k][1] = b; v[k][2] = c; }
    float d[3] = { p[0] - P[12], p[1] - P[13], p[2] - P[14] };
    p[0] = Dot3(d, pr); p[1] = Dot3(d, pu); p[2] = Dot3(d, pf);
}
static void EngineApply() {
    uint8_t* cam = ActiveCam();
    if (!cam) return;
    if (cam != g_engCamObj || SceneObj() != g_engScene) { g_engCam = false; g_engLost = true; g_gcam = NULL; return; }
    if (g_gcam && GCamera() == g_gcam) g_gcam[0x20] = 1;          // keep G_Camera::Tick off
    float r[3], u[3], f[3], p[3] = { g_cp[0], g_cp[1], g_cp[2] };
    Basis(g_yaw, g_pitch, g_tilt, r, u, f);
    ToLocal(cam, r, u, f, p);
    { float want = g_mtFov > 0.0f ? g_mtFov : g_fov;
      if (want > 0.0f && *(const float*)(cam + 0x140) != want) ((fnObjF)VT(cam)[0x50 / 4])(cam, want); }   // I3D_camera::SetFOV
    uint32_t* fl = (uint32_t*)(cam + 0xAC);
    if (!(*fl & 0x80u)) {
        float* lp = (float*)(cam + 0x80); lp[0] = p[0]; lp[1] = p[1]; lp[2] = p[2];
        *fl = (*fl & 0xFFFFFEDFu) | 0x40000000u;                   // position changed
    }
    float* m = (float*)(cam + 0x50);
    m[0] = r[0]; m[1] = r[1]; m[2] = r[2]; m[4] = u[0]; m[5] = u[1]; m[6] = u[2]; m[8] = f[0]; m[9] = f[1]; m[10] = f[2];
    *fl = (*fl & 0xFFFFFED3u) | 0x40000010u;                       // rotation now comes from the 3x3
    ((fn1)VT(cam)[0x18 / 4])(cam);                                 // I3D_frame::Update
}
static bool EngineStart() {
    g_engCam = false;
    if (!g_camOk || !g_cfg.engineCam) return false;
    uint8_t* cam = ActiveCam(); uint8_t* gc = GCamera();
    if (!cam || !gc) { LogS("engine camera not found - using the view-matrix fallback"); return false; }
    if (!(*(const uint32_t*)(cam + 0xAC) & 0x20u)) ((fn1)VT(cam)[0x18 / 4])(cam);
    const float* w = (const float*)(cam + 0x10);                   // world matrix rows: right, up, forward, position
    float r[3] = { w[0], w[1], w[2] }, u[3] = { w[4], w[5], w[6] }, f[3] = { w[8], w[9], w[10] }, p[3] = { w[12], w[13], w[14] };
    float lr = Dot3(r, r), lu = Dot3(u, u), lf = Dot3(f, f);
    bool sane = lr > 0.9f && lr < 1.1f && lu > 0.9f && lu < 1.1f && lf > 0.9f && lf < 1.1f;
    for (int i = 0; i < 3; ++i) if (!(p[i] > -1e6f && p[i] < 1e6f)) sane = false;
    if (!sane) { LogS("engine camera matrix looks invalid - using the view-matrix fallback"); return false; }
    if (g_haveMain && g_frame - g_mainFrame <= 2) {                // same convention as the D3D view the game just set?
        float vr[3] = { g_lastMain[0], g_lastMain[4], g_lastMain[8] }, vf[3] = { g_lastMain[2], g_lastMain[6], g_lastMain[10] }, vp[3];
        CamPosFromView(g_lastMain, vp);
        float dx = vp[0] - p[0], dy = vp[1] - p[1], dz = vp[2] - p[2], dr = Dot3(vr, r), df = Dot3(vf, f), dd = dx * dx + dy * dy + dz * dz;
        if (dr < 0.95f || df < 0.95f || dd > 9.0f) {
            char s[200], t[24]; s[0] = 0;
            Append(s, sizeof s, "engine camera does not match the game's view matrix (right ");
            FmtN(t, dr, 3); Append(s, sizeof s, t); Append(s, sizeof s, ", forward "); FmtN(t, df, 3); Append(s, sizeof s, t);
            Append(s, sizeof s, ", dist^2 "); FmtN(t, dd, 2); Append(s, sizeof s, t); Append(s, sizeof s, ") - using the view-matrix fallback");
            LogS(s);
            return false;
        }
    }
    PoseFromBasis(r, f, p);
    g_engScene = SceneObj(); g_engCamObj = cam; g_engLost = false;
    g_gcam = (Deref(gc + 4) == cam) ? gc : NULL;                   // G_Camera drives exactly this camera -> freeze it
    if (g_gcam) { g_gcamFreeze0 = gc[0x20]; gc[0x20] = 1; }
    float fov = *(const float*)(cam + 0x140);
    g_engFov0 = (fov > 0.02f && fov < 3.0f) ? fov : 0.0f;
    g_fov = g_engFov0; g_baseFov = g_engFov0;
    g_engCam = true;
    return true;
}
static void EngineStop() {
    if (g_engCam) {
        uint8_t* gc = GCamera();
        if (gc && gc == g_gcam) { gc[0x20] = g_gcamFreeze0; gc[0x64] = 1; }       // G_Camera takes over again and cuts back
        uint8_t* cam = ActiveCam();
        if (cam && cam == g_engCamObj && g_engFov0 > 0.0f && *(const float*)(cam + 0x140) != g_engFov0) {
            ((fnObjF)VT(cam)[0x50 / 4])(cam, g_engFov0);
            ((fn1)VT(cam)[0x18 / 4])(cam);
        }
    }
    g_engCam = false; g_gcam = NULL; g_engCamObj = NULL; g_engScene = NULL;
}

// I3D_scene vtable +0x54: start of a scene render (camera sector lookup, view setup, BeginScene)
static long STDCALL hScene54(void* scene) {
    if (g_engCam) EngineApply();
    return ((fn1)oScene54)(scene);
}
static void TryHookScene() {
    if (oScene54 || !g_camOk || !g_cfg.engineCam || !g_ls3dLo) return;
    uint8_t* s = SceneObj();
    if (!s) return;
    void** vt = VT(s);
    if (!Readable(vt, 0x58)) return;
    uintptr_t fn = (uintptr_t)vt[0x54 / 4];
    if (fn < g_ls3dLo || fn >= g_ls3dHi) return;
    Patch(vt, 0x54 / 4, (void*)hScene54, &oScene54);
    LogS("I3D_scene render hook installed (the free camera pose is re-applied right before every render)");
}

// ---------------------------------------------------------------------------------------------- free camera
static void FreecamStart() {
    if (!EngineStart()) {
        float r[3] = { 1, 0, 0 }, f[3] = { 0, 0, 1 }, p[3] = { 0, 0, 0 };
        if (g_haveMain) {
            r[0] = g_lastMain[0]; r[1] = g_lastMain[4]; r[2] = g_lastMain[8];
            f[0] = g_lastMain[2]; f[1] = g_lastMain[6]; f[2] = g_lastMain[10];
            CamPosFromView(g_lastMain, p);
        } else if (g_havePlayer) { p[0] = g_ppos[0]; p[1] = g_ppos[1] + 2.0f; p[2] = g_ppos[2]; }
        PoseFromBasis(r, f, p);
        g_fov = 0.0f; g_baseFov = 0.0f;
        if (g_haveProj && g_lastProj[5] > 0.01f) g_baseFov = 2.0f * f_atan2(1.0f, g_lastProj[5]);
    }
    g_mdx = g_mdy = g_mdz = 0;
    char a[24], b[24], c[24], s[200]; s[0] = 0;
    FmtN(a, g_cp[0], 2); FmtN(b, g_cp[1], 2); FmtN(c, g_cp[2], 2);
    Append(s, sizeof s, g_engCam ? "freecam ON (engine camera: streaming / LOD / visibility follow it) at "
                                 : (g_haveMain ? "freecam ON (view-matrix fallback), started from the game camera at "
                                               : "freecam ON (view-matrix fallback, no game camera matrix seen) at "));
    Append(s, sizeof s, a); Append(s, sizeof s, " "); Append(s, sizeof s, b); Append(s, sizeof s, " "); Append(s, sizeof s, c);
    LogS(s);
}

static void BuildMine() {
    float r[3], u[3], f[3];
    Basis(g_yaw, g_pitch, g_tilt, r, u, f);
    BuildView(g_viewMine, r, u, f, g_cp, false);
    g_haveMine = true;
}

static uint32_t g_lastTick;
static void FreecamOn() { g_fc = true; FreecamStart(); g_lastTick = NowMs(); BuildMine(); }
static void FreecamOff(const char* why) { g_fc = false; g_haveMine = false; EngineStop(); LogS(why); }

// edge + slow auto-repeat detector for the numpad stepping
struct Rep { bool down; uint32_t next; };
static bool RepeatKey(int vk, Rep* st, uint32_t now) {
    bool d = KeyDown(vk);
    if (!d) { st->down = false; return false; }
    if (!st->down) { st->down = true; st->next = now + 350; return true; }
    if (now >= st->next) { st->next = now + 60; return true; }
    return false;
}
static bool Edge(int vk, bool* prev) { bool now = KeyDown(vk); bool hit = now && !*prev; *prev = now; return hit; }

static void FreecamUpdate() {
    uint32_t now = NowMs();
    float dt = (float)(now - g_lastTick) * 0.001f; g_lastTick = now;
    if (dt <= 0.0f || dt > 0.25f) dt = 0.016f;

    // mouse look
    g_yaw += (float)g_mdx * g_cfg.sens / kDeg;
    g_pitch -= (float)g_mdy * g_cfg.sens / kDeg;
    if (g_mdz) { int n = g_mdz / 120; float k = 1.0f; for (int i = 0; i < (n < 0 ? -n : n); ++i) k *= 1.15f; g_speed = n >= 0 ? g_speed * k : g_speed / k; g_speed = Clampf(g_speed, 0.05f, 2000.0f); }
    g_mdx = g_mdy = g_mdz = 0;
    g_pitch = Clampf(g_pitch, -1.553f, 1.553f);
    g_yaw = WrapRad(g_yaw);

    float r[3], u[3], f[3];
    Basis(g_yaw, g_pitch, g_tilt, r, u, f);

    // continuous movement
    float mult = KeyDown(0xA0 /*LSHIFT*/) ? 3.0f : 1.0f, sp = g_speed * mult * dt;
    float fw = (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f);
    float rt = (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
    float up = (KeyDown(0x20 /*SPACE*/) ? 1.0f : 0.0f) - (KeyDown(0xA2 /*LCTRL*/) ? 1.0f : 0.0f);
    for (int i = 0; i < 3; ++i) g_cp[i] += f[i] * fw * sp + r[i] * rt * sp;
    g_cp[1] += up * sp;
    BuildMine();
}

// ---------------------------------------------------------------------------------------------- hooks
static long STDCALL hSetXf(void* dev, uint32_t state, const float* m) {
    if (m && Readable(m, 64)) {
        if (state == 2) {
            bool ident = IsIdentity(m), orth = Orthonormal(m);
            bool trans0 = (m[12] < 0.001f && m[12] > -0.001f && m[13] < 0.001f && m[13] > -0.001f && m[14] < 0.001f && m[14] > -0.001f);
            bool main = false, sky = false;
            if (!ident && orth) {
                if (trans0) sky = true;
                else {
                    float cp[3]; CamPosFromView(m, cp);
                    if (g_havePlayer) { float dx = cp[0] - g_ppos[0], dy = cp[1] - g_ppos[1], dz = cp[2] - g_ppos[2]; main = (dx * dx + dy * dy + dz * dz) < 80.0f * 80.0f; }
                    else main = true;
                }
            }
            if (main) { for (int i = 0; i < 16; ++i) g_lastMain[i] = m[i]; g_haveMain = true; g_mainFrame = g_frame; }
            if (g_fc && !g_engCam && g_haveMine && !g_inOverlay) {
                if (main) return ((fnSetXf)oSetXf)(dev, state, g_viewMine);
                if (sky) { float v[16]; for (int i = 0; i < 16; ++i) v[i] = g_viewMine[i]; v[12] = v[13] = v[14] = 0.0f; return ((fnSetXf)oSetXf)(dev, state, v); }
            }
        } else if (state == 3 && m[15] == 0.0f && m[11] == 1.0f && m[5] > 0.01f) {      // perspective projection
            for (int i = 0; i < 16; ++i) g_lastProj[i] = m[i];
            g_haveProj = true;
            if (g_fc && !g_engCam && g_fov > 0.0f) {
                float p[16]; for (int i = 0; i < 16; ++i) p[i] = m[i];
                float cot = f_cos(g_fov * 0.5f) / f_sin(g_fov * 0.5f);
                p[0] = m[0] * (cot / m[5]); p[5] = cot;
                return ((fnSetXf)oSetXf)(dev, state, p);
            }
        }
    }
    return ((fnSetXf)oSetXf)(dev, state, m);
}

static long STDCALL hSetRS(void* d, uint32_t s, uint32_t v) {
    if (!g_inOverlay) { if (s == RS_FILLMODE) g_fill = v; else if (s == RS_ZENABLE) g_zen = v; }
    return ((fnSetRS)oSetRS)(d, s, v);
}
static long STDCALL hSetTex(void* d, uint32_t st, void* t) { if (st == 0) g_tex0 = t; return ((fnSetTex)oSetTex)(d, st, t); }
static long STDCALL hSetVS(void* d, uint32_t h) { g_fvf = h; return ((fnSetVS)oSetVS)(d, h); }

// current device state, read back from the runtime (falls back to the tracked value on a pure device)
static uint32_t CurVS(void* d) { uint32_t v = 0; if (((fnGetVS)VT(d)[VT_GETVS])(d, &v) >= 0) return v; return g_fvf; }
static uint32_t CurRS(void* d, uint32_t s, uint32_t dflt) { uint32_t v = 0; if (((fnGetRS)VT(d)[VT_GETRS])(d, s, &v) >= 0) return v; return dflt; }

// 2D HUD = pre-transformed vertices drawn WITHOUT depth test.  Vertex-shader handles (odd values, or high bits set by
// wrappers) and depth-tested XYZRHW geometry (LS3DF's ProcessVertices output: buildings, characters) are 3D world.
static bool Is2D(void* d) {
    uint32_t h = CurVS(d);
    if (h & 0xFFFF0001u) return false;
    if ((h & 0x00Eu) != 0x004u) return false;
    return CurRS(d, RS_ZENABLE, g_zen) == 0;
}

struct WSave { uint32_t fill; void* tex; bool texSet, texRef; };
static void WBegin(void* d, WSave* s) {
    s->fill = CurRS(d, RS_FILLMODE, g_fill);
    if (s->fill < 1 || s->fill > 3) s->fill = 3;
    ((fnSetRS)oSetRS)(d, RS_FILLMODE, 2 /*WIREFRAME*/);
    s->tex = NULL; s->texSet = false; s->texRef = false;
    if (!g_wireTex) {
        void* t = NULL;
        if (((fnGetTex)VT(d)[VT_GETTEX])(d, 0, &t) >= 0) { s->tex = t; s->texRef = t != NULL; }
        else s->tex = g_tex0;
        ((fnSetTex)oSetTex)(d, 0, NULL);
        s->texSet = true;
    }
}
static void WEnd(void* d, WSave* s) {
    ((fnSetRS)oSetRS)(d, RS_FILLMODE, s->fill);
    if (s->texSet) {
        ((fnSetTex)oSetTex)(d, 0, s->tex);
        if (s->texRef) CALLV(fnRelease, s->tex, 2)(s->tex);
    }
}

#define DRAW_BODY(CALL)                                                         \
    if (g_mtBypass || g_inOverlay || (!g_wire && !g_hideHud)) return CALL;      \
    bool is2d = Is2D(d);                                                        \
    if (!is2d) g_seen3D = true;                                                 \
    else if (g_hideHud && g_seen3D) return 0;                                   \
    if (!g_wire || is2d) return CALL;                                           \
    WSave ws; WBegin(d, &ws); long r = CALL; WEnd(d, &ws); return r;

static long STDCALL hDrawP(void* d, uint32_t a, uint32_t b, uint32_t c) {
    DRAW_BODY(((fnDrawP)oDrawP)(d, a, b, c)) }
static long STDCALL hDrawIP(void* d, uint32_t a, uint32_t b, uint32_t c, uint32_t e, uint32_t f) {
    DRAW_BODY(((fnDrawIP)oDrawIP)(d, a, b, c, e, f)) }
static long STDCALL hDrawUP(void* d, uint32_t a, uint32_t b, const void* c, uint32_t e) {
    DRAW_BODY(((fnDrawUP)oDrawUP)(d, a, b, c, e)) }
static long STDCALL hDrawIUP(void* d, uint32_t a, uint32_t b, uint32_t c, uint32_t e, const void* f, uint32_t g, const void* h, uint32_t i) {
    DRAW_BODY(((fnDrawIUP)oDrawIUP)(d, a, b, c, e, f, g, h, i)) }

// ---------------------------------------------------------------------------------------------- console
static void RS(void* d, uint32_t s, uint32_t v) { ((fnSetRS)oSetRS)(d, s, v); }
static void TS(void* d, uint32_t st, uint32_t t, uint32_t v) { CALLV(fnSetTSS, d, VT_SETTSS)(d, st, t, v); }

static const uint32_t kTxt = 0xFFF0F0F0u, kDim = 0xFFA8A8A8u, kHead = 0xFFFFFFFFu;
static const char* OnOff(bool b) { return b ? "ON" : "OFF"; }

static void KeyName(char* out, size_t cap, int vk) {
    out[0] = 0;
    char t[16]; t[0] = 0;
    if (vk >= 0x70 && vk <= 0x87) { int n = vk - 0x70 + 1; t[0] = 'F'; if (n >= 10) { t[1] = (char)('0' + n / 10); t[2] = (char)('0' + n % 10); t[3] = 0; } else { t[1] = (char)('0' + n); t[2] = 0; } }
    else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { t[0] = (char)vk; t[1] = 0; }
    else if (vk == 0x2D) Append(t, sizeof t, "Insert");
    else if (vk == 0x2E) Append(t, sizeof t, "Delete");
    else if (vk == 0x24) Append(t, sizeof t, "Home");
    else if (vk == 0x23) Append(t, sizeof t, "End");
    else if (vk == 0x21) Append(t, sizeof t, "PgUp");
    else if (vk == 0x22) Append(t, sizeof t, "PgDn");
    else if (vk == 0x13) Append(t, sizeof t, "Pause");
    else if (vk == 0x91) Append(t, sizeof t, "ScrLk");
    else if (vk == 0xC0) Append(t, sizeof t, "`");
    else { Append(t, sizeof t, "Key "); char n[12]; FmtN(n, (float)vk, 0); Append(t, sizeof t, n); }
    Append(out, cap, t);
}

static void BuildHelp() {
    char k[24]; KeyName(k, sizeof k, g_cfg.keyUi);
    char* o;
    L("=== Mafia Tools - shortcuts ===", kHead);
    L("--- General ---", kDim);
    L("F10           Freecam on/off  (Esc = off)", kTxt);
    o = Ln(kTxt); Add(o, k); Tab(o, 14); Add(o, "Hide / show the whole Mafia Tools UI");
    L("Num /         Freecam info panel on/off", kTxt);
    L("Num 1         Game HUD visible/hidden", kTxt);
    L("F7            Wireframe on/off", kTxt);
    L("F6            Textures in wireframe", kTxt);
    L("F11 (hold)    This screen", kTxt);
    L("F12           Write position to log", kTxt);
    L("--- Freecam movement ---", kDim);
    L("Mouse         Look around", kTxt);
    L("W / S         Move forward / backward", kTxt);
    L("A / D         Strafe left / right", kTxt);
    L("Space/LCtrl   Move up / down", kTxt);
    L("Wheel         Adjust move speed", kTxt);
    L("LShift        Fast mode (3x speed)", kTxt);
    L("Q / E         Tilt left / right   R = reset", kTxt);
    L("--- Numpad (always active in freecam) ---", kDim);
    L("8 / 2         Position: fwd/back     Camera: pitch", kTxt);
    L("4 / 6         Position: left/right   Camera: yaw", kTxt);
    L("7 / 9         Tilt left / right", kTxt);
    L("5             Toggle mode Position <> Camera", kTxt);
    L(". (decimal)   Precision mode", kTxt);
    L("                + RShift step 0.1   + RCtrl step 0.01", kTxt);
    L("                default step 0.001", kTxt);
    L("+ / -         FOV increase / decrease", kTxt);
    L("*             Reset FOV to default", kTxt);
}

static void BuildPanel() {
    char* o = Ln(kTxt);
    Add(o, "Player");
    if (g_havePlayer) {
        Tab(o, 9); Add(o, "X: "); AddN(o, g_ppos[0], 5); Tab(o, 27); Add(o, "Y: "); AddN(o, g_ppos[1], 5); Tab(o, 45); Add(o, "Z: "); AddN(o, g_ppos[2], 5);
    } else { Tab(o, 9); Add(o, "none"); }
    if (g_fc && g_fcPanel) {
        float r[3], u[3], f[3], q[4];
        Basis(g_yaw, g_pitch, g_tilt, r, u, f);
        Quat(r, u, f, q);
        o = Ln(kTxt); Add(o, "Pos");  Tab(o, 9); Add(o, "X: ");   AddN(o, g_cp[0], 5); Tab(o, 27); Add(o, "Y: ");     AddN(o, g_cp[1], 5);       Tab(o, 45); Add(o, "Z: ");    AddN(o, g_cp[2], 5);
        o = Ln(kTxt); Add(o, "Rot");  Tab(o, 9); Add(o, "Yaw: "); AddN(o, WrapDeg(g_yaw * kDeg), 5); Tab(o, 27); Add(o, "Pitch: "); AddN(o, g_pitch * kDeg, 5); Tab(o, 45); Add(o, "Tilt: "); AddN(o, g_tilt * kDeg, 5);
        o = Ln(kTxt); Add(o, "Quat"); Tab(o, 9); Add(o, "W: ");   AddN(o, q[0], 5);    Tab(o, 27); Add(o, "X: ");     AddN(o, q[1], 5);          Tab(o, 45); Add(o, "Y: ");    AddN(o, q[2], 5);
        Tab(o, 63); Add(o, "Z: "); AddN(o, q[3], 5);
        float fov = (g_fov > 0 ? g_fov : g_baseFov) * kDeg;
        o = Ln(kTxt); Add(o, "FOV: "); if (fov > 0) AddN(o, fov, 5); else Add(o, "?");
        Tab(o, 18); Add(o, "Speed: "); AddN(o, g_speed, 5);
        float step = 1.0f; int sdec = 1;
        if (g_precision) { if (KeyDown(0xA1)) { step = 0.1f; sdec = 1; } else if (KeyDown(0xA3)) { step = 0.01f; sdec = 2; } else { step = 0.001f; sdec = 3; } }
        o = Ln(kTxt); Add(o, "Precision: "); Add(o, OnOff(g_precision)); Tab(o, 18); Add(o, "Step: "); AddN(o, step, sdec);
        Tab(o, 34); Add(o, "Mode: "); Add(o, g_modeCamera ? "Camera" : "Position"); Tab(o, 52); Add(o, "Game HUD: "); Add(o, g_hideHud ? "Hidden" : "Visible");
        o = Ln(kTxt); Add(o, "Wireframe: "); Add(o, OnOff(g_wire)); Tab(o, 18); Add(o, "Textures: "); Add(o, OnOff(g_wireTex));
        Tab(o, 34); Add(o, "Camera: "); Add(o, g_engCam ? "Engine (world follows)" : "View only");
    } else {
        o = Ln(kTxt); Add(o, "Wireframe: "); Add(o, OnOff(g_wire)); Tab(o, 18); Add(o, "Freecam: "); Add(o, OnOff(g_fc));
        Tab(o, 34); Add(o, "Game HUD: "); Add(o, g_hideHud ? "Hidden" : "Visible");
    }
    if (g_fc) {
        char k[24]; KeyName(k, sizeof k, g_cfg.keyUi);
        o = Ln(kDim); Add(o, "Hold F11 for shortcuts   "); Add(o, k); Add(o, " hides the UI");
    }
}

static void DrawOverlay(void* dev) {
    bool help = KeyDown(0x7A /*F11*/);
    if (!help && (g_uiHidden || !g_cfg.overlay)) return;
    void* bb = NULL; uint32_t d[8] = { 0 };
    if (CALLV(fnGetBB, dev, VT_GETBACKBUFFER)(dev, 0, 0, &bb) < 0 || !bb) return;
    CALLV(fnGetDesc, bb, 8)(bb, d);
    CALLV(fnRelease, bb, 2)(bb);
    float H = (float)d[7];
    if (H < 100) return;
    float fs = g_cfg.scale > 0.0f ? g_cfg.scale : (float)(int)((H + 270.0f) / 540.0f);
    fs = (float)(int)(fs + 0.5f);
    if (fs < 1.0f) fs = 1.0f;
    g_fs = fs;
    if (!g_grReady) BuildGlyphRects();
    g_nv = 0; g_nl = 0;
    float lineH = (8.0f + (float)g_cfg.lineGap) * fs;
    if (help) { BuildHelp(); FlushLines(g_cfg.px, g_cfg.py, lineH, true, 0xC8000000u, false); }
    else { BuildPanel(); FlushLines(g_cfg.px, g_cfg.py, lineH, g_cfg.bg, 0x90000000u, g_cfg.shadow); }
    if (g_nv == 0) return;

    g_inOverlay = true;
    CALLV(fnSB, dev, VT_CAPTURESB)(dev, g_sb);
    CALLV(fn1, dev, VT_BEGINSCENE)(dev);
    RS(dev, RS_ZENABLE, 0); RS(dev, RS_ZWRITE, 0); RS(dev, RS_CULL, 1); RS(dev, RS_ALPHATEST, 0);
    RS(dev, RS_FILLMODE, 3);
    RS(dev, RS_ALPHABLEND, 1); RS(dev, RS_SRCBLEND, 5); RS(dev, RS_DESTBLEND, 6);
    RS(dev, RS_FOG, 0); RS(dev, RS_LIGHTING, 0); RS(dev, RS_STENCIL, 0); RS(dev, RS_SPECULAR, 0); RS(dev, RS_COLORWRITE, 0xF);
    ((fnSetTex)oSetTex)(dev, 0, NULL);
    TS(dev, 0, 1, 2); TS(dev, 0, 2, 0); TS(dev, 0, 4, 2); TS(dev, 0, 5, 0);
    TS(dev, 1, 1, 1); TS(dev, 1, 4, 1);
    ((fnSetVS)oSetVS)(dev, FVF_TL_DIFFUSE);
    for (int i = 0; i < g_nv; i += 30000) {
        int n = g_nv - i; if (n > 30000) n = 30000;
        n -= n % 3;
        CALLV(fnDrawUP, dev, VT_DRAWUP)(dev, 4 /*TRIANGLELIST*/, (uint32_t)(n / 3), g_v + i, sizeof(Vtx));
    }
    CALLV(fn1, dev, VT_ENDSCENE)(dev);
    CALLV(fnSB, dev, VT_APPLYSB)(dev, g_sb);
    g_inOverlay = false;
}

// ---------------------------------------------------------------------------------------------- Present / Reset
static long STDCALL hPresent(void* dev, const void* a, const void* b, void* w, const void* c) {
    static bool kF10, kDiv, kN1, kF7, kF6, kF12, kEsc, kUi;
    ++g_frame;
    g_havePlayer = g_playerOk && ReadPlayerPos(g_ppos);
    TryHookScene();
    (void)kF10; (void)kDiv; (void)kN1; (void)kF7; (void)kF6; (void)kF12; (void)kEsc; (void)kUi;
    if ((g_mtWire != 0) != g_wire) { g_wire = g_mtWire != 0; LogS(g_wire ? "wireframe ON" : "wireframe off"); }
    if (g_mtFreecam && !g_fc) FreecamOn();
    else if (!g_mtFreecam && g_fc) FreecamOff("freecam off (menu)");
    if (g_fc && Focused() && !g_mtMenuOpen) FreecamUpdate(); else { g_mdx = g_mdy = g_mdz = 0; }
    if (g_engCam) EngineApply();          // also done right before the next render by the scene hook
    if (g_engLost) { g_engLost = false; if (g_fc) { FreecamOff("freecam off (the scene / camera was replaced)"); g_mtFreecam = 0; } }
    if (CALLV(fn1, dev, VT_TESTCOOP)(dev) == 0) {
        if (!g_sb) { if (CALLV(fnCreateSB, dev, VT_CREATESB)(dev, 1, &g_sb) < 0) g_sb = 0; }
        (void)DrawOverlay;                                       // no console in DeveloperTools
    }
    g_seen3D = false;
    return ((fnPresent)oPresent)(dev, a, b, w, c);
}
static long STDCALL hReset(void* dev, void* params) {
    if (g_sb) { CALLV(fnSB, dev, VT_DELETESB)(dev, g_sb); g_sb = 0; }
    return ((fnReset)oReset)(dev, params);
}

// ---------------------------------------------------------------------------------------------- DirectInput hooks
typedef long (STDCALL *fnDIState)(void*, uint32_t, void*);
typedef long (STDCALL *fnDIData)(void*, uint32_t, void*, uint32_t*, uint32_t);
typedef long (STDCALL *fnDICaps)(void*, void*);
struct DiHook { void** vt; void* st; void* da; };
static DiHook g_dih[4]; static int g_dihN;
static DiHook* FindDih(void* dev) { void** vt = VT(dev); for (int i = 0; i < g_dihN; ++i) if (g_dih[i].vt == vt) return &g_dih[i]; return NULL; }
static void* g_devCache[8]; static int g_devType[8]; static int g_devN;

static int DevType(void* dev) {            // 1 = keyboard, 2 = mouse, 0 = other
    for (int i = 0; i < g_devN; ++i) if (g_devCache[i] == dev) return g_devType[i];
    uint32_t caps[16]; for (int i = 0; i < 16; ++i) caps[i] = 0; caps[0] = 44;
    int t = 0;
    if (((fnDICaps)VT(dev)[3])(dev, caps) >= 0) { uint32_t ty = caps[2] & 0xFF; t = ty == 3 ? 1 : (ty == 2 ? 2 : 0); }
    if (g_devN < 8) { g_devCache[g_devN] = dev; g_devType[g_devN++] = t; }
    return t;
}
static long STDCALL hDIState(void* dev, uint32_t cb, void* data) {
    DiHook* h = FindDih(dev); if (!h) return 0x80004005;
    long r = ((fnDIState)h->st)(dev, cb, data);
    if (g_fc && !g_mtMenuOpen && r >= 0 && data && Readable(data, cb)) {
        if (cb == 16 || cb == 20) { const int* m = (const int*)data; g_mdx += m[0]; g_mdy += m[1]; g_mdz += m[2]; unsigned char* p = (unsigned char*)data; for (uint32_t i = 0; i < cb; ++i) p[i] = 0; }
        else if (cb == 256) { unsigned char* p = (unsigned char*)data; unsigned char esc = p[1]; for (uint32_t i = 0; i < cb; ++i) p[i] = 0; p[1] = esc; }
    }
    return r;
}
static long STDCALL hDIData(void* dev, uint32_t cbObj, void* data, uint32_t* pn, uint32_t flags) {
    DiHook* h = FindDih(dev); if (!h) return 0x80004005;
    long r = ((fnDIData)h->da)(dev, cbObj, data, pn, flags);
    if (g_fc && !g_mtMenuOpen && r >= 0 && pn && data && !(flags & 1 /*DIGDD_PEEK*/)) {
        uint32_t n = *pn;
        if (n && n < 4096 && cbObj >= 12 && Readable(data, n * cbObj)) {
            int t = DevType(dev);
            if (t == 2) {
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t* e = (const uint32_t*)((const unsigned char*)data + i * cbObj); int v = (int)e[1];
                    if (e[0] == 0) g_mdx += v; else if (e[0] == 4) g_mdy += v; else if (e[0] == 8) g_mdz += v;
                }
                *pn = 0;
            }
            else if (t == 1) {                                     // keep only ESC (DIK_ESCAPE = 1): the menu must open
                uint32_t k = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    unsigned char* e = (unsigned char*)data + i * cbObj;
                    if (*(const uint32_t*)e == 1) { if (k != i) for (uint32_t j = 0; j < cbObj; ++j) ((unsigned char*)data)[k * cbObj + j] = e[j]; ++k; }
                }
                *pn = k;
            }
        }
    }
    return r;
}

#ifndef UNIT_TEST
// ---------------------------------------------------------------------------------------------- Windows part
#if !defined(MT_EMBED) && (defined(__clang__) || defined(__GNUC__))
extern "C" {
void* memcpy(void* d, const void* s, size_t n) { unsigned char* a = (unsigned char*)d; const unsigned char* b = (const unsigned char*)s; while (n--) *a++ = *b++; return d; }
void* memmove(void* d, const void* s, size_t n) {
    unsigned char* a = (unsigned char*)d; const unsigned char* b = (const unsigned char*)s;
    if (a < b) { while (n--) *a++ = *b++; } else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void* memset(void* d, int c, size_t n) { unsigned char* a = (unsigned char*)d; while (n--) *a++ = (unsigned char)c; return d; }
int _fltused = 0;
}
#endif
// one explicit export keeps the MinGW linker from auto-exporting memcpy / memset & co.
#ifndef MT_EMBED
extern "C" __declspec(dllexport) const char MafiaToolsVersion[] = "mafiatools 2.0 (engine freecam, wireframe fix, console UI)";
#endif

static HANDLE g_log = INVALID_HANDLE_VALUE;
static char g_iniPath[MAX_PATH + 8];
static void LogS(const char* s) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char b[400]; b[0] = 0; Append(b, sizeof b - 2, s); Append(b, sizeof b, "\r\n");
    DWORD wr = 0; WriteFile(g_log, b, (DWORD)str_len(b), &wr, NULL);
}
static bool Readable(const void* p, uint32_t n) { return !IsBadReadPtr(p, n); }
static uint32_t NowMs() { return (uint32_t)GetTickCount(); }
static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static bool Focused() { HWND h = GetForegroundWindow(); if (!h) return false; DWORD pid = 0; GetWindowThreadProcessId(h, &pid); return pid == GetCurrentProcessId(); }

static void Patch(void** vt, int slot, void* hook, void** orig) {
    DWORD old; *orig = vt[slot];
    VirtualProtect(&vt[slot], 4, PAGE_READWRITE, &old); vt[slot] = hook; VirtualProtect(&vt[slot], 4, old, &old);
}

static bool Match(uint32_t addr, const unsigned char* k, int n) {
    const unsigned char* a = (const unsigned char*)(uintptr_t)addr;
    if (IsBadReadPtr(a, (UINT_PTR)n)) return false;
    for (int i = 0; i < n; ++i) if (a[i] != k[i]) return false;
    return true;
}
static bool CheckExe() {           // Game.exe 1.2: C_mission::GetGame (+0x24), C_game::GetPlayer (+0xE4)
    static const unsigned char k1[] = { 0x8B, 0x41, 0x24, 0xC3 };
    static const unsigned char k2[] = { 0x8B, 0x81, 0xE4, 0x00, 0x00, 0x00, 0xC3 };
    return Match(0x47AE50, k1, 4) && Match(0x47B520, k2, 7);
}
static bool CheckCam() {           // Game.exe 1.2 G_Camera layout used by the engine free camera
    static const unsigned char k1[] = { 0x8D, 0x41, 0x4C, 0xC3 };                       // C_game::GetCamera   lea eax,[ecx+0x4C]
    static const unsigned char k2[] = { 0x83, 0x7E, 0x10, 0x18 };                       // G_Camera::Tick      cmp [esi+0x10],0x18
    static const unsigned char k3[] = { 0x8A, 0x46, 0x20, 0x84, 0xC0, 0x0F, 0x85 };     //   ... freeze byte +0x20 -> return
    static const unsigned char k4[] = { 0xC6, 0x46, 0x64, 0x01 };                       // G_Camera::SetCar    snap byte +0x64
    return Match(0x47B510, k1, 4) && Match(0x5ED4F9, k2, 4) && Match(0x5ED518, k3, 7) && Match(0x5ED1B2, k4, 4);
}

struct GUIDX { uint32_t d1; uint16_t d2, d3; unsigned char d4[8]; };
static const GUIDX kIID_DI8A = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const GUIDX kGUID_Keyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUIDX kGUID_Mouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
typedef long (STDCALL *fnDIC)(HINSTANCE, uint32_t, const GUIDX*, void**, void*);
typedef long (STDCALL *fnDICreateDev)(void*, const GUIDX*, void**, void*);

static void HookDirectInput() {
    HMODULE m = GetModuleHandleA("dinput8.dll"); if (!m) m = LoadLibraryA("dinput8.dll");
    if (!m) { LogS("dinput8.dll not found - the game keeps receiving input in freecam"); return; }
    fnDIC create = (fnDIC)GetProcAddress(m, "DirectInput8Create");
    if (!create) { LogS("DirectInput8Create not found"); return; }
    void* di = NULL;
    if (create(GetModuleHandleA(NULL), 0x0800, &kIID_DI8A, &di, NULL) < 0 || !di) { LogS("DirectInput8Create failed"); return; }
    const GUIDX* guids[2] = { &kGUID_Keyboard, &kGUID_Mouse };
    int patched = 0;
    for (int k = 0; k < 2; ++k) {
        void* dev = NULL;
        if (((fnDICreateDev)VT(di)[3])(di, guids[k], &dev, NULL) < 0 || !dev) continue;
        void** vt = VT(dev);
        bool known = false; for (int i = 0; i < g_dihN; ++i) if (g_dih[i].vt == vt) known = true;
        if (!known && g_dihN < 4) { DiHook& h = g_dih[g_dihN]; h.vt = vt; Patch(vt, 9, (void*)hDIState, &h.st); Patch(vt, 10, (void*)hDIData, &h.da); ++g_dihN; ++patched; }
        ((fnRelease)VT(dev)[2])(dev);
    }
    ((fnRelease)VT(di)[2])(di);
    g_diHooked = patched > 0;
    LogS(g_diHooked ? "DirectInput hooked (keyboard / mouse are blocked from the game in freecam)" : "DirectInput hook already present or failed");
}

static DWORD WINAPI Installer(LPVOID) {
    HINSTANCE ls3df = NULL; void* dev = NULL;
    for (int i = 0; i < 600 && !dev; ++i) {
        Sleep(200);
        if (!ls3df) ls3df = GetModuleHandleA("LS3DF.dll");
        if (!ls3df) continue;
        void** pdev = (void**)((unsigned char*)ls3df + 0x1C597C);
        if (IsBadReadPtr(pdev, 4) || !*pdev || IsBadReadPtr(*pdev, 4)) continue;
        void** vt = *(void***)*pdev;
        if (IsBadReadPtr(vt, 4 * 100)) continue;
        dev = *pdev;
    }
    if (!dev) { LogS("device pointer never appeared - nothing hooked"); return 0; }
    {
        const unsigned char* base = (const unsigned char*)ls3df;
        uint32_t pe = *(const uint32_t*)(base + 0x3C);
        if (!IsBadReadPtr(base + pe, 0x54)) { g_ls3dLo = (uintptr_t)base; g_ls3dHi = g_ls3dLo + *(const uint32_t*)(base + pe + 0x50); }
    }
    g_playerOk = CheckExe();
    LogS(g_playerOk ? "Game.exe matches (player pointer chain enabled)" : "Game.exe does NOT match - player position disabled");
    g_camOk = g_playerOk && CheckCam();
    LogS(!g_cfg.engineCam ? "engine free camera disabled in the ini (EngineCamera=0) - view-matrix free camera only"
                          : (g_camOk ? "G_Camera layout matches (engine free camera enabled)"
                                     : "G_Camera layout does NOT match - view-matrix free camera only"));
    void** vt = *(void***)dev;
    Patch(vt, VT_RESET, (void*)hReset, &oReset);
    Patch(vt, VT_SETTRANSFORM, (void*)hSetXf, &oSetXf);
    Patch(vt, VT_SETRS, (void*)hSetRS, &oSetRS);
    Patch(vt, VT_SETTEX, (void*)hSetTex, &oSetTex);
    Patch(vt, VT_SETVS, (void*)hSetVS, &oSetVS);
    Patch(vt, VT_DRAWP, (void*)hDrawP, &oDrawP);
    Patch(vt, VT_DRAWIP, (void*)hDrawIP, &oDrawIP);
    Patch(vt, VT_DRAWUP, (void*)hDrawUP, &oDrawUP);
    Patch(vt, VT_DRAWIUP, (void*)hDrawIUP, &oDrawIUP);
    Patch(vt, VT_PRESENT, (void*)hPresent, &oPresent);
    LogS("Direct3D hooks installed");
    HookDirectInput();
    return 0;
}

static float IniFloat(const char* key, float def) {
    char buf[32], dflt[32]; FmtN(dflt, def, 3);
    GetPrivateProfileStringA("Tools", key, dflt, buf, sizeof buf, g_iniPath);
    float v = 0, frac = 0.1f; bool dot = false, neg = false;
    for (const char* s = buf; *s; ++s) {
        if (*s == '-') neg = true; else if (*s == '.' || *s == ',') dot = true;
        else if (*s >= '0' && *s <= '9') { if (!dot) v = v * 10 + (*s - '0'); else { v += (*s - '0') * frac; frac *= 0.1f; } }
    }
    return neg ? -v : v;
}

#ifdef MT_EMBED
// called once by DeveloperTools: settings from its .ini (section [Tools]), log next to it
extern "C" void MT_Start(const char* ini, const char* log) {
    g_iniPath[0] = 0; Append(g_iniPath, sizeof g_iniPath, ini);
    g_log = CreateFileA(log, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    g_cfg.overlay = false; g_uiHidden = true;
    g_cfg.speed = IniFloat("FreecamSpeed", 10.0f); g_cfg.sens = IniFloat("MouseSensitivity", 0.15f);
    g_speed = g_cfg.speed;
    g_wireTex = GetPrivateProfileIntA("Tools", "WireTextures", 0, g_iniPath) != 0;
    g_cfg.engineCam = GetPrivateProfileIntA("Tools", "EngineCamera", 1, g_iniPath) != 0;
    LogS("mafiatools (embedded in DeveloperTools) started");
    HANDLE t = CreateThread(NULL, 0, Installer, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
#else
extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        char base[MAX_PATH];
        DWORD n = GetModuleFileNameA(self, base, MAX_PATH);
        if (!n || n >= MAX_PATH) return TRUE;
        size_t dot = n;
        while (dot && base[dot - 1] != '.' && base[dot - 1] != '\\') --dot;
        if (!dot || base[dot - 1] != '.') dot = n + 1;
        base[dot - 1] = 0;
        char logp[MAX_PATH + 8]; logp[0] = 0; g_iniPath[0] = 0;
        Append(g_iniPath, sizeof g_iniPath, base); Append(g_iniPath, sizeof g_iniPath, ".ini");
        Append(logp, sizeof logp, base);           Append(logp, sizeof logp, ".log");
        if (!GetPrivateProfileIntA("Tools", "Enable", 1, g_iniPath)) return TRUE;
        g_log = CreateFileA(logp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
        g_cfg.overlay = GetPrivateProfileIntA("Tools", "Overlay", 1, g_iniPath) != 0;
        g_cfg.px = IniFloat("X", 10.0f); g_cfg.py = IniFloat("Y", 10.0f); g_cfg.scale = IniFloat("Scale", 0.0f);
        g_cfg.speed = IniFloat("FreecamSpeed", 10.0f); g_cfg.sens = IniFloat("MouseSensitivity", 0.15f);
        g_speed = g_cfg.speed;
        g_wireTex = GetPrivateProfileIntA("Tools", "WireTextures", 0, g_iniPath) != 0;
        g_cfg.keyUi = (int)GetPrivateProfileIntA("Tools", "HideUIKey", 0x78, g_iniPath);
        if (g_cfg.keyUi <= 0 || g_cfg.keyUi > 0xFE) g_cfg.keyUi = 0x78;
        g_uiHidden = GetPrivateProfileIntA("Tools", "UIHidden", 0, g_iniPath) != 0;
        g_cfg.shadow = GetPrivateProfileIntA("Tools", "Shadow", 1, g_iniPath) != 0;
        g_cfg.bg = GetPrivateProfileIntA("Tools", "Background", 0, g_iniPath) != 0;
        g_cfg.lineGap = (int)GetPrivateProfileIntA("Tools", "LineGap", 0, g_iniPath);
        if (g_cfg.lineGap < 0 || g_cfg.lineGap > 8) g_cfg.lineGap = 0;
        g_cfg.engineCam = GetPrivateProfileIntA("Tools", "EngineCamera", 1, g_iniPath) != 0;
        LogS("mafiatools started");
        HANDLE t = CreateThread(NULL, 0, Installer, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
#endif // MT_EMBED
#endif
