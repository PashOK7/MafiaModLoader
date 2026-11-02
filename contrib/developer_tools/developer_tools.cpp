// developer_tools.asi - "Developer Tools" for Mafia: The City of Lost Heaven (Game.exe 1.2 only)
// A standalone ASI plugin (Ultimate ASI Loader), NOT a MafiaModLoader mod: it hooks the game itself.
// Opened from the ESC game menu (item "Developer Tools" -> tables\menu\devtools.mnu): script triggers,
// static collision, check/road.bin, object names, god mode, infinite ammo, flying, wireframe, free camera,
// hide HUD, CZ/EN texts. See README.md.
// Build (zig, any OS):
//   zig c++ -target x86-windows-gnu -mcpu=pentium4 -O2 -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin
//       -c developer_tools.cpp -o developer_tools.o
//   (same flags) -DDT_EMBED -c debug_text.cpp -o dt.o
//   (same flags) -DMT_EMBED -c mafiatools_embed.cpp -o mt.o
//   zig c++ -target x86-windows-gnu -shared -nostdlib -s -Wl,--entry=DllMain@12 developer_tools.o dt.o mt.o
//       -o developer_tools.asi -lkernel32 -luser32 -lgdi32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>
#include <stdint.h>

#define STDC  __attribute__((stdcall))
#define THISC __attribute__((thiscall))

// ------------------------------------------------------------------ addresses (Game.exe 1.2)
static const uint32_t A_MISSION       = 0x0063788C;
static const uint32_t A_CHARGE_BAR    = 0x0058F98C;    // 80 CE 10  or dh,0x10
static const uint32_t A_RAYCAST       = 0x005D5930;
static const uint32_t A_COLLISION     = 0x00647F48;
static const uint32_t A_CYCLE_PREPARE = 0x00608770;
static const uint32_t A_SELECT_BY_ID  = 0x006081D0;
static const uint32_t A_CHANGE_WEAPON = 0x0057F550;
static const uint32_t A_SHOW_WPN_NAME = 0x00595E70;
static const uint32_t A_DROP_ITEMS    = 0x0057FAA0;
static const uint32_t A_FREE          = 0x0060FCF6;
static const uint32_t A_CHANGE_WPN_MODEL = 0x0057EC20;   // human_change_weapon_model (fastcall ecx=human): clears the charge +0x21c,
                                                         // HUD charge flag 0x1000 and the wind-up anim -> a clean "cancel throw"
static const uint32_t A_TOP_MENU      = 0x006BD890;      // open GM_Menu (ESC / pause), NULL in game (unverified)
static const float    THROW_K         = 0.015f;        // [0x624E48]
static const float    GRAVITY         = 10.0f;
enum { ID_MOLOTOV = 5, ID_GRENADE = 15 };

static const uint32_t LS3DF_VT_RVA  = 0x0009D500;
static const uint32_t LS3DF_DPL_RVA = 0x0006D640;
static const uint32_t LS3DF_DEV_RVA = 0x001C597C;
enum { VT_BEGIN = 0x34, VT_END = 0x38, VT_PRESENT = 0x3C, VT_SETTEX = 0x0C, VT_DPL = 0x58,
       VT_SX = 0x70, VT_SY = 0x74, VT_RZ = 0xE8, VT_BTN = 0xF8 };

// ------------------------------------------------------------------ tiny CRT
extern "C" void* memset(void* d, int c, size_t n) { uint8_t* p = (uint8_t*)d; while (n--) *p++ = (uint8_t)c; return d; }
extern "C" void* memcpy(void* d, const void* s, size_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; return d; }
extern "C" int _fltused = 0;
static size_t SLen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void SCat(char* d, size_t cap, const char* s) { size_t n = SLen(d); while (*s && n + 1 < cap) d[n++] = *s++; d[n] = 0; }
static void SCatU(char* d, size_t cap, uint32_t v) {
    char t[12]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    char o[12]; for (int i = 0; i < n; ++i) o[i] = t[n - 1 - i]; o[n] = 0; SCat(d, cap, o);
}
static void SCatX(char* d, size_t cap, uint32_t v) {
    char o[11] = "0x"; for (int i = 0; i < 8; ++i) o[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15]; o[10] = 0; SCat(d, cap, o);
}
static void SCatF(char* d, size_t cap, float f) {
    if (f < 0) { SCat(d, cap, "-"); f = -f; }
    uint32_t i = (uint32_t)f, fr = (uint32_t)((f - (float)i) * 1000.0f + 0.5f); if (fr > 999) fr = 999;
    SCatU(d, cap, i); SCat(d, cap, "."); if (fr < 100) SCat(d, cap, "0"); if (fr < 10) SCat(d, cap, "0"); SCatU(d, cap, fr);
}
static inline float FSqrt(float x) { float r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
static inline float FSin(float x)  { float r; __asm__("fsin"  : "=t"(r) : "0"(x)); return r; }
static inline float FCos(float x)  { float r; __asm__("fcos"  : "=t"(r) : "0"(x)); return r; }
static const float PI = 3.14159265f;

// ------------------------------------------------------------------ log + ini
static char g_ini[MAX_PATH], g_logPath[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;
static void Log(const char* s) {
    if (g_log == INVALID_HANDLE_VALUE) return;
    char b[600]; b[0] = 0; SCat(b, sizeof b, "["); SCatU(b, sizeof b, GetTickCount()); SCat(b, sizeof b, "] ");
    SCat(b, sizeof b, s); SCat(b, sizeof b, "\r\n");
    DWORD w; WriteFile(g_log, b, (DWORD)SLen(b), &w, NULL);
}
static float IniFloat(const char* key, float def) {
    char buf[32]; GetPrivateProfileStringA("DeveloperTools", key, "", buf, sizeof buf, g_ini);
    if (!buf[0]) return def;
    const char* s = buf; bool neg = false; if (*s == '-') { neg = true; s++; }
    float v = 0, f = 0.1f; bool frac = false;
    for (; *s; s++) {
        if (*s == '.' || *s == ',') { frac = true; continue; }
        if (*s < '0' || *s > '9') break;
        if (frac) { v += (*s - '0') * f; f *= 0.1f; } else v = v * 10 + (*s - '0');
    }
    return neg ? -v : v;
}
static int IniInt(const char* key, int def) { return (int)GetPrivateProfileIntA("DeveloperTools", key, def, g_ini); }

static uint32_t IniColor(const char* key, uint32_t def) {
    char buf[32]; GetPrivateProfileStringA("DeveloperTools", key, "", buf, sizeof buf, g_ini);
    if (!buf[0]) return def;
    const char* s = buf; if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    for (; *s; s++) {
        int d = (*s >= '0' && *s <= '9') ? *s - '0' : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10 : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10 : -1;
        if (d < 0) break; v = (v << 4) | (uint32_t)d;
    }
    return v;
}

// =================================================================== DeveloperTools proper

// ------------------------------------------------------------------ config
static struct {
    int enabled, log, key, showOnStart, projMode;
    float fovDeg, maxDist, lineWidth, textScale;
    int showInrange, showDist, showVolumes, showLabels, showLines, onlyActive, segs, hud, keepMs, showName, ownStates; float fillA;
    uint32_t colInrange, colInrangeHit, colDist, colDistHit, colVolume;
    int menuKey, camMatrix; float fov;
    int showAll; float allDist; uint32_t colAll;
    int god, infAmmo, fly, showCheck, showRoad, hideHud, langEn;
    float labelRef, labelMin, labelMax;
} C;
// embedded DebugText (../DebugText/DebugText.cpp built with -DDT_EMBED): script command debug_text "..." -> text panel
extern "C" int  g_dtShow;                                 // menu switch "Debug text" (ini ShowDebugText)
int g_dtShow = 1;
extern "C" { extern volatile float g_mtFov; extern volatile int g_mtWire, g_mtFreecam, g_mtBypass, g_mtMenuOpen; void MT_Start(const char* ini, const char* log); }   // mt_embed.cpp (mafiatools)
extern "C" bool DT_Start(const char* base);               // hooks the script command, settings [DebugText]/[DebugTextPanel]
extern "C" void DT_AfterHud();                            // draws the panel (call right after the game HUD)
static void LoadCfg() {
    C.enabled     = IniInt("Enabled", 1);
    C.log         = IniInt("Log", 1);
    C.key         = IniInt("Key", 0x76);               // F7
    C.showOnStart = IniInt("ShowOnStart", 1);
    C.projMode    = IniInt("Projection", 0);
    C.fovDeg      = IniFloat("FovDeg", 60.0f);
    C.maxDist     = IniFloat("MaxDistance", 150.0f);          // triggers + names of objects (m)
    if (C.maxDist < 10) C.maxDist = 10; if (C.maxDist > 3000) C.maxDist = 3000;
    C.lineWidth   = IniFloat("LineWidth", 2.0f);
    C.textScale   = IniFloat("TextScale", 1.4f);
    C.showInrange = IniInt("ShowInrange", 1);
    C.showDist    = IniInt("ShowActorsDist", 1);
    C.showVolumes = IniInt("ShowVolumes", 1);
    C.showLabels  = IniInt("ShowLabels", 1);
    C.showLines   = IniInt("ShowLines", 1);
    C.onlyActive  = IniInt("OnlyActive", 0);
    C.segs        = IniInt("Segments", 32);
    C.hud         = IniInt("Hud", 1); C.keepMs = IniInt("KeepMs", 10000); C.showName = IniInt("ShowName", 1); C.fillA = IniFloat("FillAlpha", 0.15f); C.ownStates = IniInt("OwnRenderStates", 1);
    C.colInrange    = IniColor("ColorInrange",    0xE000E0FF);
    C.colInrangeHit = IniColor("ColorInrangeHit", 0xF0FF3030);
    C.colDist       = IniColor("ColorDist",       0xE0FFD000);
    C.colDistHit    = IniColor("ColorDistHit",    0xF0FF6000);
    C.colVolume     = IniColor("ColorVolume",     0xE0FF40FF);
    C.menuKey   = IniInt("MenuKey", 0);                   // 0 = off; settings are in the ESC game menu
    C.camMatrix = IniInt("UseCameraMatrix", 1);           // projection from the game camera (follows its FOV)
    C.fov       = IniFloat("CameraFov", 0.0f);
    g_dtShow    = IniInt("ShowDebugText", 1);             // DebugText panel on/off
    C.labelRef  = IniFloat("LabelDistance", 6.0f);       // labels have TextScale size at this distance (m), 0 = fixed size on screen
    C.labelMin  = IniFloat("LabelMinScale", 0.4f);       // smallest (far away) ...
    C.labelMax  = IniFloat("LabelMaxScale", 1.4f);       // ... and largest (close up), relative to TextScale
    C.showCheck = IniInt("ShowCheck", 0);                 // check.bin: walking web (nodes + connections)
    C.showRoad  = IniInt("ShowRoad", 0);                  // road.bin: road network (crossings + road nodes)
    C.langEn    = IniInt("LanguageEN", 0);                // 1 = our menu texts from tables\\devtools_en.def
    C.hideHud   = IniInt("HideHud", 0);                   // 1 = no game HUD / our texts (menus, sun glare stay)
    C.fly       = IniInt("Flying", 0);                    // menu "Létání": on = flying
    C.god       = IniInt("GodMode", 1);                   // player takes no damage (hits, cars, explosions)
    C.infAmmo   = IniInt("InfiniteAmmo", 1);              // magazine of the weapon in hand never empties
    C.showAll   = IniInt("ShowAllObjects", 0);            // names of all actors (cars, doors, people, ...)
    C.allDist   = IniFloat("AllObjectsDistance", 40.0f);
    C.colAll    = IniColor("ColorAllObjects", 0xFFB0E0FF);            // 0 = leave the game's FOV, else degrees (40 - 130)
    if (C.segs < 8) C.segs = 8; if (C.segs > 96) C.segs = 96;
    if (C.lineWidth < 0.5f) C.lineWidth = 0.5f;
    if (C.textScale < 0.5f) C.textScale = 0.5f;
    // no longer in the menu - always on (old ini values from earlier versions are ignored)
    C.showInrange = C.showDist = C.showVolumes = C.showLabels = C.showLines = C.showName = 1; C.onlyActive = 0;
}

static void SCatF1(char* d, size_t cap, float f) {
    if (f < 0) { SCat(d, cap, "-"); f = -f; }
    if (f > 1e6f) f = 1e6f;
    uint32_t t = (uint32_t)(f * 10.0f + 0.5f);
    SCatU(d, cap, t / 10); SCat(d, cap, "."); SCatU(d, cap, t % 10);
}

// ------------------------------------------------------------------ game access
struct V3 { float x, y, z; };
static inline V3 Add(V3 a, V3 b) { V3 r = { a.x + b.x, a.y + b.y, a.z + b.z }; return r; }
static inline V3 Sub(V3 a, V3 b) { V3 r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static inline V3 Mul(V3 a, float k) { V3 r = { a.x * k, a.y * k, a.z * k }; return r; }
static inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 Cross(V3 a, V3 b) { V3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; return r; }
static inline float Len(V3 a) { return FSqrt(Dot(a, a)); }
static inline V3 Norm(V3 a) { float l = Len(a); return l > 1e-6f ? Mul(a, 1.0f / l) : a; }

static bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }

static uint8_t* GetGame() {
    uint8_t* mission = *(uint8_t**)A_MISSION;
    if (!Readable(mission, 0x40)) return NULL;
    uint8_t* game = *(uint8_t**)(mission + 0x24);
    return Readable(game, 0x2b10) ? game : NULL;
}
static uint8_t* GetPlayer() {
    uint8_t* game = GetGame(); if (!game) return NULL;
    uint8_t* h = *(uint8_t**)(game + 0xe4);
    return Readable(h, 0x650) ? h : NULL;
}
static uint8_t* GetCameraFrame() {
    uint8_t* mission = *(uint8_t**)A_MISSION; if (!Readable(mission, 0x40)) return NULL;
    uint8_t* cam = *(uint8_t**)(mission + 0x10); if (!Readable(cam, 0x180)) return NULL;
    uint8_t* fr = *(uint8_t**)(cam + 0x17c);
    return Readable(fr, 0x50) ? fr : NULL;
}
static inline V3 RdV3(const uint8_t* p) { V3 r = { ((const float*)p)[0], ((const float*)p)[1], ((const float*)p)[2] }; return r; }
static inline uint32_t InHand(uint8_t* h) { return *(uint16_t*)(h + 0x4a0); }
static inline bool IsThrowable(uint32_t id) { return id == ID_GRENADE || id == ID_MOLOTOV; }

typedef void (THISC *Fn_CyclePrepare)(void* inv);
typedef char (THISC *Fn_SelectByID)(void* inv, uint32_t id, void* vec);
typedef void (THISC *Fn_ChangeWeapon)(void* human, uint32_t a, uint32_t hasDrops);
typedef void (THISC *Fn_Human)(void* human);
typedef void (THISC *Fn_DropItems)(void* human, void* vec);
typedef void (__cdecl *Fn_Free)(void* p);
typedef int  (THISC *Fn_Raycast)(void* world, const V3* org, const V3* delta, V3* hitPos, V3* hitNormal, int flags, uint32_t mask);
struct DropVec { uint8_t alloc; uint8_t pad[3]; uint8_t* begin; uint8_t* end; uint8_t* cap; };

static bool GameHasFocus() {
    HWND w = GetForegroundWindow(); if (!w) return false;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
static bool KeyDown(int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; }
// ------------------------------------------------------------------ projection
static uint8_t* g_ls3d = NULL;
static D3DMATRIX g_vp;          // view * projection (device mode)
static bool  g_devMode = false;
static float g_vx, g_vy, g_vw, g_vh;
// camera mode
static V3 g_cPos, g_cR, g_cU, g_cF; static float g_p11, g_p22;

static void MatMul(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& o) {
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        o.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
}
static bool g_projLogged = false;
static bool SetupProjection(int scrW, int scrH) {
    g_vx = 0; g_vy = 0; g_vw = (float)scrW; g_vh = (float)scrH;
    g_devMode = false;
    if (C.projMode == 0 && g_ls3d) {
        IDirect3DDevice8** pdev = (IDirect3DDevice8**)(g_ls3d + LS3DF_DEV_RVA);
        IDirect3DDevice8* dev = Readable(pdev, 4) ? *pdev : NULL;
        D3DMATRIX v, p; memset(&v, 0, sizeof v); memset(&p, 0, sizeof p);
        if (dev && SUCCEEDED(dev->GetTransform(D3DTS_VIEW, &v)) && SUCCEEDED(dev->GetTransform(D3DTS_PROJECTION, &p))
            && p._34 > 0.99f && p._34 < 1.01f && p._44 > -0.01f && p._44 < 0.01f && p._11 > 0.1f && p._22 > 0.1f) {
            MatMul(v, p, g_vp);
            D3DVIEWPORT8 vp;
            if (SUCCEEDED(dev->GetViewport(&vp)) && vp.Width > 16 && vp.Height > 16) {
                g_vx = (float)vp.X; g_vy = (float)vp.Y; g_vw = (float)vp.Width; g_vh = (float)vp.Height;
            }
            g_devMode = true;
        }
        if (!g_projLogged) {
            char l[300]; l[0] = 0; SCat(l, sizeof l, "projection: device "); SCatX(l, sizeof l, (uint32_t)dev);
            SCat(l, sizeof l, g_devMode ? " (perspective OK, using it) P11 " : " (not usable, camera fallback) P11 ");
            SCatF(l, sizeof l, p._11); SCat(l, sizeof l, " P22 "); SCatF(l, sizeof l, p._22);
            SCat(l, sizeof l, " P34 "); SCatF(l, sizeof l, p._34); Log(l);
            g_projLogged = true;
        }
    }
    if (g_devMode) return true;
    uint8_t* fr = GetCameraFrame(); if (!fr) return false;
    float lr, lu, lf;
    g_cR = RdV3(fr + 0x10); g_cU = RdV3(fr + 0x20); g_cF = RdV3(fr + 0x30); g_cPos = RdV3(fr + 0x40);
    lr = Len(g_cR); lu = Len(g_cU); lf = Len(g_cF);
    if (lr < 1e-4f || lu < 1e-4f || lf < 1e-4f) return false;
    g_cR = Mul(g_cR, 1.0f / lr); g_cU = Mul(g_cU, 1.0f / lu); g_cF = Mul(g_cF, 1.0f / lf);
    float h = C.fovDeg * PI / 360.0f;
    g_p22 = FCos(h) / FSin(h);
    g_p11 = g_p22 * (float)scrH / (float)scrW;
    return true;
}
// returns false when the point is behind the camera
static bool Project(V3 p, float& sx, float& sy) {
    float cx, cy, cw;
    if (g_devMode) {
        const D3DMATRIX& m = g_vp;
        cx = p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41;
        cy = p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42;
        cw = p.x * m._14 + p.y * m._24 + p.z * m._34 + m._44;
    } else {
        V3 r = Sub(p, g_cPos);
        cx = Dot(r, g_cR) * g_p11; cy = Dot(r, g_cU) * g_p22; cw = Dot(r, g_cF);
    }
    if (cw < 0.1f) return false;
    sx = g_vx + (cx / cw * 0.5f + 0.5f) * g_vw;
    sy = g_vy + (0.5f - cy / cw * 0.5f) * g_vh;
    return sx > -4000 && sx < 8000 && sy > -4000 && sy < 8000;
}

// ------------------------------------------------------------------ drawing
struct Vtx { float x, y, z, rhw; uint32_t diffuse, specular; float u, v; };
enum { MAX_VTX = 393216 };
static Vtx g_v[MAX_VTX];
static int g_nv = 0;

static float g_vz = 0, g_vr = 1;   // depth of flat 2D quads (labels); 0/1 = always on top
static inline void V(float x, float y, uint32_t c) {
    if (g_nv >= MAX_VTX) return;
    Vtx& t = g_v[g_nv++]; t.x = x; t.y = y; t.z = g_vz; t.rhw = g_vr; t.diffuse = c; t.specular = 0; t.u = t.v = 0;
}
static inline uint32_t WithAlpha(uint32_t c, float a) {
    if (a < 0) a = 0; if (a > 1) a = 1;
    return (c & 0x00FFFFFF) | ((uint32_t)((float)(c >> 24) * a) << 24);
}
static void Seg(float x0, float y0, float x1, float y1, float w, uint32_t c0, uint32_t c1) {
    float dx = x1 - x0, dy = y1 - y0, l = FSqrt(dx * dx + dy * dy);
    if (l < 0.01f) return;
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    V(x0 + nx, y0 + ny, c0); V(x1 + nx, y1 + ny, c1); V(x1 - nx, y1 - ny, c1);
    V(x0 + nx, y0 + ny, c0); V(x1 - nx, y1 - ny, c1); V(x0 - nx, y0 - ny, c0);
}
// ring lying on the hit surface (plane perpendicular to the normal)
static void WorldRing(V3 c, V3 n, float r, float w, uint32_t col) {
    V3 ref = { 0, 1, 0 };
    if (n.y > 0.9f || n.y < -0.9f) { ref.x = 1; ref.y = 0; }
    V3 a = Norm(Cross(n, ref)), b = Norm(Cross(n, a));
    c = Add(c, Mul(n, 0.03f));                                    // lift slightly off the surface
    const int N = 24;
    float px = 0, py = 0; bool pv = false;
    for (int i = 0; i <= N; ++i) {
        float ang = 2 * PI * i / N;
        V3 p = Add(c, Add(Mul(a, FCos(ang) * r), Mul(b, FSin(ang) * r)));
        float sx, sy; bool ok = Project(p, sx, sy);
        if (ok && pv) Seg(px, py, sx, sy, w, col, col);
        px = sx; py = sy; pv = ok;
    }
    float sx, sy;                                                 // centre dot
    if (Project(c, sx, sy)) { float d = w * 1.2f; V(sx - d, sy - d, col); V(sx + d, sy - d, col); V(sx + d, sy + d, col);
                              V(sx - d, sy - d, col); V(sx + d, sy + d, col); V(sx - d, sy + d, col); }
}

// ------------------------------------------------------------------ HUD (throwable slots + hints)
// Everything is built from flat triangles in the same pre-transformed vertex list as the flight line,
// in "reference units" of an 800x600 screen scaled by the real height (and HudScale).
static void Rect(float x0, float y0, float x1, float y1, uint32_t c) {
    V(x0, y0, c); V(x1, y0, c); V(x0, y1, c);
    V(x1, y0, c); V(x1, y1, c); V(x0, y1, c);
}
static void Frame4(float x0, float y0, float x1, float y1, float w, uint32_t c) {
    Rect(x0, y0, x1, y0 + w, c); Rect(x0, y1 - w, x1, y1, c);
    Rect(x0, y0 + w, x0 + w, y1 - w, c); Rect(x1 - w, y0 + w, x1, y1 - w, c);
}
static void Ellipse(float cx, float cy, float rx, float ry, uint32_t c) {
    const int N = 20; float px = cx + rx, py = cy;
    for (int i = 1; i <= N; ++i) {
        float a = 2.0f * PI * (float)i / (float)N, x = cx + FCos(a) * rx, y = cy + FSin(a) * ry;
        V(cx, cy, c); V(px, py, c); V(x, y, c); px = x; py = y;
    }
}
static void Circle(float cx, float cy, float r, float w, uint32_t c) {
    const int N = 16; float px = cx + r, py = cy;
    for (int i = 1; i <= N; ++i) {
        float a = 2.0f * PI * (float)i / (float)N, x = cx + FCos(a) * r, y = cy + FSin(a) * r;
        Seg(px, py, x, y, w, c, c); px = x; py = y;
    }
}

// 5x7 font, rows top->bottom, bit4 = left column
static const uint8_t kFontAZ[26][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},{0x11,0x11,0x11,0x1F,0x11,0x11,0x11},{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},{0x11,0x12,0x14,0x18,0x14,0x12,0x11},{0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},{0x11,0x11,0x19,0x15,0x13,0x11,0x11},{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},{0x1F,0x04,0x04,0x04,0x04,0x04,0x04},{0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},{0x11,0x11,0x11,0x15,0x15,0x15,0x0A},{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04},{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F} };
static const uint8_t kFont09[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},{0x1F,0x01,0x02,0x04,0x08,0x08,0x08},{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C} };
static const uint8_t kGlyphX[7]     = {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11};   // 'x' (times)
static const uint8_t kGlyphSlash[7] = {0x01,0x01,0x02,0x04,0x08,0x10,0x10};
static const uint8_t kGlyphDash[7]  = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
static const uint8_t kGlyphPlus[7]  = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00};
static const uint8_t kGlyphDot[7]   = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
static const uint8_t kGlyphUnder[7] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F};
static const uint8_t kGlyphPct[7] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03};
static const uint8_t kGlyphQ[7] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04};
static const uint8_t kGlyphLP[7] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02};
static const uint8_t kGlyphRP[7] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08};
static const uint8_t kGlyphLT[7] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02};
static const uint8_t kGlyphGT[7] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08};
static const uint8_t kGlyphColon[7] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00};

// Windows-1250 (the ini is ANSI): Czech letters -> base letter + accent (1 acute, 2 caron, 3 ring); upper-cases everything
static const uint8_t* Glyph(uint8_t ch, int& accent) {
    accent = 0;
    static const struct { uint8_t c; char base; uint8_t acc; } kCz[] = {
        {0xC1,'A',1},{0xE1,'A',1},{0xC8,'C',2},{0xE8,'C',2},{0xCF,'D',2},{0xEF,'D',2},{0xC9,'E',1},{0xE9,'E',1},
        {0xCC,'E',2},{0xEC,'E',2},{0xCD,'I',1},{0xED,'I',1},{0xD2,'N',2},{0xF2,'N',2},{0xD3,'O',1},{0xF3,'O',1},
        {0xD8,'R',2},{0xF8,'R',2},{0x8A,'S',2},{0x9A,'S',2},{0x8D,'T',2},{0x9D,'T',2},{0xDA,'U',1},{0xFA,'U',1},
        {0xD9,'U',3},{0xF9,'U',3},{0xDD,'Y',1},{0xFD,'Y',1},{0x8E,'Z',2},{0x9E,'Z',2} };
    for (unsigned i = 0; i < sizeof kCz / sizeof kCz[0]; ++i)
        if (kCz[i].c == ch) { accent = kCz[i].acc; ch = (uint8_t)kCz[i].base; break; }
    if (ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 32);
    if (ch >= 'A' && ch <= 'Z') return kFontAZ[ch - 'A'];
    if (ch >= '0' && ch <= '9') return kFont09[ch - '0'];
    switch (ch) { case '*': return kGlyphX; case '/': return kGlyphSlash; case '-': return kGlyphDash;
                  case '+': return kGlyphPlus; case '.': return kGlyphDot; case '_': return kGlyphUnder; case '%': return kGlyphPct; case '?': return kGlyphQ; case '(': return kGlyphLP; case ')': return kGlyphRP; case '<': return kGlyphLT; case '>': return kGlyphGT; case ':': return kGlyphColon; }
    return NULL;   // space / unknown -> blank cell
}
static void GlyphRows(const uint8_t* rows, int n, int row0, float x, float y, float px, uint32_t c) {
    for (int r = 0; r < n; ++r) {
        uint8_t bits = rows[r];
        for (int col = 0; col < 5; ) {          // merge horizontal runs into one quad
            if (!(bits & (0x10 >> col))) { ++col; continue; }
            int c0 = col; while (col < 5 && (bits & (0x10 >> col))) ++col;
            Rect(x + c0 * px, y + (row0 + r) * px, x + col * px, y + (row0 + r + 1) * px, c);
        }
    }
}
static float TextW(const char* s, float px) { float w = 0; for (; *s; ++s) w += 6 * px; return w > 0 ? w - px : 0; }
static void Text(const char* s, float x, float y, float px, uint32_t c, bool shadow = true) {
    static const uint8_t kAcute[3] = {0x02,0x04,0x00}, kCaron[3] = {0x0A,0x04,0x00}, kRing[3] = {0x04,0x0A,0x04};
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
        uint32_t col = pass ? c : (c & 0xFF000000) >> 1 & 0xFF000000;
        float o = pass ? 0.0f : px * 0.6f, cx = x + o;
        for (const char* p = s; *p; ++p, cx += 6 * px) {
            int acc; const uint8_t* g = Glyph((uint8_t)*p, acc);
            if (!g) continue;
            GlyphRows(g, 7, 0, cx, y + o, px, col);
            if (acc == 1) GlyphRows(kAcute, 2, -2, cx, y + o, px, col);
            if (acc == 2) GlyphRows(kCaron, 2, -2, cx, y + o, px, col);
            if (acc == 3) GlyphRows(kRing, 3, -3, cx, y + o, px, col);
        }
    }
}
static void KeyName(int vk, char* out, size_t cap) {
    out[0] = 0;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { out[0] = (char)vk; out[1] = 0; return; }
    if (vk >= VK_F1 && vk <= VK_F12) { SCat(out, cap, "F"); SCatU(out, cap, (uint32_t)(vk - VK_F1 + 1)); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { SCat(out, cap, "N"); SCatU(out, cap, (uint32_t)(vk - VK_NUMPAD0)); return; }
    switch (vk) {
    case VK_LBUTTON: SCat(out, cap, "LMB"); break;  case VK_RBUTTON: SCat(out, cap, "RMB"); break;
    case VK_MBUTTON: SCat(out, cap, "M3"); break;   case VK_XBUTTON1: SCat(out, cap, "M4"); break;
    case VK_XBUTTON2: SCat(out, cap, "M5"); break;  case VK_SPACE: SCat(out, cap, "SPACE"); break;
    case VK_TAB: SCat(out, cap, "TAB"); break;      case VK_SHIFT: case VK_LSHIFT: SCat(out, cap, "SHIFT"); break;
    case VK_CONTROL: case VK_LCONTROL: SCat(out, cap, "CTRL"); break; case VK_MENU: SCat(out, cap, "ALT"); break;
    case VK_CAPITAL: SCat(out, cap, "CAPS"); break;
    default: SCat(out, cap, "?"); break;
    }
}
// ------------------------------------------------------------------ script VM (Game.exe 1.2)
//  C_program (embedded at C_detector+0x70, C_entity+0x9ac):
//    +0x10/+0x14 instruction vector begin/end, 8 bytes each {int opcode; void* payload}
//    +0x1c pc, +0x48 float regs / +0x4c count, +0x58 actor regs / +0x5c count
//  C_detector (program = detector + 0x70): frame +0x68 (= program - 8), inrange mask +0xd8 (= program + 0x68),
//    trigger volumes: colls +0x118, world matrices +0x11c (0x40 stride), count +0x124
static const uint32_t A_BASE_PROC = 0x0046D3B0;   // C_program::Process base dispatcher (thiscall, ret 4)
static const uint32_t A_DET_PROC  = 0x00433D10;   // C_detector's C_program override (thiscall, ret 4)
enum { OP_IF = 0x09, OP_IFFLTINRANGE = 0x14, OP_DET_INRANGE = 0x1D, OP_GETACTORSDIST = 0x39 };

enum { IT_INRANGE = 0, IT_DIST = 1 };
enum { CMP_NONE = 0, CMP_LT = 1, CMP_GT = 2, CMP_RANGE = 3 };
struct Item {
    uint8_t type, cmp; int16_t reg;
    int ins0, ins1;            // first / last instruction index that produced it
    int a, b;                  // actor regs (IT_DIST)
    float lo, hi;              // thresholds (immediates)
    int loReg, hiReg;          // or float regs (-1 = immediate)
    uint8_t loggedDraw;
};
enum { MAX_IT = 24, MAX_PROG = 1024, HASH = 4096 };
struct Prog {
    uint8_t* p; uint32_t vt; uint8_t* ib; uint8_t* ie; uint32_t seen;
    uint8_t isDet; uint8_t n; Item it[MAX_IT];
};
static Prog    g_pr[MAX_PROG];
static int     g_npr = 0;
static int16_t g_hash[HASH];
static int     g_scanLogs = 0;
static bool    g_show = true;

static void ClearProgs() {
    g_npr = 0;
    for (int i = 0; i < HASH; ++i) g_hash[i] = -1;
}

static inline int RdI(const uint8_t* p, int i) { return ((const int*)p)[i]; }
static inline float RdF(const uint8_t* p, int i) { return ((const float*)p)[i]; }
static inline bool SaneR(float r) { return r > 0.05f && r < 5000.0f; }

static void AddItem(Prog& g, const Item& it) {
    for (int k = 0; k < g.n; ++k) {
        Item& o = g.it[k];
        if (o.type == it.type && o.cmp == it.cmp && o.a == it.a && o.b == it.b && o.reg == it.reg
            && o.lo == it.lo && o.hi == it.hi && o.loReg == it.loReg && o.hiReg == it.hiReg) {
            if (it.ins0 < o.ins0) o.ins0 = it.ins0;
            if (it.ins1 > o.ins1) o.ins1 = it.ins1;
            return;
        }
    }
    if (g.n < MAX_IT) g.it[g.n++] = it;
}

// After `getactorsdist a, b, zz` look a few lines ahead for the comparison that uses flt[zz]
static void ScanDist(Prog& g, int i, int n, int a, int b, int zz) {
    bool found = false;
    for (int j = i + 1; j < n && j <= i + 16; ++j) {
        const uint8_t* e = g.ib + j * 8;
        int op = RdI(e, 0); const uint8_t* pay = *(uint8_t* const*)(e + 4);
        if (op == OP_GETACTORSDIST && Readable(pay, 12) && RdI(pay, 2) == zz) break;   // flt[zz] overwritten
        Item it; memset(&it, 0, sizeof it);
        it.type = IT_DIST; it.a = a; it.b = b; it.reg = (int16_t)zz; it.ins0 = i; it.ins1 = j;
        it.loReg = it.hiReg = -1;
        if (op == OP_IFFLTINRANGE && Readable(pay, 12) && RdI(pay, 2) == zz) {
            // payload: float low, float high, int reg   (low <= flt[reg] < high)
            it.cmp = CMP_RANGE; it.lo = RdF(pay, 0); it.hi = RdF(pay, 1);
            if (SaneR(it.hi)) { AddItem(g, it); found = true; }
        } else if (op == OP_IF && Readable(pay, 20)) {
            // payload: int leftIdx, float leftImm, int rightIdx, float rightImm, int cmp (1 '<', 2 '>', 3 '=', 4 '!')
            int li = RdI(pay, 0), ri = RdI(pay, 2), c = RdI(pay, 4);
            float lim = RdF(pay, 1), rim = RdF(pay, 3);
            if (c != 1 && c != 2) continue;
            if (li == zz) {
                bool lt = c == 1;                               // dist < right  /  dist > right
                if (ri < 0) { if (!SaneR(rim)) continue; if (lt) it.hi = rim; else it.lo = rim; }
                else { if (lt) it.hiReg = ri; else it.loReg = ri; }
                it.cmp = lt ? CMP_LT : CMP_GT;
                AddItem(g, it); found = true;
            } else if (ri == zz) {
                bool lt = c == 2;                               // left > dist  ->  dist < left
                if (li < 0) { if (!SaneR(lim)) continue; if (lt) it.hi = lim; else it.lo = lim; }
                else { if (lt) it.hiReg = li; else it.loReg = li; }
                it.cmp = lt ? CMP_LT : CMP_GT;
                AddItem(g, it); found = true;
            }
        }
    }
    if (!found) {                                                // distance read but compared somewhere we can't see
        Item it; memset(&it, 0, sizeof it);
        it.type = IT_DIST; it.cmp = CMP_NONE; it.a = a; it.b = b; it.reg = (int16_t)zz; it.ins0 = it.ins1 = i;
        it.loReg = it.hiReg = -1;
        AddItem(g, it);
    }
}

static bool ProgName(const uint8_t* p, bool det, char* out, int cap);
static void Scan(Prog& g) {
    g.n = 0;
    int n = (int)((g.ie - g.ib) >> 3);
    if (n <= 0 || n > 200000 || !Readable(g.ib, (size_t)n * 8)) return;
    for (int i = 0; i < n; ++i) {
        const uint8_t* e = g.ib + i * 8;
        int op = RdI(e, 0); const uint8_t* pay = *(uint8_t* const*)(e + 4);
        if (op == OP_DET_INRANGE && Readable(pay, 8)) {
            // payload: int destReg, float radius
            float r = RdF(pay, 1);
            if (!SaneR(r)) continue;
            Item it; memset(&it, 0, sizeof it);
            it.type = IT_INRANGE; it.cmp = CMP_LT; it.hi = r; it.reg = (int16_t)RdI(pay, 0);
            it.ins0 = it.ins1 = i; it.loReg = it.hiReg = -1;
            AddItem(g, it);
        } else if (op == OP_GETACTORSDIST && Readable(pay, 12)) {
            ScanDist(g, i, n, RdI(pay, 0), RdI(pay, 1), RdI(pay, 2));
        }
    }
    if (g.n && g_scanLogs < 300) {
        ++g_scanLogs;
        char l[500]; l[0] = 0;
        SCat(l, sizeof l, "prog "); SCatX(l, sizeof l, (uint32_t)g.p); SCat(l, sizeof l, g.isDet ? " det" : "");
        { char nm[48]; if (ProgName(g.p, g.isDet, nm, sizeof nm)) { SCat(l, sizeof l, " '"); SCat(l, sizeof l, nm); SCat(l, sizeof l, "'"); } }
        SCat(l, sizeof l, " ins "); SCatU(l, sizeof l, (uint32_t)n); SCat(l, sizeof l, ":");
        for (int k = 0; k < g.n && SLen(l) < 420; ++k) {
            const Item& it = g.it[k];
            SCat(l, sizeof l, it.type == IT_INRANGE ? " [inrange r" : " [dist a");
            if (it.type == IT_INRANGE) SCatF1(l, sizeof l, it.hi);
            else {
                SCatU(l, sizeof l, (uint32_t)it.a); SCat(l, sizeof l, " b"); SCatU(l, sizeof l, (uint32_t)it.b);
                static const char* cn[] = { " ?", " <", " >", " in" };
                SCat(l, sizeof l, cn[it.cmp]);
                if (it.cmp == CMP_RANGE) { SCat(l, sizeof l, " "); SCatF1(l, sizeof l, it.lo); SCat(l, sizeof l, ".."); SCatF1(l, sizeof l, it.hi); }
                else if (it.cmp == CMP_LT) { SCat(l, sizeof l, " "); if (it.hiReg >= 0) { SCat(l, sizeof l, "flt"); SCatU(l, sizeof l, (uint32_t)it.hiReg); } else SCatF1(l, sizeof l, it.hi); }
                else if (it.cmp == CMP_GT) { SCat(l, sizeof l, " "); if (it.loReg >= 0) { SCat(l, sizeof l, "flt"); SCatU(l, sizeof l, (uint32_t)it.loReg); } else SCatF1(l, sizeof l, it.lo); }
            }
            SCat(l, sizeof l, " @"); SCatU(l, sizeof l, (uint32_t)it.ins0); SCat(l, sizeof l, "]");
        }
        Log(l);
    }
}

static void Register(uint8_t* p, bool det) {
    if (!Readable(p, 0x70)) return;
    uint32_t h = ((uint32_t)p >> 3) & (HASH - 1);
    Prog* g = NULL;
    for (int k = 0; k < HASH; ++k, h = (h + 1) & (HASH - 1)) {
        int idx = g_hash[h];
        if (idx < 0) break;
        if (g_pr[idx].p == p) { g = &g_pr[idx]; break; }
    }
    uint8_t* ib = *(uint8_t**)(p + 0x10);
    uint8_t* ie = *(uint8_t**)(p + 0x14);
    uint32_t vt = *(uint32_t*)p;
    if (!g) {
        if (g_npr >= MAX_PROG) { ClearProgs(); h = ((uint32_t)p >> 3) & (HASH - 1); }
        while (g_hash[h] >= 0) h = (h + 1) & (HASH - 1);
        g_hash[h] = (int16_t)g_npr;
        g = &g_pr[g_npr++];
        g->p = p; g->isDet = det ? 1 : 0; g->vt = vt; g->ib = ib; g->ie = ie; g->n = 0;
        Scan(*g);
    } else if (g->vt != vt || g->ib != ib || g->ie != ie) {
        g->vt = vt; g->ib = ib; g->ie = ie; g->isDet = det ? 1 : 0;
        Scan(*g);
    }
    if (det) g->isDet = 1;
    g->seen = GetTickCount();
}

// ------------------------------------------------------------------ process hooks
typedef void (__attribute__((fastcall)) *Fn_Proc)(uint8_t* prog, void* edx, int delta);
static Fn_Proc o_baseProc, o_detProc;
extern "C" __attribute__((fastcall)) void H_BaseProc(uint8_t* prog, void* edx, int delta) {
    Register(prog, false);
    o_baseProc(prog, edx, delta);
}
extern "C" __attribute__((fastcall)) void H_DetProc(uint8_t* prog, void* edx, int delta) {
    Register(prog, true);
    o_detProc(prog, edx, delta);
}

static uint8_t* g_trampMem = NULL;
static int g_trampUsed = 0;
// 5-byte jmp at `at`; the original first `len` bytes (position independent) move to a trampoline.
// If somebody already put a jmp there, chain to it instead.
static void* InlineHook(uint32_t at, const uint8_t* expect, int len, void* fn, const char* name) {
    uint8_t* t = (uint8_t*)at;
    if (!Readable(t, 16)) { Log("ERROR: hook target not readable"); return NULL; }
    if (!g_trampMem) {
        g_trampMem = (uint8_t*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!g_trampMem) { Log("ERROR: VirtualAlloc"); return NULL; }
    }
    uint8_t* tr = g_trampMem + g_trampUsed; g_trampUsed += 32;
    int patch;
    if (t[0] == 0xE9) {
        uint32_t dest = at + 5 + *(int32_t*)(t + 1);
        tr[0] = 0xE9; *(int32_t*)(tr + 1) = (int32_t)(dest - ((uint32_t)tr + 5));
        patch = 5;
        char l[160]; l[0] = 0; SCat(l, sizeof l, name); SCat(l, sizeof l, ": already hooked by another mod, chaining to ");
        SCatX(l, sizeof l, dest); Log(l);
    } else {
        for (int i = 0; i < len; ++i) if (t[i] != expect[i]) {
            char l[160]; l[0] = 0; SCat(l, sizeof l, "ERROR: "); SCat(l, sizeof l, name);
            SCat(l, sizeof l, " - unexpected bytes (not Game.exe 1.2?), not hooked"); Log(l); return NULL;
        }
        for (int i = 0; i < len; ++i) tr[i] = t[i];
        tr[len] = 0xE9; *(int32_t*)(tr + len + 1) = (int32_t)((at + len) - ((uint32_t)tr + len + 5));
        patch = len;
    }
    DWORD old;
    if (!VirtualProtect(t, patch, PAGE_EXECUTE_READWRITE, &old)) { Log("ERROR: VirtualProtect"); return NULL; }
    t[0] = 0xE9; *(int32_t*)(t + 1) = (int32_t)((uint32_t)fn - (at + 5));
    for (int i = 5; i < patch; ++i) t[i] = 0x90;
    VirtualProtect(t, patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, patch);
    char l[160]; l[0] = 0; SCat(l, sizeof l, name); SCat(l, sizeof l, " hooked at "); SCatX(l, sizeof l, at); Log(l);
    return tr;
}
static void HookScript() {
    static const uint8_t kBase[6] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };   // mov eax, fs:[0]
    static const uint8_t kDet[6]  = { 0x83, 0xEC, 0x0C, 0x56, 0x8B, 0xF1 };   // sub esp,0xc / push esi / mov esi,ecx
    o_baseProc = (Fn_Proc)InlineHook(A_BASE_PROC, kBase, 6, (void*)&H_BaseProc, "C_program::Process");
    o_detProc  = (Fn_Proc)InlineHook(A_DET_PROC, kDet, 6, (void*)&H_DetProc, "C_detector::Process");
}

// ------------------------------------------------------------------ world drawing
static float g_w = 1.5f;   // line width in pixels for this frame

static V3 g_cam = { 0, 0, 0 };   // camera position for this frame (set in DrawProgs)
static int g_ringN = 32;         // segments per ring for the sphere being drawn (LOD)

static void SegW(V3 a, V3 b, uint32_t ca, uint32_t cb, float w);
static void FillTri3(V3 a, V3 b, V3 c, uint32_t col);
static void Ring(V3 c, V3 a, V3 b, float r, uint32_t col) {
    int N = g_ringN;
    V3 prev = Add(c, Mul(a, r));
    for (int i = 1; i <= N; ++i) {
        float ang = 2 * PI * (float)i / (float)N;
        V3 p = Add(c, Add(Mul(a, FCos(ang) * r), Mul(b, FSin(ang) * r)));
        SegW(prev, p, col, col, g_w);
        prev = p;
    }
}
// A 3D trigger sphere drawn as three great circles (like the dev tools)
// (Y is up). The trigger tests a 3D distance, so the whole ball is the trigger, not just the ground circle.
static void WireSphere(V3 c, float r, uint32_t col) {
    V3 X = { 1, 0, 0 }, Y = { 0, 1, 0 }, Z = { 0, 0, 1 };
    V3 dv = Sub(c, g_cam); float L = Len(dv);
    int N = C.segs;
    if (L > 60) N = N * 2 / 3; if (L > 120) N = N / 2;
    if (N < 12) N = 12; if (N > 96) N = 96;
    g_ringN = N;
    // three thin great circles: the equator and two vertical ones
    Ring(c, X, Z, r, col); Ring(c, X, Y, r, col); Ring(c, Z, Y, r, col);
}
// ---- 3D segment clipped against the camera near plane and a screen guard band.
// Nothing is dropped because one end is behind the camera: the visible part is always drawn.
static float ClipW(V3 p) {
    if (g_devMode) { const D3DMATRIX& m = g_vp; return p.x * m._14 + p.y * m._24 + p.z * m._34 + m._44; }
    return Dot(Sub(p, g_cPos), g_cF);
}
static void ProjRaw(V3 p, float w, float& sx, float& sy) {
    float cx, cy;
    if (g_devMode) {
        const D3DMATRIX& m = g_vp;
        cx = p.x * m._11 + p.y * m._21 + p.z * m._31 + m._41;
        cy = p.x * m._12 + p.y * m._22 + p.z * m._32 + m._42;
    } else { V3 r = Sub(p, g_cPos); cx = Dot(r, g_cR) * g_p11; cy = Dot(r, g_cU) * g_p22; }
    sx = g_vx + (cx / w * 0.5f + 0.5f) * g_vw;
    sy = g_vy + (0.5f - cy / w * 0.5f) * g_vh;
}
static uint32_t LerpCol(uint32_t a, uint32_t b, float t) {
    if (a == b) return a;
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        float x = (float)((a >> s) & 0xFF), y = (float)((b >> s) & 0xFF);
        r |= ((uint32_t)(x + (y - x) * t + 0.5f) & 0xFF) << s;
    }
    return r;
}
// ---- depth: with g_zOn the 3D primitives carry the game camera's own depth (z = Q - Q*near/w, rhw = 1/w),
// so they can be drawn with the z test against the scene's depth buffer (collision behind walls is hidden).
static bool  g_zOn = false;
static float g_zQ = 1.0f, g_zQN = 0.0f, g_zBias = 0.0f;
static float g_zRel = 0.001f;                                     // + this share of the distance
static inline void ZW(float w, float& z, float& rhw) {
    float wb = w - (g_zBias + w * g_zRel); if (wb < 0.05f) wb = 0.05f;    // pulled toward the camera
    z = g_zQ - g_zQN / wb; if (z < 0) z = 0; if (z > 0.99999f) z = 0.99999f;
    rhw = 1.0f / wb;
}
static inline void VZ(float x, float y, uint32_t c, float z, float rhw) {
    if (g_nv >= MAX_VTX) return;
    Vtx& t = g_v[g_nv++]; t.x = x; t.y = y; t.z = z; t.rhw = rhw; t.diffuse = c; t.specular = 0; t.u = t.v = 0;
}
static void SegZ(float x0, float y0, float z0, float r0, float x1, float y1, float z1, float r1, float w, uint32_t c0, uint32_t c1) {
    float dx = x1 - x0, dy = y1 - y0, l = FSqrt(dx * dx + dy * dy);
    if (l < 0.01f) return;
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    VZ(x0 + nx, y0 + ny, c0, z0, r0); VZ(x1 + nx, y1 + ny, c1, z1, r1); VZ(x1 - nx, y1 - ny, c1, z1, r1);
    VZ(x0 + nx, y0 + ny, c0, z0, r0); VZ(x1 - nx, y1 - ny, c1, z1, r1); VZ(x0 - nx, y0 - ny, c0, z0, r0);
}
static bool g_soft = false;                                       // software occlusion test on (SoftVisible)
static bool SoftVisible(V3 p);
static bool LabelVisible(V3 p);
static V3 FrontPoint(V3 c, float r);
static void SegWRaw(V3 a, V3 b, uint32_t ca, uint32_t cb, float w) {
    const float NW = 0.12f;
    float wa = ClipW(a), wb = ClipW(b);
    if (!(wa >= NW) && !(wb >= NW)) return;                       // both behind (or NaN)
    if (!(wa >= NW)) { float t = (NW - wa) / (wb - wa); a = Add(a, Mul(Sub(b, a), t)); ca = LerpCol(ca, cb, t); wa = NW; }
    else if (!(wb >= NW)) { float t = (NW - wb) / (wa - wb); b = Add(b, Mul(Sub(a, b), t)); cb = LerpCol(cb, ca, t); wb = NW; }
    float x0, y0, x1, y1; ProjRaw(a, wa, x0, y0); ProjRaw(b, wb, x1, y1);
    const float G = 256.0f;
    float dx = x1 - x0, dy = y1 - y0, t0 = 0, t1 = 1;
    float pp[4] = { -dx, dx, -dy, dy };
    float qq[4] = { x0 - (g_vx - G), (g_vx + g_vw + G) - x0, y0 - (g_vy - G), (g_vy + g_vh + G) - y0 };
    for (int i = 0; i < 4; ++i) {
        if (pp[i] == 0) { if (qq[i] < 0) return; continue; }
        float r = qq[i] / pp[i];
        if (pp[i] < 0) { if (r > t1) return; if (r > t0) t0 = r; }
        else           { if (r < t0) return; if (r < t1) t1 = r; }
    }
    if (!(t0 <= t1)) return;
    if (g_zOn) {                                                   // z and rhw are linear in screen space
        float za, ra, zb, rb; ZW(wa, za, ra); ZW(wb, zb, rb);
        SegZ(x0 + dx * t0, y0 + dy * t0, za + (zb - za) * t0, ra + (rb - ra) * t0,
             x0 + dx * t1, y0 + dy * t1, za + (zb - za) * t1, ra + (rb - ra) * t1, w, LerpCol(ca, cb, t0), LerpCol(ca, cb, t1));
        return;
    }
    Seg(x0 + dx * t0, y0 + dy * t0, x0 + dx * t1, y0 + dy * t1, w, LerpCol(ca, cb, t0), LerpCol(ca, cb, t1));
}
// Segment with the occlusion test: split into pieces of ~8 px, pieces whose middle is hidden behind the
// collision geometry (walls, floors) are dropped, the visible runs are drawn.
static void SegW(V3 a, V3 b, uint32_t ca, uint32_t cb, float w) {
    if (!g_soft) { SegWRaw(a, b, ca, cb, w); return; }
    float wa = ClipW(a), wb = ClipW(b);
    if (!(wa >= 0.12f) && !(wb >= 0.12f)) return;
    float ma = wa > 0.12f ? wa : 0.12f, mb = wb > 0.12f ? wb : 0.12f;
    float x0, y0, x1, y1; ProjRaw(a, ma, x0, y0); ProjRaw(b, mb, x1, y1);
    float L = FSqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    int k = (int)(L / 8.0f) + 1; if (k > 48) k = 48;
    int st = -1;
    for (int i = 0; i <= k; ++i) {
        bool vis = false;
        if (i < k) { float t = (i + 0.5f) / k; vis = SoftVisible(Add(a, Mul(Sub(b, a), t))); }
        if (vis && st < 0) st = i;
        if (!vis && st >= 0) {
            float t0 = (float)st / k, t1 = (float)i / k;
            SegWRaw(Add(a, Mul(Sub(b, a), t0)), Add(a, Mul(Sub(b, a), t1)), LerpCol(ca, cb, t0), LerpCol(ca, cb, t1), w);
            st = -1;
        }
    }
}
static void Line3(V3 a, V3 b, uint32_t col) { SegW(a, b, col, col, g_w * 0.8f); }
// Filled 3D triangle: clipped against the near plane in 3D, then against the screen guard band in 2D,
// so a face that reaches behind the camera still shows its visible part instead of vanishing.
struct P2 { float x, y, z, r; };
static int ClipPoly2(const P2* in, int n, P2* out, int axis, float lim, bool keepLess) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        P2 a = in[i], b = in[(i + 1) % n];
        float va = axis ? a.y : a.x, vb = axis ? b.y : b.x;
        bool ia = keepLess ? va <= lim : va >= lim, ib = keepLess ? vb <= lim : vb >= lim;
        if (ia) out[m++] = a;
        if (ia != ib) { float t = (lim - va) / (vb - va); P2 q = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.r + (b.r - a.r) * t }; out[m++] = q; }
    }
    return m;
}
static void FillTri3(V3 a, V3 b, V3 c, uint32_t col) {
    const float NW = 0.12f;
    if (g_soft) {                                                   // all of it behind a wall -> skip
        V3 m = Mul(Add(Add(a, b), c), 1.0f / 3.0f);
        if (!SoftVisible(m) && !SoftVisible(a) && !SoftVisible(b) && !SoftVisible(c)) return;
    }
    V3 in[3] = { a, b, c }; float w[3] = { ClipW(a), ClipW(b), ClipW(c) };
    V3 q[4]; float qw[4]; int n = 0;
    for (int i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        bool ii = w[i] >= NW, ij = w[j] >= NW;
        if (ii) { q[n] = in[i]; qw[n++] = w[i]; }
        if (ii != ij) { float t = (NW - w[i]) / (w[j] - w[i]); q[n] = Add(in[i], Mul(Sub(in[j], in[i]), t)); qw[n++] = NW; }
    }
    if (n < 3) return;
    P2 p[16], r[16];
    for (int i = 0; i < n; ++i) { ProjRaw(q[i], qw[i], p[i].x, p[i].y); p[i].z = 0; p[i].r = 1; if (g_zOn) ZW(qw[i], p[i].z, p[i].r); }
    const float G = 256.0f;
    n = ClipPoly2(p, n, r, 0, g_vx - G, false);          if (n < 3) return;
    n = ClipPoly2(r, n, p, 0, g_vx + g_vw + G, true);    if (n < 3) return;
    n = ClipPoly2(p, n, r, 1, g_vy - G, false);          if (n < 3) return;
    n = ClipPoly2(r, n, p, 1, g_vy + g_vh + G, true);    if (n < 3) return;
    for (int i = 1; i + 1 < n; ++i) {
        VZ(p[0].x, p[0].y, col, p[0].z, p[0].r); VZ(p[i].x, p[i].y, col, p[i].z, p[i].r); VZ(p[i + 1].x, p[i + 1].y, col, p[i + 1].z, p[i + 1].r);
    }
}

// ---- label font: Segoe UI rendered by GDI into a texture atlas (cp1250 0x20..0xFF), drawn as textured
// quads that carry the label's depth like the rest of the overlay. Fallback: the built-in 5x7 font.
enum { MAX_TV = 120000, FT_W = 512, FT_H = 512 };
static Vtx g_tv[MAX_TV]; static int g_ntv = 0;
static IDirect3DTexture8* g_ftex = NULL; static IDirect3DDevice8* g_fdev = NULL; static bool g_fFailed = false;
struct LGlyph { float u0, v0, u1, v1, w; };
static LGlyph g_gl[256]; static float g_fEm = 32.0f, g_fCellH = 40.0f;
static void BuildFont(IDirect3DDevice8* dev) {
    if (g_fFailed || (g_ftex && g_fdev == dev)) return;
    g_ftex = NULL; g_fdev = dev;                                    // a new device: the old texture is gone with it
    bool ok = false;
    HDC dc = CreateCompatibleDC(NULL);
    HFONT f = CreateFontW(-(int)g_fEm, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (dc && f) {
        HGDIOBJ of = SelectObject(dc, f);
        TEXTMETRICW tm; GetTextMetricsW(dc, &tm); g_fCellH = (float)(tm.tmHeight + 2);
        BITMAPINFO bi; memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = FT_W; bi.bmiHeader.biHeight = -FT_H;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp && bits) {
            HGDIOBJ ob = SelectObject(dc, bmp);
            memset(bits, 0, FT_W * FT_H * 4);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
            int x = 0, y = 0, ch = (int)g_fCellH;
            memset(g_gl, 0, sizeof g_gl);
            for (int c = 0x20; c < 256; ++c) {
                char b = (char)c; wchar_t wc = 0;
                if (MultiByteToWideChar(1250, 0, &b, 1, &wc, 1) != 1 || wc < 0x20) continue;
                SIZE sz; GetTextExtentPoint32W(dc, &wc, 1, &sz);
                int cw = sz.cx + 4;
                if (x + cw > FT_W) { x = 0; y += ch; }
                if (y + ch > FT_H) break;
                TextOutW(dc, x + 2, y + 1, &wc, 1);
                g_gl[c].u0 = (float)x / FT_W; g_gl[c].v0 = (float)y / FT_H;
                g_gl[c].u1 = (float)(x + cw) / FT_W; g_gl[c].v1 = (float)(y + ch) / FT_H;
                g_gl[c].w = (float)cw;
                x += cw;
            }
            GdiFlush();
            IDirect3DTexture8* t = NULL;
            if (SUCCEEDED(dev->CreateTexture(FT_W, FT_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t))) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(t->LockRect(0, &lr, NULL, 0))) {
                    for (int yy = 0; yy < FT_H; ++yy) {
                        uint32_t* d = (uint32_t*)((uint8_t*)lr.pBits + yy * lr.Pitch);
                        const uint32_t* sp = (const uint32_t*)bits + yy * FT_W;
                        for (int xx = 0; xx < FT_W; ++xx) {
                            uint32_t v = sp[xx], r = (v >> 16) & 255, g = (v >> 8) & 255, bl = v & 255;
                            uint32_t a = r > g ? r : g; if (bl > a) a = bl;
                            d[xx] = (a << 24) | 0xFFFFFF;
                        }
                    }
                    t->UnlockRect(0);
                    g_ftex = t; ok = true;
                } else t->Release();
            }
            SelectObject(dc, ob); DeleteObject(bmp);
        }
        SelectObject(dc, of);
    }
    if (f) DeleteObject(f);
    if (dc) DeleteDC(dc);
    if (!ok) { g_fFailed = true; Log("label font: Segoe UI texture failed - using the built-in font"); }
    else Log("label font: Segoe UI ready");
}
// width / text at em height h (px)
static float LW(const char* s, float h) {
    if (!g_ftex) return TextW(s, h / 7.0f);
    float k = h / g_fEm, w = 0;
    for (const uint8_t* p = (const uint8_t*)s; *p; ++p) w += g_gl[*p].w ? g_gl[*p].w - 4 : g_fEm * 0.3f;
    return w * k;
}
static void TVq(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
    if (g_ntv + 6 > MAX_TV) return;
    Vtx q[4] = { { x0, y0, g_vz, g_vr, c, 0, u0, v0 }, { x1, y0, g_vz, g_vr, c, 0, u1, v0 },
                 { x1, y1, g_vz, g_vr, c, 0, u1, v1 }, { x0, y1, g_vz, g_vr, c, 0, u0, v1 } };
    g_tv[g_ntv++] = q[0]; g_tv[g_ntv++] = q[1]; g_tv[g_ntv++] = q[2];
    g_tv[g_ntv++] = q[0]; g_tv[g_ntv++] = q[2]; g_tv[g_ntv++] = q[3];
}
static void LT(const char* s, float x, float y, float h, uint32_t col) {
    if (!g_ftex) { Text(s, x, y, h / 7.0f, col); return; }
    float k = h / g_fEm;
    uint32_t sh = (col >> 24) << 24;                                // black shadow (labels without a box)
    for (int pass = 0; pass < 2; ++pass) {
        float cx = x - 2 * k + (pass ? 0 : 1), cy = y - k + (pass ? 0 : 1);
        for (const uint8_t* p = (const uint8_t*)s; *p; ++p) {
            const LGlyph& gl = g_gl[*p];
            if (!gl.w) { cx += g_fEm * 0.3f * k; continue; }
            if (*p != ' ') TVq(cx, cy, cx + gl.w * k, cy + g_fCellH * k, gl.u0, gl.v0, gl.u1, gl.v1, pass ? col : sh);
            cx += (gl.w - 4) * k;
        }
    }
}
static void DrawTV(IDirect3DDevice8* dev) {
    if (!dev || !g_ftex || g_ntv < 3) return;
    static const D3DTEXTURESTAGESTATETYPE ts[] = { D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2,
        D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_MAGFILTER, D3DTSS_MINFILTER, D3DTSS_MIPFILTER,
        D3DTSS_ADDRESSU, D3DTSS_ADDRESSV, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS };
    static const DWORD tsVal[] = { D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE,
        D3DTOP_MODULATE, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE,
        D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, 0, D3DTTFF_DISABLE };
    const int NTS = sizeof ts / sizeof ts[0];
    DWORD tsOld[NTS], vs = 0;
    for (int i = 0; i < NTS; ++i) dev->GetTextureStageState(0, ts[i], &tsOld[i]);
    dev->GetVertexShader(&vs);
    IDirect3DBaseTexture8* oldTex = NULL; dev->GetTexture(0, &oldTex);
    IDirect3DVertexBuffer8* oldVB = NULL; UINT oldStride = 0; dev->GetStreamSource(0, &oldVB, &oldStride);
    IDirect3DIndexBuffer8* oldIB = NULL; UINT oldBase = 0; dev->GetIndices(&oldIB, &oldBase);
    for (int i = 0; i < NTS; ++i) dev->SetTextureStageState(0, ts[i], tsVal[i]);
    dev->SetVertexShader(0x1C4);
    dev->SetTexture(0, g_ftex);
    for (int off = 0; off + 3 <= g_ntv; off += 30000) {
        int cnt = g_ntv - off; if (cnt > 30000) cnt = 30000;
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(cnt / 3), g_tv + off, sizeof(Vtx));
    }
    dev->SetTexture(0, oldTex); if (oldTex) oldTex->Release();
    dev->SetStreamSource(0, oldVB, oldStride); if (oldVB) oldVB->Release();
    dev->SetIndices(oldIB, oldBase); if (oldIB) oldIB->Release();
    dev->SetVertexShader(vs);
    for (int i = 0; i < NTS; ++i) dev->SetTextureStageState(0, ts[i], tsOld[i]);
}
// labels are drawn at the depth of their anchor point (pulled 0.75 m toward the camera), so with the
// z test on they hide behind walls like the geometry; outside the z pass g_vz/g_vr stay 0/1 = on top
static void LabelDepth(V3 at) {
    if (!g_zOn) return;
    float b = g_zBias; g_zBias = 0.75f;
    ZW(ClipW(at), g_vz, g_vr);
    g_zBias = b;
}
static void LabelDepthOff() { g_vz = 0; g_vr = 1; }
static const float kLabelScale = 0.75f;                            // labels a bit smaller than TextScale
// labels sit in the world: they get smaller with distance like the object they belong to
static float LabelDistScale(V3 at) {
    (void)at; return 1.0f;                                          // one size (user)
    if (!(C.labelRef > 0.1f)) return 1.0f;
    float w = ClipW(at); if (!(w > 0.1f)) w = 0.1f;
    float k = C.labelRef / w;
    if (k < C.labelMin) k = C.labelMin; if (k > C.labelMax) k = C.labelMax;
    return k;
}
// label above a world point; 'top' (the script name) is drawn as an extra line above 's'
static bool g_labelBg = false;                                     // dark box behind Label (trigger labels only)
static void Label(V3 at, const char* s, uint32_t col, const char* top = NULL) {
    float sx, sy; if (!Project(at, sx, sy)) return;
    if (g_soft && !LabelVisible(at)) return;
    float k = g_vh / 600.0f; if (k < 0.6f) k = 0.6f;
    float h = 7.0f * k * (C.textScale / 1.4f) * LabelDistScale(at);   // em height in px
    LabelDepth(at);
    float w = LW(s, h);
    bool hasTop = top && top[0];
    float wt = hasTop ? LW(top, h) : 0;
    float W = wt > w ? wt : w, lh = h * 1.25f, pad = h * 0.35f;
    float y0 = hasTop ? sy - lh : sy;
    if (g_labelBg) Rect(sx - W * 0.5f - pad, y0 - pad * 0.5f, sx + W * 0.5f + pad, sy + lh, 0x90000000);
    if (hasTop) LT(top, sx - wt * 0.5f, y0, h, 0xFFFFFFFF);
    LT(s, sx - w * 0.5f, sy, h, col | 0xFF000000);
    LabelDepthOff();
}

// script label like the dev tools: gold title + grey lines in a dark box whose bottom sits on 'at'
static uint32_t g_lbTitle = 0xFFE8C050;                            // LabelBox title colour (the script's colour)
static uint32_t g_lbText = 0xFFE6E6E6;                             // LabelBox line colour
static uint32_t ProgCol(const char* name) {
    static const uint32_t kPal[12] = { 0xFFFFC040, 0xFF40E0FF, 0xFFFF6090, 0xFF80FF50, 0xFFB080FF, 0xFFFF9030,
                                       0xFF5090FF, 0xFFFFFF60, 0xFF40FFC0, 0xFFFF60FF, 0xFFC0FF40, 0xFFFF4040 };
    uint32_t h = 2166136261u;
    for (const uint8_t* q = (const uint8_t*)name; *q; ++q) { h ^= *q; h *= 16777619u; }
    return kPal[h % 12];
}
static const uint32_t kGreen = 0xFF30FF40;
static uint32_t HitCol(uint32_t) { return kGreen; }   // inside / condition met = green
static void LabelBox(V3 at, const char* title, char (*ln)[96], int n) {
    float sx, sy; if (!Project(at, sx, sy)) return;
    if (g_soft && !LabelVisible(at)) return;
    float k = g_vh / 600.0f; if (k < 0.6f) k = 0.6f;
    float h = 6.5f * k * (C.textScale / 1.4f) * LabelDistScale(at), lh = h * 1.25f, pad = h * 0.45f;
    bool hasT = title && title[0];
    int rows = n + (hasT ? 1 : 0);
    if (!rows) return;
    LabelDepth(at);
    float W = hasT ? LW(title, h) : 0;
    for (int i = 0; i < n; ++i) { float w = LW(ln[i], h); if (w > W) W = w; }
    float y = sy - rows * lh;
    Rect(sx - W * 0.5f - pad, y - pad * 0.5f, sx + W * 0.5f + pad, sy, 0xA0000000);
    if (hasT) { LT(title, sx - LW(title, h) * 0.5f, y, h, g_lbTitle); y += lh; }
    for (int i = 0; i < n; ++i, y += lh) LT(ln[i], sx - LW(ln[i], h) * 0.5f, y, h, g_lbText);
    LabelDepthOff();
}

static bool InLS3D(const void* p);
typedef void (THISC *Fn_UpdWM)(void* frame);
static Fn_UpdWM g_updWM = NULL; static bool g_updWMTried = false;
static bool FramePos(const uint8_t* fr, V3& out) {
    if (!Readable(fr, 0xb0)) return false;
    if (!(*(const uint32_t*)(fr + 0xac) & 0x20) && InLS3D(*(void* const*)fr)) {   // world matrix not valid
        if (!g_updWMTried) {
            g_updWMTried = true;
            HMODULE h = GetModuleHandleA("LS3DF.dll");
            if (h) g_updWM = (Fn_UpdWM)(void*)GetProcAddress(h, "?UpdateWMatrixProc@I3D_frame@@AAEXXZ");
            Log(g_updWM ? "frame world matrix refresh: LS3DF UpdateWMatrixProc found" : "frame world matrix refresh: export NOT found");
        }
        if (g_updWM) g_updWM((void*)fr);
    }
    out = RdV3(fr + 0x40);
    return out.x == out.x && out.y == out.y && out.z == out.z;
}
static bool ActorPos(const uint8_t* act, V3& out) {
    if (!Readable(act, 0x70)) return false;
    return FramePos(*(uint8_t* const*)(act + 0x68), out);
}

// name of a frame (I3D_frame +0x100), copied into out; false if it does not look like a name
static bool FrameName(const uint8_t* fr, char* out, int cap) {
    out[0] = 0;
    if (!Readable(fr, 0x104)) return false;
    const char* n = *(const char* const*)(fr + 0x100);
    if (!Readable(n, 1)) return false;
    int i = 0;
    for (; i < cap - 1; ++i) {
        if (((uintptr_t)(n + i) & 0xFFF) == 0 && !Readable(n + i, 1)) break;
        char ch = n[i];
        if (!ch) break;
        if ((uint8_t)ch < 0x20) { out[0] = 0; return false; }
        out[i] = ch;
    }
    out[i] = 0;
    return i > 0;
}
// script name = name of the actor that owns the program: a detector (program at +0x70) or an entity
// (program embedded at +0x9c); the actor's displayed name is its frame's name
static bool ProgName(const uint8_t* p, bool det, char* out, int cap) {
    out[0] = 0;
    const uint8_t* owner = p - (det ? 0x70 : 0x9c);
    if (!Readable(owner, 0x70)) return false;
    uint32_t vt = *(const uint32_t*)owner;
    if (vt < 0x00600000 || vt >= 0x00680000) return false;        // not an actor (e.g. the mission's own script)
    return FrameName(*(const uint8_t* const*)(owner + 0x68), out, cap);
}

// counts the actors detector_inrange would count right now
static int CountInRange(uint8_t* game, uint32_t mask, V3 c, float r) {
    uint8_t** b = *(uint8_t***)(game + 0xf0);
    uint8_t** e = *(uint8_t***)(game + 0xf4);
    if (!b || e < b) return 0;
    int n = (int)(e - b); if (n > 20000 || !Readable(b, (size_t)n * 4)) return 0;
    int cnt = 0; float r2 = r * r;
    for (int i = 0; i < n; ++i) {
        uint8_t* a = b[i];
        if (!Readable(a, 0x600)) continue;
        int kind = *(int*)(a + 0x10);
        if (kind != 0x1b && kind != 2) continue;
        if (!(mask & *(uint32_t*)(a + 0x5fc))) continue;
        V3 p; if (!ActorPos(a, p)) continue;
        V3 d = Sub(p, c);
        if (Dot(d, d) < r2) ++cnt;
    }
    return cnt;
}

static int g_drawn = 0, g_alive = 0;

// script command names by opcode (built by Game.exe 1.2 at 0x463960, read out with an emulator)
static const char* const kOpName[398] = {
    "label", "goto", "gosub", "return", "wait", "beep",
    "end", "dim_flt", "let", "if", "rnd", "dim_frm",
    "dim_act", "findframe", "frm_seton", "frm_seton1", "frm_ison", "frm_linkto",
    "findactor", "call", "iffltinrange", "playsound", "playsoundstop", "playsoundex",
    "stopsound", "getsoundtime", "act_setstate", "detector_issignal", "detector_setsignal", "detector_inrange",
    "setcompass", "setlmlevel", "detector_waitforuse", "detector_waitforhit", "commandblock", "callsubroutine",
    "soundfade", "endofmission", "program_storage", "door_lock", "door_open", "door_enableus",
    "recclear", "recaddactor", "recload", "recloadfull", "recwaitforend", "recunload",
    "loadcolltree", "explosion", "timeron", "timeroff", "change_mission", "camera_setswing",
    "airplane_start", "airplane_start2", "debug_text", "getactorsdist", "getcardamage", "human_addweapon",
    "human_canaddweapon", "human_delweapon", "human_isweapon", "human_activateweapon", "human_anyweaponinhand", "human_anyweaponininventory",
    "human_getiteminrhand", "preloadmodel", "loaddifferences", "cleardifferences", "{", "}",
    "setaipriority", "event", "setevent", "event_use_cb", "enemy_changeanim", "human_changeanim",
    "dialog_begin", "dialog_end", "dialog_camswitch", "game_nightmission", "human_looktoactor", "human_looktoframe",
    "getmissionnumber", "setmissionnumber", "human_linktohand", "human_unlinkfromhand", "createweaponfromframe", "garage_addcar",
    "garage_addcaridx", "garage_delcar", "garage_generatecars", "garage_releasecars", "garage_carmanager", "player_lockcontrols",
    "person_playanim", "person_stopanim", "gunshop_menu", "human_set8slot", "enemy_forcescript", "compareownerwithex",
    "human_setproperty", "human_getproperty", "disablecolls", "human_getowner", "human_force_settocar", "car_setdestroymotor",
    "actor_setdir", "actor_setpos", "actor_setplacement", "garage_enablesteal", "settimeoutevent", "emitparticle",
    "stopparticle", "setplayerfireevent", "findnearactor", "setplayerhornevent", "human_holster", "camera_lock",
    "camera_unlock", "setmissionnameid", "wingman_setindicator", "wingman_delindicator", "model_playanim", "model_stopanim",
    "model_create", "model_destroy", "human_changemodel", "autosavegame", "autosavegamefull", "human_death",
    "human_talk", "human_eraseab", "human_createab", "policemanager_on", "policemanager_add", "policemanager_del",
    "policemanager_setspeed", "policemanager_forcearrest", "car_enableus", "car_setdooropen", "human_setfiretarget", "iscarusable",
    "door_setopenpose", "airplane_isdestroyed", "human_returnfrompanic", "car_getspeed", "car_setspeed", "car_forcestop",
    "car_breakmotor", "inventory_push", "inventory_pop", "fuckingbox_add", "fuckingbox_recompile", "fuckingbox_add_dest",
    "human_throwitem", "fuckingbox_getnumbox", "fuckingbox_getnumdest", "fuckingbox_move", "wagon_setevent", "wagon_getlastnode",
    "car_setspeccoll", "car_getspeccoll", "mission_objectives", "mission_objectivesremove", "mission_objectivesclear", "racing_mission6_init",
    "racing_mission6_start", "car_explosion", "human_fromcar", "[break]", "car_remove_driver", "setmodeltocar",
    "getenemyaistate", "get_pm_state", "get_pm_crashtime", "get_pm_firetime", "get_pm_numpredators", "get_pm_humanstate",
    "use_lightcache", "stream_create", "stream_connect", "stream_play", "stream_pause", "stream_stop",
    "stream_destroy", "stream_fadevol", "stream_setloop", "stream_setpos", "stream_getpos", "settankhitcount",
    "setnoanimhit", "carlight_main", "carlight_light", "carlight_indic_l", "carlight_indic_r", "carlight_indic_off",
    "car_muststeal", "car_setprojector", "garage_cansteal", "garage_addlaststolen", "garage_show", "car_inwater",
    "human_forcefall", "create_physicalobject", "destroy_physicalobject", "weather_reset", "weather_setparam", "getticktime",
    "getgametime", "getactiveplayer", "getangleactortoactor", "setcitytrafficvisible", "showcardamage", "character_push",
    "character_pop", "console_addtext", "camera_setrange", "actor_duplicate", "actor_delete", "human_reset",
    "human_havebox", "human_havebody", "human_stoptalk", "enemy_block", "enemy_unblock", "get_remote_float",
    "get_remote_frame", "get_remote_actor", "set_remote_float", "set_remote_frame", "set_remote_actor", "getactorframe",
    "zatmyse", "cardamagevisible", "[debug_me]", "[debug_stop]", "ifplayerstealcar", "intro_subtitle_add",
    "subtitle_add", "math_cos", "math_sin", "math_acos", "math_asin", "math_atan",
    "math_abs", "frm_getdir", "frm_setdir", "frm_getpos", "frm_setpos", "frm_getscale",
    "frm_setscale", "frm_getrot", "frm_setrot", "frm_getworlddir", "frm_getworldpos", "frm_getworldscale",
    "frm_getworldrot", "frm_getlocalmatrix", "frm_getworldmatrix", "vect_set", "vect_copy", "vect_mul_scl",
    "vect_mul_vect", "vect_mul_quat", "vect_mul_matrix", "vect_add_vect", "vect_sub_vect", "vect_cross",
    "vect_normalize", "vect_magnitude", "vect_inverse", "vect_angleto", "vect_angleto_2d", "vect_rotate_2d",
    "matrix_zero", "matrix_identity", "matrix_copy", "matrix_inverse", "matrix_mul", "quat_copy",
    "quat_make", "quat_setdir", "quat_extract", "quat_inverse", "quat_getrotmatrix", "quat_add",
    "quat_sub", "quat_mul_scl", "quat_mul_quat", "quat_dot", "quat_normalize", "quat_rotbymatrix",
    "quat_slerp", "coll_testline", "vlvp", "ctrl_read", "ctrl_readex", "getframefromactor",
    "actorupdateplacement", "racing_autoinvisible", "setplayerwfallevent", "car_invisible", "car_switchshowenergy", "debug_getframeinfo",
    "policeitchforplayer", "noanimpreload", "compareactors", "setnullactor", "compareframes", "setnullframe",
    "setfreeride", "enablemap", "frameistelephone", "car_calm", "bridge_shutdown", "settraffsectorsnd",
    "citymusic_off", "citymusic_on", "[dan_debug]", "frm_getparent", "ispointinsector", "ispointinarea",
    "getactivecamera", "sound_getvolume", "sound_setvolume", "racing_change_model", "citycaching_tick", "actorlightnesson",
    "car_lock", "car_lock_all", "pm_showsymbol", "pm_showprogress", "pm_setprogress", "setfilmmusic",
    "getfilmmusic", "human_getseatidx", "human_entertotruck", "floatreg_push", "floatreg_pop", "airplaneshowdamage",
    "setnpckillevent", "car_getseatcount", "frm_setalpha", "human_throwgrenade", "detector_createdyncoll", "detector_erasedyncoll",
    "car_unbreakable", "phobj_impuls", "weather_preparebuffer", "subtitle", "human_candie", "version_is_germany",
    "version_is_editor", "garage_nezahazuj", "police_support", "play_avi_intro", "camera_setfov", "camera_getfov",
    "door_getstate", "car_getmaxlevels", "car_getactlevel", "car_setactlevel", "freeride_scoreon", "freeride_scoreset",
    "freeride_scoreget", "freeride_scoreadd", "taxidriver_enable", "human_returntotraff", "timer_getinterval", "timer_setinterval",
    "frm_getnumchildren", "frm_getchild", "human_waittoready", "actor_adddorici", "car_repair", "police_speed_factor",
    "cartridge_invalidate", "npc_shutup", "freeride_enablecar", "getcarlinenumfromtable", "enemy_gethostilestate", "human_shutup",
    "pumper_canwork", "group_disband", "inventory_clear", "car_disable_uo", "getlastsavenum", "freeride_traffsetup",
    "poczkurvenychbedencar", "human_getactanimid", "car_setaxis", "m7_zastavzkurvenepolise", "isvalidtaxipassenger", "graber_setfocus",
    "dan_internal_1", "dan_internal_2",
};
static const char* OpName(int op) { return op >= 0 && op < 398 ? kOpName[op] : "?"; }


// actor kind (+0x10) -> readable name (table at 0x63e588 + PS2 ctors)
static const char* KindName(int k) {
    switch (k) {
    case 1: return "actor"; case 2: return "player"; case 3: return "human"; case 4: return "car";
    case 5: return "detector"; case 6: return "door"; case 8: return "railway"; case 9: return "box";
    case 0xa: return "bottle"; case 0xc: return "traffic car"; case 0x11: return "dropout";
    case 0x12: return "traffic gen"; case 0x13: return "don_ocas"; case 0x14: return "bridge";
    case 0x15: return "dog"; case 0x16: return "airplane"; case 0x18: return "turnout";
    case 0x19: return "pumper"; case 0x1a: return "crash obj"; case 0x1b: return "enemy";
    case 0x1c: return "race camera"; case 0x20: return "physics"; case 0x21: return "actor shot";
    case 0x23: return "box_new"; case 0x24: return "car ext"; case 0x25: return "drop weapon";
    }
    return NULL;
}

// name + kind of every actor in the game's actor list (game +0xf0/+0xf4) near the camera
static int g_allDrawn = 0;
// Child frame i of a frame: count at frame+0x11c, child = vtable slot 0x30 (stdcall this, index) -
// the same call Game.exe makes everywhere it walks a frame's children (e.g. 0x407d2b, 0x430c12).
typedef uint8_t* (__attribute__((stdcall)) *Fn_GetChild)(void* frame, int index);
static uint8_t* g_lsEnd = NULL;
static bool InLS3D(const void* p) {
    if (!g_ls3d) return false;
    if (!g_lsEnd) {
        const uint8_t* nt = g_ls3d + *(const int32_t*)(g_ls3d + 0x3C);
        g_lsEnd = g_ls3d + *(const uint32_t*)(nt + 0x50);           // SizeOfImage
    }
    return (const uint8_t*)p >= g_ls3d && (const uint8_t*)p < g_lsEnd;
}
static uint8_t* FrameChild(uint8_t* fr, int i) {
    if (!Readable(fr, 0x120)) return NULL;
    uint32_t* vt = *(uint32_t**)fr;
    if (!InLS3D(vt) || !Readable(vt, 0x34)) return NULL;
    void* fn = (void*)(uintptr_t)vt[0x30 / 4];
    if (!InLS3D(fn)) return NULL;
    return ((Fn_GetChild)fn)(fr, i);
}
static int g_partDrawn = 0;
static void DrawParts(uint8_t* fr, V3 cam, int depth) {
    if (depth > 4 || g_partDrawn >= 160 || !Readable(fr, 0x120)) return;
    int n = *(const int*)(fr + 0x11c);
    if (n <= 0 || n > 64) return;
    for (int i = 0; i < n && g_partDrawn < 160; ++i) {
        uint8_t* c = FrameChild(fr, i);
        if (!c || !Readable(c, 0x120)) continue;
        char nm[48];
        V3 p;
        if (FrameName(c, nm, sizeof nm) && FramePos(c, p) && Len(Sub(p, cam)) < 40.0f) {
            Label(p, nm, 0xFFC8C8C8);
            ++g_partDrawn;
        }
        DrawParts(c, cam, depth + 1);
    }
}
// ---- names of the scene objects (scene.4ds + cache.bin: houses, trees, lamps, fences ...). The scene
// *(mission+0x10) is an I3D_frame; its tree is walked with the frame's own GetChild (vtable 0x30) every
// few seconds. Kept: named VISUAL (1) / MODEL (9) frames whose parent is not one of them (= whole
// objects, not their sub-meshes), and not the frame of an actor (those are labelled above).
static bool OffScreen(V3 c, float r);
static bool FiniteV(const float* f);
static void FocalSetup();
enum { SO_MAX = 40000 };
static uint8_t* g_so[SO_MAX]; static int g_nso = 0, g_nsoScene = 0;
static DWORD g_soT = 0; static uint8_t* g_soScene = NULL;
static void SoWalk(uint8_t* fr, int depth, bool inObj) {
    if (depth > 12 || g_nso >= SO_MAX || !Readable(fr, 0x120)) return;
    int t = *(const int*)(fr + 0x110);
    bool obj = (t == 1 || t == 9);
    if (obj && !inObj && g_nso < SO_MAX) {
        const char* nm = *(const char* const*)(fr + 0x100);
        if (Readable(nm, 2) && nm[0]) g_so[g_nso++] = fr;
    }
    int n = *(const int*)(fr + 0x11c);
    if (n <= 0 || n > 20000) return;
    for (int i = 0; i < n && g_nso < SO_MAX; ++i) {
        uint8_t* c = FrameChild(fr, i);
        if (c && c != fr) SoWalk(c, depth + 1, inObj || obj);
    }
}
static void DrawSceneNames(uint8_t* game, V3 cam) {
    uint8_t* ms = *(uint8_t**)A_MISSION;
    if (!Readable(ms, 0x14)) return;
    uint8_t* sc = *(uint8_t**)(ms + 0x10);
    if (!Readable(sc, 0x120)) return;
    FocalSetup();
    DWORD now = GetTickCount();
    if (sc != g_soScene || now - g_soT > 4000) {
        g_soScene = sc; g_soT = now; g_nso = 0;
        int n = *(const int*)(sc + 0x11c);
        for (int i = 0; i < n && i < 20000 && g_nso < SO_MAX; ++i) { uint8_t* c = FrameChild(sc, i); if (c && c != sc) SoWalk(c, 0, false); }
        uint8_t* root = *(uint8_t**)(sc + 0x210);                       // I3D_scene root frame
        if (g_nso == 0 && Readable(root, 0x120)) { int m = *(const int*)(root + 0x11c);
            for (int i = 0; i < m && i < 20000 && g_nso < SO_MAX; ++i) { uint8_t* c = FrameChild(root, i); if (c && c != root) SoWalk(c, 0, false); } }
        static int s_l = 0;
        if (s_l < 6) { ++s_l; char l[96]; l[0] = 0; SCat(l, sizeof l, "scene objects: "); SCatU(l, sizeof l, (uint32_t)g_nso); Log(l); }
        g_nsoScene = g_nso;
    }
    g_nso = g_nsoScene;                                             // cache.bin objects are added again every frame
    // actor frames: skip (their names are drawn with the kind)
    uint8_t** b = *(uint8_t***)(game + 0xf0); uint8_t** e = *(uint8_t***)(game + 0xf4);
    int na = (b && e > b) ? (int)(e - b) : 0; if (na > 20000 || (na && !Readable(b, (size_t)na * 4))) na = 0;
    const float R = 45.0f;
    struct Cand { float d; int i; };
    static Cand cd[2048]; int nc = 0;
    for (int i = 0; i < g_nso && nc < 2048; ++i) {
        uint8_t* fr = g_so[i];
        if (!Readable(fr, 0x120)) continue;
        V3 p = RdV3(fr + 0x40);
        if (!FiniteV(&p.x)) continue;
        float d = Len(Sub(p, cam));
        if (d > R || OffScreen(p, 2.0f)) continue;
        cd[nc].d = d; cd[nc].i = i; ++nc;
    }
    {   // cache.bin: blocks +0x04/+0x08 (stride 0x74), block +0x04 active, +0x0c/+0x10 vector<I3D_frame*> objects
        const uint8_t* bb = *(const uint8_t* const*)(uintptr_t)0x0063D26C, *be = *(const uint8_t* const*)(uintptr_t)0x0063D270;
        uint32_t nb = (bb && be > bb) ? (uint32_t)((be - bb) / 0x74) : 0;
        if (nb > 20000 || (nb && !Readable(bb, (size_t)nb * 0x74))) nb = 0;
        static int s_cl = 0;
        if (s_cl < 4 && nb) { ++s_cl; char l[96]; l[0] = 0; SCat(l, sizeof l, "cache.bin: "); SCatU(l, sizeof l, nb); SCat(l, sizeof l, " blocks"); Log(l); }
        for (uint32_t i = 0; i < nb && nc < 2048; ++i) {
            const uint8_t* blk = bb + i * 0x74;
            if (!blk[4]) continue;                                      // block not streamed in
            uint8_t* const* ob = *(uint8_t* const* const*)(blk + 0x0c); uint8_t* const* oe = *(uint8_t* const* const*)(blk + 0x10);
            uint32_t no = (ob && oe > ob) ? (uint32_t)(oe - ob) : 0;
            if (no > 5000 || (no && !Readable(ob, no * 4))) continue;
            for (uint32_t k = 0; k < no && nc < 2048; ++k) {
                uint8_t* fr = ob[k];
                if (!Readable(fr, 0x120) || g_nso >= SO_MAX) continue;
                V3 p; if (!FramePos(fr, p)) continue;
                float d = Len(Sub(p, cam));
                if (d > R || OffScreen(p, 2.0f)) continue;
                g_so[g_nso] = fr; cd[nc].d = d; cd[nc].i = g_nso; ++nc; ++g_nso;
            }
        }
    }
    for (int a = 1; a < nc; ++a) { Cand x = cd[a]; int j = a - 1; while (j >= 0 && cd[j].d > x.d) { cd[j + 1] = cd[j]; --j; } cd[j + 1] = x; }
    int drawn = 0;
    for (int k = 0; k < nc && drawn < 150; ++k) {
        uint8_t* fr = g_so[cd[k].i];
        bool isAct = false;
        for (int j = 0; j < na && !isAct; ++j) isAct = Readable(b[j], 0x6c) && *(uint8_t* const*)(b[j] + 0x68) == fr;
        if (isAct) continue;
        char nm[64]; if (!FrameName(fr, nm, sizeof nm)) continue;
        V3 at = RdV3(fr + 0x40); at.y += 1.0f;
        Label(at, nm, 0xFFB8C8D8);
        ++drawn;
    }
}
static void DrawAllNames(uint8_t* game, V3 cam) {
    g_partDrawn = 0;
    g_allDrawn = 0;
    uint8_t** b = *(uint8_t***)(game + 0xf0);
    uint8_t** e = *(uint8_t***)(game + 0xf4);
    if (!b || e < b) return;
    int n = (int)(e - b); if (n > 20000 || !Readable(b, (size_t)n * 4)) return;
    for (int i = 0; i < n && g_allDrawn < 400; ++i) {
        uint8_t* a = b[i];
        if (!Readable(a, 0x70)) continue;
        int kind = *(int*)(a + 0x10);
        if (kind == 5 && C.showLabels) continue;            // detectors are labelled by the trigger view
        V3 p; if (!ActorPos(a, p)) continue;
        if (Len(Sub(p, cam)) > C.allDist) continue;
        char nm[64]; if (!FrameName(*(uint8_t* const*)(a + 0x68), nm, sizeof nm)) continue;
        char l[48]; l[0] = 0;
        const char* kn = KindName(kind);
        if (kn) SCat(l, sizeof l, kn); else { SCat(l, sizeof l, "kind "); SCatU(l, sizeof l, (uint32_t)kind); }
        V3 at = p; at.y += 1.0f;
        Label(at, l, C.colAll, nm);
        ++g_allDrawn;
        if ((kind == 4 || kind == 0xc) && Len(Sub(p, cam)) < 40.0f)     // car / traffic car: label its parts
            DrawParts(*(uint8_t* const*)(a + 0x68), cam, 0);
    }
}

static void DrawVolumes(uint8_t* det, V3 cam, const char* name) {
    int cnt = *(int*)(det + 0x124);
    uint8_t** colls = *(uint8_t***)(det + 0x118);
    const uint8_t* world = *(uint8_t* const*)(det + 0x11c);
    if (cnt <= 0 || cnt > 64 || !Readable(colls, (size_t)cnt * 4)) return;
    bool live = *(uint8_t*)(det + 0xdf) != 0;
    uint32_t col = WithAlpha(C.colVolume, live ? 1.0f : 0.45f);
    for (int i = 0; i < cnt; ++i) {
        const uint8_t* v = colls[i];
        if (!Readable(v, 0x48)) continue;
        V3 c = RdV3(v + 0x10); float r = RdF(v, 7);
        if (!SaneR(r)) continue;
        if (Len(Sub(c, cam)) - r > C.maxDist) continue;
        bool boxDone = false;
        if (world && Readable(world + i * 0x40, 0x40)) {
            const float* m = (const float*)(world + i * 0x40);
            V3 rx = { m[0], m[1], m[2] }, ry = { m[4], m[5], m[6] }, rz = { m[8], m[9], m[10] }, t = { m[12], m[13], m[14] };
            V3 lo = RdV3(v + 0x24), hi = RdV3(v + 0x30);
            if (Len(Sub(t, c)) < r + 2.0f && Len(rx) > 1e-4f && Len(ry) > 1e-4f && Len(rz) > 1e-4f
                && hi.x >= lo.x && hi.y >= lo.y && hi.z >= lo.z && Len(Sub(hi, lo)) < 4 * r + 1) {
                rx = Norm(rx); ry = Norm(ry); rz = Norm(rz);
                V3 k[8];
                for (int j = 0; j < 8; ++j) {
                    float x = (j & 1) ? hi.x : lo.x, y = (j & 2) ? hi.y : lo.y, z = (j & 4) ? hi.z : lo.z;
                    k[j] = Add(t, Add(Mul(rx, x), Add(Mul(ry, y), Mul(rz, z))));
                }
                static const uint8_t E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
                for (int j = 0; j < 12; ++j) Line3(k[E[j][0]], k[E[j][1]], col);
                boxDone = true;
            }
        }
        if (!boxDone) WireSphere(c, r, col);
        if (C.showLabels) {
            char l[64]; l[0] = 0; SCat(l, sizeof l, live ? "VOLUME" : "VOLUME OFF");
            V3 at = c; at.y += boxDone ? 0.3f : r;
            Label(at, l, col, name);
        }
        ++g_drawn;
    }
}

// ------------------------------------------------------------------ static collision (tree.klz)
//  g_collision singleton @ 0x00647F48 (1.2), loaded level = *(singleton + 0x284) (NULL = nothing loaded)
//  level: +0x30/+0x34 faces  (0x20: u32 flags|link<<16, 3 vertex ptrs, normal xyz, d)
//         +0x40/+0x44 AABB   (0x20: flags, frame, min3, max3)
//         +0x38/+0x3c XTOBB  (0xb8: flags, frame, aabb min3 max3, extent min3 max3, M 4x4, Minv 4x4)
//         +0x50/+0x54 OBB    (0xa0: flags, frame, extent min3 max3, M 4x4, Minv 4x4)
//         +0x48/+0x4c sphere (0x18: flags, frame, pos3, radius)
//         +0x58/+0x5c cyl.   (0x14: flags, frame, pos x z, radius - infinite vertical)
//  Layout: collision_load_level 0x0057dca0, sizes match the tree.klz record sizes.
static const uint32_t A_COLL_LEVEL = 0x00647F48 + 0x284;   // Game.exe 1.2 g_collision (1.0 export had 0x0067A588); load fn 0x005C2B70 writes +0x284 @0x5C2D19
static struct {
    int show, key, wire, prims, depth, cull, labels, xray; float dist, fillA, width, bias;
    uint32_t colFace, colEdge, colPrim;
} CC;
static void LoadCollCfg() {
    CC.show    = IniInt("ShowCollision", 0);
    CC.key     = IniInt("CollisionKey", 0x77);            // F8
    CC.wire    = IniInt("CollisionWire", 1);
    CC.prims   = 1;                                       // boxes / spheres / cylinders always drawn
    CC.dist    = IniFloat("CollisionDistance", 1000.0f);  // the whole level (the depth test hides what is not seen)
    CC.fillA   = 0;                                       // outlines only (user: no fill)
    CC.width   = IniFloat("CollisionLineWidth", 1.0f);
    CC.depth   = IniInt("CollisionDepthTest", 1);          // 1 = hidden behind walls (z-buffer), 0 = see through
    CC.bias    = IniFloat("CollisionDepthBias", 0.03f);    // metres pulled toward the camera so lines on a wall pass the test
    CC.cull    = IniInt("CollisionCullBack", 1);           // 1 = skip faces turned away (far side of thin walls showed through)
    CC.labels  = IniInt("CollisionLabels", 0);                     // material labels on the faces around + what the screen centre looks at
    CC.xray    = IniInt("CollisionSeeThrough", 0);                 // 1 = every collision through walls / ground (no depth test, no culling)
    CC.colFace = 0xFF30C0FF;
    CC.colEdge = 0xFFE8E040;                               // mesh outlines: yellow
    CC.colPrim = 0xFF40E0FF;                               // boxes / spheres / cylinders: cyan
    if (CC.width < 0.3f) CC.width = 0.3f; if (CC.width > 6) CC.width = 6;
    if (CC.dist < 5) CC.dist = 5; if (CC.dist > 1000) CC.dist = 1000;
}
struct CTri { V3 a, b, c, n, m; float r, shade; uint32_t emask; int nb[3]; uint32_t prop; };   // nb = neighbour over edge k, prop = face flags dword (material byte 0)   // m/r = bounding sphere, emask bit k = draw edge k (v[k] -> v[k+1])
static CTri*    g_ct = NULL;
static int      g_nct = 0, g_ctCap = 0;
static uint8_t* g_ctLevel = NULL;
static uint32_t g_ctFaces = 0;
static int      g_collDrawn = 0;

static bool FiniteV(const float* f) {
    for (int i = 0; i < 3; ++i) if (!(f[i] == f[i]) || f[i] > 1e6f || f[i] < -1e6f) return false;
    return true;
}
static uint8_t* CollLevel() {
    if (!Readable((void*)A_COLL_LEVEL, 4)) return NULL;
    uint8_t* lv = *(uint8_t**)A_COLL_LEVEL;
    return Readable(lv, 0x68) ? lv : NULL;
}
// Outline edges only: an edge shared by two (nearly) coplanar faces is a triangulation seam
// (the diagonal of a wall quad) - hide it. A shared edge is drawn once, by the first face.
// Returns the number of hidden edges.
struct CEdge { int32_t q[6]; int tri; int k; };
static inline int32_t QPos(float v) { return (int32_t)(v * 50.0f + (v < 0 ? -0.5f : 0.5f)); }   // 2 cm grid
static int BuildOutline() {
    if (g_nct <= 0) return 0;
    uint32_t cap = 1; while (cap < (uint32_t)g_nct * 6) cap <<= 1;
    CEdge* h = (CEdge*)VirtualAlloc(NULL, (size_t)cap * sizeof(CEdge), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!h) return 0;
    for (uint32_t i = 0; i < cap; ++i) h[i].tri = -1;
    int hidden = 0;
    for (int i = 0; i < g_nct; ++i) {
        CTri& t = g_ct[i];
        const V3* v[3] = { &t.a, &t.b, &t.c };
        for (int k = 0; k < 3; ++k) {
            const V3* p0 = v[k]; const V3* p1 = v[(k + 1) % 3];
            int32_t A[3] = { QPos(p0->x), QPos(p0->y), QPos(p0->z) }, B[3] = { QPos(p1->x), QPos(p1->y), QPos(p1->z) };
            bool sw = A[0] != B[0] ? A[0] > B[0] : (A[1] != B[1] ? A[1] > B[1] : A[2] > B[2]);
            int32_t q[6];
            for (int j = 0; j < 3; ++j) { q[j] = sw ? B[j] : A[j]; q[3 + j] = sw ? A[j] : B[j]; }
            uint32_t hh = 2166136261u;
            for (int j = 0; j < 6; ++j) { hh ^= (uint32_t)q[j]; hh *= 16777619u; }
            uint32_t s = hh & (cap - 1);
            for (;;) {
                CEdge& e = h[s];
                if (e.tri < 0) { for (int j = 0; j < 6; ++j) e.q[j] = q[j]; e.tri = i; e.k = k; break; }
                bool same = true;
                for (int j = 0; j < 6 && same; ++j) same = e.q[j] == q[j];
                if (same) {
                    t.emask &= ~(1u << k);                              // partner already owns this edge
                    CTri& o = g_ct[e.tri];
                    t.nb[k] = e.tri; o.nb[e.k] = i;
                    float d = Dot(o.n, t.n);
                    if (d > 0.985f && (o.emask & (1u << e.k))) { o.emask &= ~(1u << e.k); ++hidden; }
                    break;
                }
                s = (s + 1) & (cap - 1);
            }
        }
    }
    VirtualFree(h, 0, MEM_RELEASE);
    return hidden;
}
// copy the face soup once per level into our own array (world coordinates, no pointer chasing per frame)
static void BuildCollCache(uint8_t* lv) {
    g_nct = 0; g_ctLevel = lv; g_ctFaces = 0;
    if (!lv) return;
    uint32_t n = *(uint32_t*)(lv + 0x30);
    uint8_t* f = *(uint8_t**)(lv + 0x34);
    g_ctFaces = n;
    if (!n || n > 2000000 || !Readable(f, (size_t)n * 0x20)) { Log("collision: no readable face array"); return; }
    if ((int)n > g_ctCap) {
        if (g_ct) VirtualFree(g_ct, 0, MEM_RELEASE);
        g_ct = (CTri*)VirtualAlloc(NULL, (size_t)n * sizeof(CTri), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_ctCap = g_ct ? (int)n : 0;
        if (!g_ct) { Log("collision: out of memory"); return; }
    }
    int bad = 0, skip = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t* r = (const uint32_t*)(f + i * 0x20);
        if (r[0] & 0x20) { ++skip; continue; }                    // degenerate (flagged by the loader)
        const float* p[3];
        bool ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            p[k] = (const float*)r[1 + k];
            ok = (uint32_t)p[k] > 0x10000 && Readable(p[k], 12) && FiniteV(p[k]);
        }
        if (!ok) { ++bad; continue; }
        CTri& t = g_ct[g_nct++];
        t.a = { p[0][0], p[0][1], p[0][2] }; t.b = { p[1][0], p[1][1], p[1][2] }; t.c = { p[2][0], p[2][1], p[2][2] };
        V3 nn = Cross(Sub(t.b, t.a), Sub(t.c, t.a)); float L = Len(nn);
        float ny = L > 1e-9f ? nn.y / L : 0; if (ny < 0) ny = -ny;
        t.shade = 0.55f + 0.45f * ny;                              // floors bright, walls darker
        t.n = L > 1e-9f ? Mul(nn, 1.0f / L) : nn;
        t.m = Mul(Add(Add(t.a, t.b), t.c), 1.0f / 3.0f);
        float ra = Len(Sub(t.a, t.m)), rb = Len(Sub(t.b, t.m)), rc = Len(Sub(t.c, t.m));
        t.r = ra > rb ? (ra > rc ? ra : rc) : (rb > rc ? rb : rc);
        t.emask = 7; t.nb[0] = t.nb[1] = t.nb[2] = -1; t.prop = r[0];
    }
    int hidden = BuildOutline();
    char l[200]; l[0] = 0;
    SCat(l, sizeof l, "collision level "); SCatX(l, sizeof l, (uint32_t)lv);
    SCat(l, sizeof l, ": faces "); SCatU(l, sizeof l, n);
    SCat(l, sizeof l, " cached "); SCatU(l, sizeof l, (uint32_t)g_nct);
    SCat(l, sizeof l, " degenerate "); SCatU(l, sizeof l, (uint32_t)skip);
    SCat(l, sizeof l, " unreadable "); SCatU(l, sizeof l, (uint32_t)bad);
    SCat(l, sizeof l, " inner-edges "); SCatU(l, sizeof l, (uint32_t)hidden);
    SCat(l, sizeof l, "  aabb "); SCatU(l, sizeof l, *(uint32_t*)(lv + 0x40));
    SCat(l, sizeof l, " xtobb "); SCatU(l, sizeof l, *(uint32_t*)(lv + 0x38));
    SCat(l, sizeof l, " obb "); SCatU(l, sizeof l, *(uint32_t*)(lv + 0x50));
    SCat(l, sizeof l, " sphere "); SCatU(l, sizeof l, *(uint32_t*)(lv + 0x48));
    SCat(l, sizeof l, " cyl "); SCatU(l, sizeof l, *(uint32_t*)(lv + 0x58));
    Log(l);
    if (g_nct) {
        l[0] = 0; SCat(l, sizeof l, "  first face v0 ");
        SCatF1(l, sizeof l, g_ct[0].a.x); SCat(l, sizeof l, " "); SCatF1(l, sizeof l, g_ct[0].a.y); SCat(l, sizeof l, " "); SCatF1(l, sizeof l, g_ct[0].a.z);
        SCat(l, sizeof l, "  camera "); SCatF1(l, sizeof l, g_cam.x); SCat(l, sizeof l, " "); SCatF1(l, sizeof l, g_cam.y); SCat(l, sizeof l, " "); SCatF1(l, sizeof l, g_cam.z);
        Log(l);
    }
}
// box given by local extents and a row-major 4x4 (row 3 = translation)
static void DrawBox(const float* mn, const float* mx, const float* M, uint32_t col) {
    V3 w[8]; float sx[8], sy[8]; bool all = true;
    for (int i = 0; i < 8; ++i) {
        float x = (i & 1) ? mx[0] : mn[0], y = (i & 2) ? mx[1] : mn[1], z = (i & 4) ? mx[2] : mn[2];
        if (M) w[i] = { x * M[0] + y * M[4] + z * M[8] + M[12], x * M[1] + y * M[5] + z * M[9] + M[13], x * M[2] + y * M[6] + z * M[10] + M[14] };
        else   w[i] = { x, y, z };
        if (!FiniteV(&w[i].x)) return;
        all = Project(w[i], sx[i], sy[i]) && all;
    }
    static const uint8_t E[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
    static const uint8_t F[6][4]  = { {0,1,3,2},{4,5,7,6},{0,1,5,4},{2,3,7,6},{0,2,6,4},{1,3,7,5} };
    V3 bc = FrontPoint(Mul(Add(w[0], w[7]), 0.5f), Len(Sub(w[7], w[0])) * 0.5f);
    (void)all; (void)bc;
    if (CC.fillA > 0) {                                             // boxes: lighter fill than the faces
        uint32_t f = WithAlpha(col, CC.fillA * 0.6f);
        V3 bcen = Mul(Add(w[0], w[7]), 0.5f);
        for (int i = 0; i < 6; ++i) {
            const uint8_t* q = F[i];
            V3 fn = Cross(Sub(w[q[1]], w[q[0]]), Sub(w[q[2]], w[q[0]]));
            V3 fc = Mul(Add(Add(w[q[0]], w[q[1]]), Add(w[q[2]], w[q[3]])), 0.25f);
            if (Dot(fn, Sub(fc, bcen)) < 0) fn = Mul(fn, -1.0f);           // outward
            if (Dot(fn, Sub(g_cam, fc)) <= 0) continue;                    // turned away: not filled
            FillTri3(w[q[0]], w[q[1]], w[q[2]], f); FillTri3(w[q[0]], w[q[2]], w[q[3]], f);
        }
    }
    for (int i = 0; i < 12; ++i) {
        SegW(w[E[i][0]], w[E[i][1]], col, col, g_w);
    }
}
static inline uint32_t CollFade(uint32_t c, V3 p, float D) {
    float d = Len(Sub(p, g_cam)) / D; if (d > 1) d = 1;
    return WithAlpha(c, 1.0f - d * d);
}
static inline bool Near(V3 p, float d2) { V3 d = Sub(p, g_cam); return Dot(d, d) < d2; }
// collision material names (byte 0 of the property dword); NULL = unused number
static const char* const kMatName[47] = {
    NULL, "Asfalt", "Pavement", "Pavement 2",
    "Concrete", "Grass", "Sand/Country road", "Clay/Road",
    "Gravel", "Rock", "Railroad track", "Leaves",
    NULL, "Wood 0%", "Wood 30%", "Wood 70%",
    "Parquet", "Marble/Stone", "Carpet", "Metal construction",
    "Metal surface 0%", "Metal surface 30%", "Metal surface 70%", NULL,
    "Breakable glass", "Bulletproof glass", NULL, "Fence metal",
    "Fence wood", "Fence glass", NULL, "Dead water",
    NULL, "Universal coll", NULL, "Body",
    "Lattice?", "Bath foam?", "Brick wall", NULL,
    "Dead-coll", "Crash-coll", "Straw?", NULL,
    "Alive water", "Mirror/Pool", "Country road 2?",
};
// "TYPE  N Material  flags NN" from a collision record's property dword (byte 0 material, byte 1 flags)
static void CollPropText(const char* type, uint32_t prop, char* l, size_t cap) {
    uint32_t m = prop & 0xFF, f = (prop >> 8) & 0xFF;
    l[0] = 0; SCat(l, cap, type); SCat(l, cap, "  ");
    SCatU(l, cap, m);
    if (m < 47 && kMatName[m]) { SCat(l, cap, " "); SCat(l, cap, kMatName[m]); }
    if (f) {                                                        // flags as short hex
        char hx[3] = { "0123456789ABCDEF"[f >> 4], "0123456789ABCDEF"[f & 15], 0 };
        SCat(l, cap, "  flags 0x"); SCat(l, cap, hx);
    }
}
static int g_primLab = 0;
// the collision face under the screen centre (ray along the camera's +z axis), -1 = none
static int PickFace(float maxd, float& tHit) {
    uint8_t* cf = GetCameraFrame();
    if (!cf || !Readable(cf, 0x50)) return -1;
    V3 o = RdV3(cf + 0x40), d = RdV3(cf + 0x30);
    float L = Len(d); if (!(L > 1e-6f)) return -1;
    d = Mul(d, 1.0f / L);
    int best = -1; tHit = maxd;
    for (int i = 0; i < g_nct; ++i) {
        const CTri& t = g_ct[i];
        V3 w = Sub(t.m, o); float tc = Dot(w, d);
        if (tc + t.r < 0 || tc - t.r > tHit) continue;
        if (Dot(w, w) - tc * tc > t.r * t.r) continue;             // ray misses the bounding sphere
        V3 e1 = Sub(t.b, t.a), e2 = Sub(t.c, t.a), pv = Cross(d, e2);
        float det = Dot(e1, pv); if (det > -1e-9f && det < 1e-9f) continue;
        float inv = 1.0f / det; V3 tv = Sub(o, t.a);
        float u = Dot(tv, pv) * inv; if (u < 0 || u > 1) continue;
        V3 qv = Cross(tv, e1);
        float v = Dot(d, qv) * inv; if (v < 0 || u + v > 1) continue;
        float th = Dot(e2, qv) * inv;
        if (th > 0.05f && th < tHit) { tHit = th; best = i; }
    }
    return best;
}
static int* s_ord = NULL; static int s_ordCap = 0;
static float g_focX = 1.0f, g_focY = 1.0f;                       // pixels per unit at w = 1
static void FocalSetup() {
    if (g_devMode) {
        const D3DMATRIX& m = g_vp;
        g_focX = FSqrt(m._11 * m._11 + m._21 * m._21 + m._31 * m._31) * 0.5f * g_vw;
        g_focY = FSqrt(m._12 * m._12 + m._22 * m._22 + m._32 * m._32) * 0.5f * g_vh;
    } else { g_focX = g_p11 * 0.5f * g_vw; g_focY = g_p22 * 0.5f * g_vh; }
    if (!(g_focX > 0)) g_focX = g_vw; if (!(g_focY > 0)) g_focY = g_vh;
}
// a bounding sphere entirely behind the camera or entirely outside the screen
static bool OffScreen(V3 c, float r) {
    float w = ClipW(c);
    if (!(w >= -r)) return true;
    if (w <= r) return false;                                       // straddles the camera plane
    float sx, sy; ProjRaw(c, w, sx, sy);
    float rx = r / w * g_focX + 4, ry = r / w * g_focY + 4;
    return sx + rx < g_vx || sx - rx > g_vx + g_vw || sy + ry < g_vy || sy - ry > g_vy + g_vh;
}
// Ground under a vertical collision cylinder (they are infinite in y): the lowest collision face
// straight below/above it, computed once per cylinder (a few per frame) and cached for the level.
static float* g_cylY = NULL; static uint32_t g_cylN = 0; static const uint8_t* g_cylLv = NULL; static int g_cylBudget = 0;
static float CylGround(uint32_t idx, float x, float z) {
    if (g_cylLv != g_ctLevel) {
        if (g_cylY) VirtualFree(g_cylY, 0, MEM_RELEASE);
        g_cylY = (float*)VirtualAlloc(NULL, 65536 * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        g_cylN = 65536; g_cylLv = g_ctLevel;
        if (g_cylY) for (uint32_t i = 0; i < g_cylN; ++i) g_cylY[i] = -1e30f;   // -1e30 = not computed
    }
    if (!g_cylY || idx >= g_cylN) return g_cam.y - 1.5f;
    if (g_cylY[idx] > -1e29f) return g_cylY[idx];
    if (g_cylBudget <= 0) { float nan = 0.0f; return nan / nan; }
    --g_cylBudget;
    float best = 1e30f;
    for (int i = 0; i < g_nct; ++i) {
        const CTri& t = g_ct[i];
        float dx = x - t.m.x, dz = z - t.m.z;
        if (dx * dx + dz * dz > t.r * t.r) continue;
        float d = (t.b.x - t.a.x) * (t.c.z - t.a.z) - (t.c.x - t.a.x) * (t.b.z - t.a.z);
        if (d > -1e-6f && d < 1e-6f) continue;                       // vertical face
        float u = ((x - t.a.x) * (t.c.z - t.a.z) - (t.c.x - t.a.x) * (z - t.a.z)) / d;
        float v = ((t.b.x - t.a.x) * (z - t.a.z) - (x - t.a.x) * (t.b.z - t.a.z)) / d;
        if (u < 0 || v < 0 || u + v > 1) continue;
        float y = t.a.y + u * (t.b.y - t.a.y) + v * (t.c.y - t.a.y);
        if (y < best) best = y;
    }
    if (best > 1e29f) { V3 pp; uint8_t* pl = GetPlayer(); best = (pl && ActorPos(pl, pp)) ? pp.y : g_cam.y - 1.5f; }
    g_cylY[idx] = best;
    return best;
}
static int g_collEnd = 0;                                           // collision vertices are g_v[0 .. g_collEnd)
// ---- Software occlusion. The game's own depth buffer is no use here: LS3D draws the far and the near
// part of the scene in two passes and clears the depth between them (and the W-buffer through
// d3d8to9 is not reliable either). So every frame the static collision mesh around the camera is
// rasterised into a small depth buffer (1/w, nearest wins) and our lines / labels test against it.
enum { SZ_W = 480, SZ_HMAX = 360 };
static float g_sz[SZ_W * SZ_HMAX];
static int   g_szH = 180;
static bool  g_szOK = false;
static float g_szSX = 1, g_szSY = 1;                                // screen px -> buffer px
static void SzTri(float x0, float y0, float i0, float x1, float y1, float i1, float x2, float y2, float i2) {
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area > -1e-6f && area < 1e-6f) return;
    float mnx = x0, mxx = x0, mny = y0, mxy = y0;
    if (x1 < mnx) mnx = x1; if (x1 > mxx) mxx = x1; if (x2 < mnx) mnx = x2; if (x2 > mxx) mxx = x2;
    if (y1 < mny) mny = y1; if (y1 > mxy) mxy = y1; if (y2 < mny) mny = y2; if (y2 > mxy) mxy = y2;
    int bx0 = (int)mnx, bx1 = (int)mxx + 1, by0 = (int)mny, by1 = (int)mxy + 1;
    if (bx0 < 0) bx0 = 0; if (by0 < 0) by0 = 0; if (bx1 > SZ_W - 1) bx1 = SZ_W - 1; if (by1 > g_szH - 1) by1 = g_szH - 1;
    if (bx0 > bx1 || by0 > by1) return;
    float ia = 1.0f / area;
    for (int y = by0; y <= by1; ++y) {
        float py = y + 0.5f;
        float* row = g_sz + y * SZ_W;
        for (int x = bx0; x <= bx1; ++x) {
            float px = x + 0.5f;
            float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * ia;
            float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * ia;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -0.02f || w1 < -0.02f || w2 < -0.02f) continue;  // small overlap closes the seams
            float iw = w0 * i0 + w1 * i1 + w2 * i2;
            if (iw > row[x]) row[x] = iw;
        }
    }
}
static void SzWorldTri(V3 a, V3 b, V3 c) {
    const float NW = 0.12f;
    V3 in[3] = { a, b, c }; float w[3] = { ClipW(a), ClipW(b), ClipW(c) };
    V3 q[4]; float qw[4]; int n = 0;
    for (int k = 0; k < 3; ++k) {
        int j = (k + 1) % 3;
        bool ik = w[k] >= NW, ij = w[j] >= NW;
        if (ik) { q[n] = in[k]; qw[n++] = w[k]; }
        if (ik != ij) { float u = (NW - w[k]) / (w[j] - w[k]); q[n] = Add(in[k], Mul(Sub(in[j], in[k]), u)); qw[n++] = NW; }
    }
    if (n < 3) return;
    float sx[4], sy[4], iw[4];
    for (int k = 0; k < n; ++k) {
        float x, y; ProjRaw(q[k], qw[k], x, y);
        sx[k] = (x - g_vx) * g_szSX; sy[k] = (y - g_vy) * g_szSY; iw[k] = 1.0f / qw[k];
    }
    for (int k = 1; k + 1 < n; ++k) SzTri(sx[0], sy[0], iw[0], sx[k], sy[k], iw[k], sx[k + 1], sy[k + 1], iw[k + 1]);
}
// point on the camera side of a solid (box centre -> its front surface), for the occlusion test
static V3 FrontPoint(V3 c, float r) {
    V3 d = Sub(g_cam, c); float L = Len(d);
    if (!(L > 0.01f)) return c;
    float m = r < 3.0f ? r : 3.0f; if (m > L - 0.3f) m = L - 0.3f; if (m < 0) m = 0;
    return Add(c, Mul(d, m / L));
}
static int g_rayTests = 0, g_rayHid = 0, g_raySoftDiff = 0;
enum { OCC_MAX = 65536 };
static int g_occF[OCC_MAX]; static int g_nOccF = 0;
static const float* g_occB[OCC_MAX]; static uint8_t g_occT[OCC_MAX]; static int g_nOccB = 0;
static void BuildSoftZ() {
    g_nOccF = g_nOccB = 0;
    g_szOK = false;
    if (g_vw < 16 || g_vh < 16) return;
    g_szH = (int)(SZ_W * g_vh / g_vw); if (g_szH < 16) g_szH = 16; if (g_szH > SZ_HMAX) g_szH = SZ_HMAX;
    g_szSX = SZ_W / g_vw; g_szSY = g_szH / g_vh;
    uint8_t* lv = CollLevel();
    uint32_t nf = lv ? *(uint32_t*)(lv + 0x30) : 0;
    if (lv != g_ctLevel || nf != g_ctFaces) BuildCollCache(lv);
    if (!lv || g_nct <= 0) return;
    for (int i = 0; i < SZ_W * g_szH; ++i) g_sz[i] = 0;
    FocalSetup();
    float R = C.maxDist + 20.0f;
    const float NW = 0.12f;
    for (int i = 0; i < g_nct; ++i) {
        const CTri& t = g_ct[i];
        if (Len(Sub(t.m, g_cam)) - t.r > R) continue;
        if (OffScreen(t.m, t.r)) continue;
        if (g_nOccF < OCC_MAX) g_occF[g_nOccF++] = i;
        SzWorldTri(t.a, t.b, t.c);
    }
    // the boxes of the collision file: building walls, crates, fences ... are mostly AABB / XTOBB / OBB
    struct { int cnt, arr, stride; } P[3] = { {0x40,0x44,0x20}, {0x38,0x3c,0xb8}, {0x50,0x54,0xa0} };
    for (int k = 0; k < 3; ++k) {
        uint32_t n = *(uint32_t*)(lv + P[k].cnt);
        uint8_t* a = *(uint8_t**)(lv + P[k].arr);
        if (!n || n > 200000 || !Readable(a, (size_t)n * P[k].stride)) continue;
        for (uint32_t i = 0; i < n; ++i) {
            const float* r = (const float*)(a + i * P[k].stride);
            const float *mn, *mx, *M;
            if (k == 0) { mn = r + 2; mx = r + 5; M = NULL; }
            else { mn = r + (k == 1 ? 8 : 2); mx = mn + 3; M = r + (k == 1 ? 14 : 8); }
            V3 w8[8]; bool ok = true;
            for (int c = 0; c < 8 && ok; ++c) {
                float x = (c & 1) ? mx[0] : mn[0], y = (c & 2) ? mx[1] : mn[1], z = (c & 4) ? mx[2] : mn[2];
                if (M) w8[c] = { x * M[0] + y * M[4] + z * M[8] + M[12], x * M[1] + y * M[5] + z * M[9] + M[13], x * M[2] + y * M[6] + z * M[10] + M[14] };
                else   w8[c] = { x, y, z };
                ok = FiniteV(&w8[c].x);
            }
            if (!ok) continue;
            V3 cen = Mul(Add(w8[0], w8[7]), 0.5f); float rad = Len(Sub(w8[7], w8[0])) * 0.5f;
            if (!(rad < 2000.0f)) continue;
            if (Len(Sub(cen, g_cam)) - rad > R) continue;
            if (OffScreen(cen, rad)) continue;
            if (g_nOccB < OCC_MAX) { g_occB[g_nOccB] = r; g_occT[g_nOccB++] = (uint8_t)k; }
            static const uint8_t F[6][4] = { {0,1,3,2},{4,5,7,6},{0,1,5,4},{2,3,7,6},{0,2,6,4},{1,3,7,5} };
            for (int f = 0; f < 6; ++f) {
                SzWorldTri(w8[F[f][0]], w8[F[f][1]], w8[F[f][2]]);
                SzWorldTri(w8[F[f][0]], w8[F[f][2]], w8[F[f][3]]);
            }
        }
    }
    g_szOK = true;
    {   // every 5 s: what the occlusion sees (for diagnosis from the log)
        static DWORD s_t = 0; DWORD now = GetTickCount();
        if (now - s_t > 5000) {
            s_t = now;
            int filled = 0; for (int i = 0; i < SZ_W * g_szH; ++i) if (g_sz[i] > 0) ++filled;
            float cz = g_sz[(g_szH / 2) * SZ_W + SZ_W / 2];
            char l[220]; l[0] = 0; SCat(l, sizeof l, "occlusion: covered "); SCatU(l, sizeof l, (uint32_t)(filled * 100 / (SZ_W * g_szH)));
            SCat(l, sizeof l, "% of screen, centre at "); SCatF1(l, sizeof l, cz > 0 ? 1.0f / cz : -1.0f);
            SCat(l, sizeof l, " m; labels tested "); SCatU(l, sizeof l, (uint32_t)g_rayTests);
            SCat(l, sizeof l, " hidden "); SCatU(l, sizeof l, (uint32_t)g_rayHid);
            SCat(l, sizeof l, " (ray/zbuf differ "); SCatU(l, sizeof l, (uint32_t)g_raySoftDiff); SCat(l, sizeof l, ")");
            SCat(l, sizeof l, "  cam "); SCatF1(l, sizeof l, g_cam.x); SCat(l, sizeof l, " "); SCatF1(l, sizeof l, g_cam.y); SCat(l, sizeof l, " "); SCatF1(l, sizeof l, g_cam.z);
            Log(l);
            g_rayTests = g_rayHid = g_raySoftDiff = 0;
        }
    }
}
// ---- exact visibility for labels: the segment camera -> point against every collision face and box.
// Faces: sphere pre-test + Moller-Trumbore. Boxes: slab test in the box's local space (Minv).
static bool SegBox(V3 a, V3 b, const float* mn, const float* mx, const float* Mi) {
    if (Mi) {
        V3 la = { a.x * Mi[0] + a.y * Mi[4] + a.z * Mi[8] + Mi[12], a.x * Mi[1] + a.y * Mi[5] + a.z * Mi[9] + Mi[13], a.x * Mi[2] + a.y * Mi[6] + a.z * Mi[10] + Mi[14] };
        V3 lb = { b.x * Mi[0] + b.y * Mi[4] + b.z * Mi[8] + Mi[12], b.x * Mi[1] + b.y * Mi[5] + b.z * Mi[9] + Mi[13], b.x * Mi[2] + b.y * Mi[6] + b.z * Mi[10] + Mi[14] };
        a = la; b = lb;
    }
    float t0 = 0, t1 = 1;
    const float* pa = &a.x; const float* pb = &b.x;
    for (int k = 0; k < 3; ++k) {
        float d = pb[k] - pa[k];
        if (d > -1e-9f && d < 1e-9f) { if (pa[k] < mn[k] || pa[k] > mx[k]) return false; continue; }
        float u = (mn[k] - pa[k]) / d, v = (mx[k] - pa[k]) / d;
        if (u > v) { float x = u; u = v; v = x; }
        if (u > t0) t0 = u; if (v < t1) t1 = v;
        if (t0 > t1) return false;
    }
    return true;
}
static bool RayBlocked(V3 a, V3 p) {
    if (!g_szOK) return false;
    V3 d = Sub(p, a); float L = Len(d);
    if (!(L > 0.6f)) return false;
    V3 b = Add(a, Mul(d, (L - 0.5f) / L));                         // stop 0.5 m before the point
    V3 dir = Mul(d, 1.0f / L); float LL = L - 0.5f;
    for (int j = 0; j < g_nOccF; ++j) {
        const CTri& t = g_ct[g_occF[j]];
        V3 w = Sub(t.m, a); float tc = Dot(w, dir);
        if (tc + t.r < 0 || tc - t.r > LL) continue;
        if (Dot(w, w) - tc * tc > t.r * t.r) continue;
        V3 e1 = Sub(t.b, t.a), e2 = Sub(t.c, t.a), pv = Cross(dir, e2);
        float det = Dot(e1, pv); if (det > -1e-9f && det < 1e-9f) continue;
        float inv = 1.0f / det; V3 tv = Sub(a, t.a);
        float u = Dot(tv, pv) * inv; if (u < 0 || u > 1) continue;
        V3 qv = Cross(tv, e1);
        float v = Dot(dir, qv) * inv; if (v < 0 || u + v > 1) continue;
        float th = Dot(e2, qv) * inv;
        if (th > 0.05f && th < LL) return true;
    }
    for (int j = 0; j < g_nOccB; ++j) {
        const float* r = g_occB[j]; int k = g_occT[j];
        const float *mn, *mx, *Mi;
        if (k == 0) { mn = r + 2; mx = r + 5; Mi = NULL; }
        else if (k == 1) { mn = r + 8; mx = r + 11; Mi = r + 30; }
        else { mn = r + 2; mx = r + 5; Mi = r + 24; }
        if (Mi && !FiniteV(Mi + 12)) continue;
        if (SegBox(a, b, mn, mx, Mi)) return true;
    }
    return false;
}
static bool LabelVisible(V3 p) {
    ++g_rayTests;
    bool sv = SoftVisible(p);
    if (!sv) { ++g_rayHid; return false; }                         // the z buffer already says hidden
    bool hid = RayBlocked(g_cam, p);
    if (hid) { ++g_rayHid; ++g_raySoftDiff; }
    return !hid;
}
// true = p is not behind the collision geometry. Lenient: the FARTHEST occluder of the 3x3 pixels
// around counts, plus a bias so lines lying on a wall / floor stay visible.
static bool SoftVisible(V3 p) {
    if (!g_szOK) return true;
    float w = ClipW(p);
    if (!(w >= 0.12f)) return true;
    float x, y; ProjRaw(p, w, x, y);
    int bx = (int)((x - g_vx) * g_szSX), by = (int)((y - g_vy) * g_szSY);
    if (bx < 0 || by < 0 || bx >= SZ_W || by >= g_szH) return true;
    // majority of the 3x3 pixels around: hidden when at least 5 of them hold something nearer than
    // the point (minus a bias so lines lying on a wall / floor stay visible). One empty or farther
    // pixel (an edge of a wall, a gap between boxes) no longer makes everything behind visible.
    float lim = w - (0.3f + 0.015f * w);
    if (lim <= 0.12f) return true;
    float ivLim = 1.0f / lim;                                       // occluder 1/w above this = nearer
    int blocked = 0, total = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        int yy = by + dy; if (yy < 0 || yy >= g_szH) continue;
        for (int dx = -1; dx <= 1; ++dx) {
            int xx = bx + dx; if (xx < 0 || xx >= SZ_W) continue;
            ++total;
            if (g_sz[yy * SZ_W + xx] > ivLim) ++blocked;
        }
    }
    return blocked * 2 <= total;
}
// A box the player stands inside: the player cannot be inside solid collision, so it is either a
// collision the player ignores (cars / bullets only?) or read wrongly. Drawn magenta + logged once.
static const float* g_boxLogged[64]; static int g_nBoxLogged = 0;
static bool InBoxLocal(V3 p, const float* mn, const float* mx, const float* Mi) {
    if (Mi) { V3 l = { p.x * Mi[0] + p.y * Mi[4] + p.z * Mi[8] + Mi[12], p.x * Mi[1] + p.y * Mi[5] + p.z * Mi[9] + Mi[13], p.x * Mi[2] + p.y * Mi[6] + p.z * Mi[10] + Mi[14] }; p = l; }
    return p.x > mn[0] && p.x < mx[0] && p.y > mn[1] && p.y < mx[1] && p.z > mn[2] && p.z < mx[2];
}
static void BoxDiag(int k, const float* r, int nf) {
    for (int i = 0; i < g_nBoxLogged; ++i) if (g_boxLogged[i] == r) return;
    if (g_nBoxLogged < 64) g_boxLogged[g_nBoxLogged++] = r;
    static const char* const kN[3] = { "AABB", "XTOBB", "OBB" };
    char l[600]; l[0] = 0; SCat(l, sizeof l, "PLAYER INSIDE "); SCat(l, sizeof l, kN[k]);
    char t[64]; CollPropText("", *(const uint32_t*)r, t, sizeof t); SCat(l, sizeof l, t);
    SCat(l, sizeof l, " prop "); SCatX(l, sizeof l, *(const uint32_t*)r);
    { char fn[48]; if (FrameName(*(const uint8_t* const*)(r + 1), fn, sizeof fn)) { SCat(l, sizeof l, " frame '"); SCat(l, sizeof l, fn); SCat(l, sizeof l, "'"); } }
    SCat(l, sizeof l, " raw:");
    for (int i = 2; i < nf && SLen(l) < 560; ++i) { SCat(l, sizeof l, " "); SCatF1(l, sizeof l, r[i]); }
    Log(l);
}
static void DrawCollision() {
    g_collDrawn = 0; g_primLab = 0; g_cylBudget = 48;
    if (g_ctLevel != CollLevel()) g_nBoxLogged = 0;
    if (!CC.show) return;
    { uint8_t* cf = GetCameraFrame(); uint8_t* pl = GetPlayer();
      if (cf) g_cam = RdV3(cf + 0x40); else if (pl) ActorPos(pl, g_cam); }
    uint8_t* lv = CollLevel();
    uint32_t nf = lv ? *(uint32_t*)(lv + 0x30) : 0;
    if (lv != g_ctLevel || nf != g_ctFaces) BuildCollCache(lv);
    if (!lv) { static bool s_l = false; if (!s_l) { s_l = true; Log("collision: no level loaded (g_collision+0x284 is NULL)"); } return; }
    float D = CC.dist, D2 = D * D;
    float fa = CC.fillA;
    // Pass 1: faces in range and on screen, bucketed by distance. Pass 2 draws them nearest first,
    // so when the vertex buffer fills up it is the far faces that are dropped, not the ones around you.
    enum { NB = 64 };
    if (s_ordCap < g_nct) {
        if (s_ord) VirtualFree(s_ord, 0, MEM_RELEASE);
        s_ord = (int*)VirtualAlloc(NULL, (size_t)g_nct * 2 * sizeof(int) + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        s_ordCap = s_ord ? g_nct : 0;
        if (!s_ord) return;
    }
    int* cand = s_ord; int* ord = s_ord + s_ordCap;
    int cnt[NB + 1]; for (int k = 0; k <= NB; ++k) cnt[k] = 0;
    int nc = 0;
    FocalSetup();
    for (int i = 0; i < g_nct; ++i) {
        const CTri& t = g_ct[i];
        float dm = Len(Sub(t.m, g_cam));
        if (dm - t.r > D) continue;                                  // whole triangle out of range
        if (OffScreen(t.m, t.r)) continue;                           // behind the camera or outside the view
        int bk = (int)(dm / D * NB); if (bk < 0) bk = 0; if (bk >= NB) bk = NB - 1;
        cand[nc++] = i | (bk << 24); ++cnt[bk + 1];
    }
    for (int k = 1; k <= NB; ++k) cnt[k] += cnt[k - 1];
    for (int k = 0; k < nc; ++k) { int bk = (uint32_t)cand[k] >> 24; ord[cnt[bk]++] = cand[k] & 0xFFFFFF; }
    for (int o = 0; o < nc && g_nv < MAX_VTX * 2 / 3; ++o) {
        const int i = ord[o];
        const CTri& t = g_ct[i];
        float dm = Len(Sub(t.m, g_cam));
        bool front = CC.xray || !CC.cull || g_zOn || Dot(t.n, Sub(g_cam, t.a)) >= 0;
        bool any = false;
        if (fa > 0 && Dot(t.n, Sub(g_cam, t.a)) >= 0) {
            {
                float d = dm / D; if (d > 1) d = 1;
                uint32_t c = CC.colEdge;                                 // fill in the outline colour
                uint32_t r = (uint32_t)(((c >> 16) & 0xFF) * t.shade), g = (uint32_t)(((c >> 8) & 0xFF) * t.shade), b = (uint32_t)((c & 0xFF) * t.shade);
                uint32_t f = WithAlpha((r << 16) | (g << 8) | b | 0xFF000000, fa); (void)d;
                FillTri3(t.a, t.b, t.c, f);
                any = true;
            }
        }
        if (CC.wire && t.emask) {
            uint32_t c = CC.colEdge;
            float sh = 0.7f + 0.3f * t.shade;                        // floor outlines a bit brighter than walls
            uint32_t base = (c & 0xFF000000) | ((uint32_t)(((c >> 16) & 0xFF) * sh) << 16) | ((uint32_t)(((c >> 8) & 0xFF) * sh) << 8) | (uint32_t)((c & 0xFF) * sh);
            const V3* v[3] = { &t.a, &t.b, &t.c };
            for (int k = 0; k < 3; ++k) {
                if (!(t.emask & (1u << k))) continue;
                if (!front && (t.nb[k] < 0 || Dot(g_ct[t.nb[k]].n, Sub(g_cam, t.a)) < 0)) continue;   // both sides turned away
                V3 p0 = *v[k], p1 = *v[(k + 1) % 3];
                float L = Len(Sub(p1, p0));
                int n = (int)(L / 25.0f) + 1; if (n > 4) n = 4;       // long edges in pieces (near-plane clipping)
                V3 prev = p0; uint32_t cp = base;
                for (int j = 1; j <= n; ++j) {
                    V3 p = Add(p0, Mul(Sub(p1, p0), (float)j / (float)n));
                    uint32_t cn = base;
                    if ((cp | cn) >> 24) SegW(prev, p, cp, cn, g_w);
                    prev = p; cp = cn;
                }
            }
            any = true;
        }
        if (any) ++g_collDrawn;
    }
    if (CC.labels) {
        float th; int fi = PickFace(D, th);
        // material name on the faces around (nearest first, one label per material within 6 m)
        float LD = D < 25 ? D : 25;
        V3 at[56]; uint32_t am[56]; int na = 0;
        for (int o = 0; o < nc && na < 56; ++o) {
            const int i = ord[o];
            if (i == fi) continue;
            const CTri& t = g_ct[i];
            V3 d = Sub(t.m, g_cam);
            float dm = Len(d);
            if (dm > LD) { if (dm - D / NB > LD) break; continue; }
            if (!CC.xray && Dot(t.n, Sub(g_cam, t.a)) < 0) continue;
            if (ClipW(t.m) < 0.5f) continue;
            uint32_t m = t.prop & 0xFF;
            bool dup = false;
            for (int j = 0; j < na && !dup; ++j) { V3 e = Sub(at[j], t.m); dup = am[j] == m && Dot(e, e) < 36.0f; }
            if (dup) continue;
            at[na] = t.m; am[na] = m; ++na;
            char l[32]; l[0] = 0;
            if (m < 47 && kMatName[m]) SCat(l, sizeof l, kMatName[m]); else { SCat(l, sizeof l, "mat "); SCatU(l, sizeof l, m); }
            Label(t.m, l, CC.colEdge);
        }
        if (fi >= 0) {
            const CTri& t = g_ct[fi];
            uint32_t hc = 0xFFFFFF40;
            Line3(t.a, t.b, hc); Line3(t.b, t.c, hc); Line3(t.c, t.a, hc);
            char l[64], top[48]; CollPropText("MESH", t.prop, l, sizeof l);
            SCat(l, sizeof l, "  "); SCatF1(l, sizeof l, th); SCat(l, sizeof l, " m");
            top[0] = 0; SCat(top, sizeof top, "face #"); SCatU(top, sizeof top, (uint32_t)fi);
            Label(t.m, l, hc, top);
        }
    }
    if (!CC.prims) return;
    uint32_t pc = CC.colPrim;
    V3 plp = { 0, -1e6f, 0 }; { uint8_t* pl = GetPlayer(); if (pl) ActorPos(pl, plp); plp.y += 0.9f; }   // player's hip
    struct { int cnt, arr, stride; } P[5] = { {0x40,0x44,0x20}, {0x38,0x3c,0xb8}, {0x50,0x54,0xa0}, {0x48,0x4c,0x18}, {0x58,0x5c,0x14} };
    for (int k = 0; k < 5; ++k) {
        uint32_t n = *(uint32_t*)(lv + P[k].cnt);
        uint8_t* a = *(uint8_t**)(lv + P[k].arr);
        if (!n || n > 200000 || !Readable(a, (size_t)n * P[k].stride)) continue;
        for (uint32_t i = 0; i < n && g_nv < MAX_VTX * 3 / 4; ++i) {
            const float* r = (const float*)(a + i * P[k].stride);
            V3 lc = { 0, 0, 0 };
            switch (k) {
            case 0: {                                              // AABB
                V3 c = { (r[2] + r[5]) * 0.5f, (r[3] + r[6]) * 0.5f, (r[4] + r[7]) * 0.5f };
                if (!FiniteV(&c.x)) continue;
                { V3 e = { r[5] - r[2], r[6] - r[3], r[7] - r[4] }; float br = Len(e) * 0.5f;
                  if (Len(Sub(c, g_cam)) - br > D || OffScreen(c, br)) continue; }
                { bool in = InBoxLocal(plp, r + 2, r + 5, NULL); if (in) BoxDiag(0, r, 8);
                  DrawBox(r + 2, r + 5, NULL, in ? 0xFFFF40FF : pc); } lc = c; break; }
            case 1: case 2: {                                      // XTOBB / OBB
                const float* ext = r + (k == 1 ? 8 : 2);
                const float* M = r + (k == 1 ? 14 : 8);
                V3 c = { M[12], M[13], M[14] };
                if (!FiniteV(&c.x)) continue;
                {   V3 e = { ext[3] - ext[0], ext[4] - ext[1], ext[5] - ext[2] };
                    V3 lm = { (ext[0] + ext[3]) * 0.5f, (ext[1] + ext[4]) * 0.5f, (ext[2] + ext[5]) * 0.5f };
                    V3 wc = { lm.x * M[0] + lm.y * M[4] + lm.z * M[8] + M[12], lm.x * M[1] + lm.y * M[5] + lm.z * M[9] + M[13], lm.x * M[2] + lm.y * M[6] + lm.z * M[10] + M[14] };
                    float sc = Len(V3{ M[0], M[1], M[2] }); if (!(sc > 0.01f && sc < 100.0f)) sc = 1;
                    if (!FiniteV(&wc.x)) continue;
                    float br = Len(e) * 0.5f * sc;
                    if (Len(Sub(wc, g_cam)) - br > D || OffScreen(wc, br)) continue;
                    c = wc; }
                { const float* Mi = r + (k == 1 ? 30 : 24); bool in = FiniteV(Mi + 12) && InBoxLocal(plp, ext, ext + 3, Mi);
                  if (in) BoxDiag(k, r, k == 1 ? 46 : 40);
                  DrawBox(ext, ext + 3, M, in ? 0xFFFF40FF : pc); } lc = c; break; }
            case 3: {                                              // sphere
                V3 c = { r[2], r[3], r[4] }; float rad = r[5];
                if (!FiniteV(&c.x) || !SaneR(rad) || !Near(c, D2) || OffScreen(c, rad)) continue;
                V3 X = { 1, 0, 0 }, Y = { 0, 1, 0 }, Z = { 0, 0, 1 };
                g_ringN = 24; Ring(c, X, Z, rad, pc); Ring(c, X, Y, rad, pc); Ring(c, Z, Y, rad, pc); lc = c; break; }
            case 4: {                                              // vertical cylinder, unbounded in y
                float rad = r[4];
                V3 c = { r[2], g_cam.y, r[3] };
                if (!(c.x == c.x) || !SaneR(rad) || !Near(c, D2)) continue;
                float gy = CylGround(i, c.x, c.z);
                if (!(gy == gy)) continue;                             // ground not known yet (next frames)
                c.y = gy;
                V3 X = { 1, 0, 0 }, Z = { 0, 0, 1 };
                { V3 mid = { c.x, gy + 2.0f, c.z }; if (OffScreen(mid, 2.5f + rad)) continue; }
                g_ringN = 24;
                for (int j = 0; j <= 2; ++j) { V3 cc = c; cc.y = gy + j * 2.0f; Ring(cc, X, Z, rad, pc); }
                for (int j = 0; j < 4; ++j) {
                    float an = j * PI * 0.5f;
                    V3 o = { c.x + FCos(an) * rad, gy, c.z + FSin(an) * rad }, o2 = o; o2.y = gy + 4.0f;
                    Line3(o, o2, pc);
                }
                lc = c; break; }
            }
            if (CC.labels && g_primLab < 120) {
                static const char* const kN[5] = { "AABB", "XTOBB", "OBB", "SPHERE", "CYLINDER" };
                float lim = D < 15 ? D : 15;
                if (Near(lc, lim * lim) && ClipW(lc) > 0.1f && (!g_soft || SoftVisible(FrontPoint(lc, 1.5f)))) {
                    bool sv = g_soft; g_soft = false;
                    char l[48], top[64];
                    CollPropText(kN[k], *(const uint32_t*)r, l, sizeof l);
                    if (!FrameName(*(const uint8_t* const*)(r + 1), top, sizeof top)) top[0] = 0;
                    Label(lc, l, pc, top);
                    g_soft = sv;
                    ++g_primLab;
                }
            }
        }
    }
}

static void DrawProgs(uint8_t* game) {
    uint8_t* player = GetPlayer();
    V3 cam = { 0, 0, 0 };
    uint8_t* cf = GetCameraFrame();
    if (cf) cam = RdV3(cf + 0x40); else if (player) ActorPos(player, cam);
    g_cam = cam;
    uint32_t now = GetTickCount();
    g_drawn = 0; g_alive = 0;
    {   // every detector of the mission, also the ones the game does not run now (detector_waitforuse,
        // wait ...): its C_program is embedded at C_detector+0x70
        uint8_t** b = *(uint8_t***)(game + 0xf0);
        uint8_t** e = *(uint8_t***)(game + 0xf4);
        int n = (b && e > b) ? (int)(e - b) : 0;
        if (n > 20000 || (n && !Readable(b, (size_t)n * 4))) n = 0;
        for (int i = 0; i < n; ++i) {
            uint8_t* a = b[i];
            if (!Readable(a, 0xb8)) continue;
            uint8_t* pg = NULL; bool det = false;
            if (*(const int*)(a + 0x10) == 5) { pg = a + 0x70; det = true; }               // kind 5 = detector
            else if (*(const uint32_t*)(a + 0x9c) == 0x00624858) pg = a + 0x9c;           // entity with a script
            if (!pg) continue;
            uint8_t* ib = *(uint8_t**)(pg + 0x10); uint8_t* ie = *(uint8_t**)(pg + 0x14);
            if (!ib || ie <= ib || (size_t)(ie - ib) > 200000 * 8) continue;
            Register(pg, det);
        }
    }
    const uint32_t kGold = 0xFFD8B040, kHit = 0xFFFF6A30;
    for (int pi = 0; pi < g_npr; ++pi) {
        Prog& g = g_pr[pi];
        if (now - g.seen > (uint32_t)C.keepMs) continue;
        uint8_t* p = g.p;
        if (!Readable(p, 0x70) || *(uint32_t*)p != g.vt) continue;
        ++g_alive;
        int pc = *(int*)(p + 0x1c);
        const float* flt = *(const float* const*)(p + 0x48); int fn = *(int*)(p + 0x4c);
        uint8_t* const* acts = *(uint8_t* const* const*)(p + 0x58); int an = *(int*)(p + 0x5c);
        if (fn < 0 || fn > 10000 || (fn && !Readable(flt, (size_t)fn * 4))) fn = 0;
        if (an < 0 || an > 10000 || (an && !Readable(acts, (size_t)an * 4))) an = 0;

        // the owning actor (detector: program at +0x70, entity: at +0x9c) gives the anchor
        uint8_t* owner = p - (g.isDet ? 0x70 : 0x9c);
        V3 anchor; bool hasAnchor = false;
        if (Readable(owner, 0x70)) {
            uint32_t ovt = *(uint32_t*)owner;
            if (ovt >= 0x00600000 && ovt < 0x00680000) hasAnchor = ActorPos(owner, anchor);
        }
        bool nearCam = hasAnchor && Len(Sub(anchor, cam)) <= C.maxDist;

        char name[48]; name[0] = 0;
        ProgName(p, g.isDet != 0, name, sizeof name);
        const uint32_t kGold = ProgCol(name), kHit = HitCol(kGold);   // own colour per script
        g_lbTitle = kGold;
        bool anyHit = false;
        char ln[4][96]; int nl = 0;
        {   // current command + signal
            int op = -1;
            uint8_t* ins = g.ib + (size_t)pc * 8;
            if (pc >= 0 && ins + 4 <= g.ie && Readable(ins, 4)) op = *(int*)ins;
            ln[0][0] = 0; SCat(ln[0], sizeof ln[0], op >= 0 ? OpName(op) : "end");
            if (g.isDet) {
                SCat(ln[0], sizeof ln[0], ", signal "); SCatU(ln[0], sizeof ln[0], p[0x6c]);
                if (p[0x6d]) SCat(ln[0], sizeof ln[0], " (use)");
                if (p[0x6e]) SCat(ln[0], sizeof ln[0], " (hit)");
            }
            nl = 1;
        }

        for (int k = 0; k < g.n; ++k) {
            const Item& it = g.it[k];
            bool active = pc >= it.ins0 - 2 && pc <= it.ins1 + 12;
            if (C.onlyActive && !active) continue;
            char* l = nl < 4 ? ln[nl] : NULL;

            if (it.type == IT_INRANGE) {
                if (!g.isDet) continue;
                V3 c; if (!FramePos(*(uint8_t* const*)(p - 8), c)) continue;
                if (Len(Sub(c, cam)) - it.hi > C.maxDist) continue;
                uint32_t mask = *(uint32_t*)(p + 0x68);
                int inside = CountInRange(game, mask, c, it.hi);
                { V3 pp; if (player && ActorPos(player, pp) && Len(Sub(pp, c)) < it.hi && !inside) inside = 1; }   // the player walked in
                if (inside) anyHit = true;
                if (C.showInrange) {
                    WireSphere(c, it.hi, inside ? kHit : kGold); ++g_drawn;
                    if (!hasAnchor || Len(Sub(anchor, c)) > it.hi + 2.0f) { Label(c, "inrange", inside ? kHit : kGold, name); }
                }
                if (l) {
                    l[0] = 0; SCat(l, 96, "in range "); SCatF1(l, 96, it.hi); SCat(l, 96, " m of types 0x");
                    char hx[9]; int h = 0;                       // short hex, no leading zeros
                    for (int s = 28; s >= 0; s -= 4) { int dg = (mask >> s) & 15; if (dg || h || !s) hx[h++] = "0123456789ABCDEF"[dg]; }
                    hx[h] = 0; SCat(l, 96, hx);
                    if (inside) { SCat(l, 96, " - IN "); SCatU(l, 96, (uint32_t)inside); }
                    ++nl;
                }
            } else {
                if (it.a < 0 || it.a >= an || it.b < 0 || it.b >= an) continue;
                uint8_t* A = acts[it.a]; uint8_t* B = acts[it.b];
                V3 pa, pb; if (!ActorPos(A, pa) || !ActorPos(B, pb)) continue;
                bool aPl = A == player || (Readable(A, 0x14) && *(const int*)(A + 0x10) == 2);
                bool swap = aPl;                             // centre on the actor that is NOT the player
                V3 c = swap ? pb : pa, o = swap ? pa : pb;
                float d = Len(Sub(pa, pb));
                float lo = it.lo, hi = it.hi;
                if (it.loReg >= 0) lo = it.loReg < fn ? flt[it.loReg] : 0;
                if (it.hiReg >= 0) hi = it.hiReg < fn ? flt[it.hiReg] : 0;
                float shown = it.cmp == CMP_GT ? lo : hi;
                bool hit = false;
                if (it.cmp == CMP_LT) hit = d < hi;
                else if (it.cmp == CMP_GT) hit = d > lo;
                else if (it.cmp == CMP_RANGE) hit = d >= lo && d < hi;
                if (Len(Sub(c, cam)) - (SaneR(shown) ? shown : 0) > C.maxDist) continue;
                uint32_t col = hit ? kHit : kGold;
                if (hit) anyHit = true;
                if (C.showDist) {
                    if (it.cmp != CMP_NONE && SaneR(shown)) {
                        WireSphere(c, shown, col);
                        if (it.cmp == CMP_RANGE && SaneR(lo)) WireSphere(c, lo, WithAlpha(col, 0.5f));
                        ++g_drawn;
                        if (!hasAnchor || Len(Sub(anchor, c)) > shown + 2.0f) { Label(c, "getactorsdist", col, name); }
                        static int s_dl = 0;
                        if (s_dl < 60 && !it.loggedDraw) {
                            ++s_dl; g.it[k].loggedDraw = 1;
                            char lg[200]; lg[0] = 0; SCat(lg, sizeof lg, "dist sphere '"); SCat(lg, sizeof lg, name);
                            SCat(lg, sizeof lg, "' r "); SCatF1(lg, sizeof lg, shown);
                            SCat(lg, sizeof lg, " A kind "); SCatU(lg, sizeof lg, Readable(A, 0x14) ? *(const uint32_t*)(A + 0x10) : 999);
                            { char an[40]; if (FrameName(*(uint8_t* const*)(A + 0x68), an, sizeof an)) { SCat(lg, sizeof lg, " '"); SCat(lg, sizeof lg, an); SCat(lg, sizeof lg, "'"); } }
                            SCat(lg, sizeof lg, " B kind "); SCatU(lg, sizeof lg, Readable(B, 0x14) ? *(const uint32_t*)(B + 0x10) : 999);
                            { char bn[40]; if (FrameName(*(uint8_t* const*)(B + 0x68), bn, sizeof bn)) { SCat(lg, sizeof lg, " '"); SCat(lg, sizeof lg, bn); SCat(lg, sizeof lg, "'"); } }
                            SCat(lg, sizeof lg, A == player ? " A=player" : (B == player ? " B=player" : " neither is player"));
                            Log(lg);
                        }
                    }
                    if (C.showLines && (active || it.cmp == CMP_NONE)) Line3(c, o, WithAlpha(col, 0.6f));
                }
                if (l && (it.cmp != CMP_NONE || active)) {
                    l[0] = 0; SCat(l, 96, "dist "); SCatF1(l, 96, d);
                    if (it.cmp == CMP_LT) { SCat(l, 96, " < "); SCatF1(l, 96, hi); }
                    else if (it.cmp == CMP_GT) { SCat(l, 96, " > "); SCatF1(l, 96, lo); }
                    else if (it.cmp == CMP_RANGE) { SCat(l, 96, " in "); SCatF1(l, 96, lo); SCat(l, 96, ".."); SCatF1(l, 96, hi); }
                    if (hit) SCat(l, 96, " - HIT");
                    ++nl;
                }
            }
        }

        if (nearCam) {
            // marker: a short vertical stick with a small cross at its foot, label on top
            V3 top = anchor; top.y += 1.0f;
            V3 a1 = anchor, a2 = anchor, b1 = anchor, b2 = anchor;
            a1.x -= 0.2f; a2.x += 0.2f; b1.z -= 0.2f; b2.z += 0.2f;
            Line3(anchor, top, kGold); Line3(a1, a2, kGold); Line3(b1, b2, kGold);
            g_lbTitle = anyHit ? kGreen : kGold; g_lbText = anyHit ? kGreen : 0xFFE6E6E6;
            Line3(anchor, top, g_lbTitle);
            if (C.showName || nl > 1) LabelBox(top, C.showName ? name : NULL, ln, nl);
            g_lbText = 0xFFE6E6E6;
        }
        if (C.showVolumes && g.isDet) DrawVolumes(p - 0x70, cam, name);
    }
}

// ------------------------------------------------------------------ IGraph hooks
typedef int  (STDC *Fn_Int)(void* g);
typedef void (STDC *Fn_Void)(void* g);
typedef bool (STDC *Fn_SetTex)(void* g, void* tex);
typedef int  (STDC *Fn_DPL)(void* g, int type, uint32_t prims, void* verts, int stream);
static Fn_Void o_present, o_begin, o_end;
static Fn_SetTex o_setTex;
static Fn_DPL o_dpl;
static Fn_Int o_sx, o_sy;

// ------------------------------------------------------------------ game camera (LS3DF I3D_camera)
// The camera frame (mission+0x10 -> +0x17c) is an I3D_camera: vtable LS3DF+0x9B6D0, +0x50 SetFOV(rad) (stdcall,
// clamps to 0.01..pi), +0x140 fov (rad, horizontal), +0x144/+0x148 near/far, +0x158 aspect, +0x1A4 the
// projection matrix the scene is rendered with (rebuilt by 0x1000B400): [0] cot(fov/2), [5] [0]*aspect,
// [10] Q = far/(far-near), [11] 1, [14] -near*Q.
static const uint32_t LS3DF_CAM_VT_RVA = 0x0009B6D0;
typedef void (__attribute__((stdcall)) *Fn_SetFov)(void* cam, float rad);
static uint8_t* g_camObj = NULL;
static bool g_camOK = false, g_camLogged = false;
static uint8_t* CamObj() {
    uint8_t* fr = GetCameraFrame();
    if (!fr || !g_ls3d || !Readable(fr, 0x3F8)) return NULL;
    if (*(uint32_t*)fr != (uint32_t)(g_ls3d + LS3DF_CAM_VT_RVA)) {
        static uint32_t s_bad = 0;
        if (s_bad != *(uint32_t*)fr) {
            s_bad = *(uint32_t*)fr;
            char l[120]; l[0] = 0; SCat(l, sizeof l, "camera frame is not an I3D_camera, vtable "); SCatX(l, sizeof l, s_bad); Log(l);
        }
        return NULL;
    }
    return fr;
}
static float g_camProj[16];                     // the camera's projection (W-buffer range for our draw)
// LS3DF's depth mode (IGraph state, RVA 0xA69D8): 1 = z-buffer, 2 = w-buffer (the game's W-buffer
// option). Our depth-tested primitives must use the same mode as the scene, else the compare is garbage.
static const uint32_t LS3DF_ZMODE_RVA = 0x000A69D8;
static uint32_t ZMode() {
    if (!g_ls3d || !Readable(g_ls3d + LS3DF_ZMODE_RVA, 4)) return D3DZB_TRUE;
    uint32_t v = *(const uint32_t*)(g_ls3d + LS3DF_ZMODE_RVA);
    return v == D3DZB_USEW ? D3DZB_USEW : D3DZB_TRUE;
}
static void CamSetup() {
    g_camOK = false;
    uint8_t* c = CamObj(); g_camObj = c;
    if (!c) return;
    const float* m = (const float*)(c + 0x1A4);
    float n = *(const float*)(c + 0x144), f = *(const float*)(c + 0x148);
    bool sane = m[0] > 0.05f && m[0] < 60 && m[5] > 0.05f && m[5] < 60 && m[11] > 0.99f && m[11] < 1.01f
             && n > 0.001f && f > n && m[10] > 0.5f && m[10] < 2.0f;
    if (!g_camLogged) {
        g_camLogged = true;
        char l[300]; l[0] = 0; SCat(l, sizeof l, "camera "); SCatX(l, sizeof l, (uint32_t)c);
        SCat(l, sizeof l, " fov "); SCatF(l, sizeof l, *(const float*)(c + 0x140) * 180.0f / PI);
        SCat(l, sizeof l, " near "); SCatF(l, sizeof l, n); SCat(l, sizeof l, " far "); SCatF(l, sizeof l, f);
        SCat(l, sizeof l, " aspect "); SCatF(l, sizeof l, *(const float*)(c + 0x158));
        SCat(l, sizeof l, " P11 "); SCatF(l, sizeof l, m[0]); SCat(l, sizeof l, " P22 "); SCatF(l, sizeof l, m[5]);
        SCat(l, sizeof l, " (FovDeg fallback P11 "); SCatF(l, sizeof l, g_p11); SCat(l, sizeof l, " P22 "); SCatF(l, sizeof l, g_p22);
        SCat(l, sizeof l, sane ? ") - using camera" : ") - NOT SANE, ignored");
        Log(l);
    }
    if (!sane) return;
    g_camOK = true;
    if (!g_devMode && C.camMatrix) { g_p11 = m[0]; g_p22 = m[5]; }
    g_zQ = m[10]; g_zQN = -m[14];
    for (int i = 0; i < 16; ++i) g_camProj[i] = m[i];
}
// FOV override. A different value than the one we set means the game (or a script, a sniper scope)
// set it; below 30 degrees (zoom) the game is left alone. Off again -> the game's value is put back.
static float g_gameFov = 0, g_fovSet = 0;
static void ApplyFov() {
    g_mtFov = C.fov >= 40.0f ? C.fov * PI / 180.0f : 0.0f;          // the free camera uses the slider too
    uint8_t* c = CamObj(); if (!c) return;
    float cur = *(const float*)(c + 0x140);
    Fn_SetFov setFov = *(Fn_SetFov*)(*(uint8_t**)c + 0x50);
    if (cur != g_fovSet) g_gameFov = cur;
    if (C.fov < 40.0f) {
        if (g_fovSet != 0 && cur == g_fovSet && g_gameFov > 0.1f) setFov(c, g_gameFov);
        g_fovSet = 0;
        return;
    }
    if (g_gameFov < 30.0f * PI / 180.0f) return;
    float want = C.fov * PI / 180.0f;
    if (cur != want) {
        static int s_n = 0;
        if (s_n < 4) {
            ++s_n; char l[120]; l[0] = 0; SCat(l, sizeof l, "fov: game "); SCatF(l, sizeof l, cur * 180.0f / PI);
            SCat(l, sizeof l, " -> "); SCatF(l, sizeof l, C.fov); Log(l);
        }
        setFov(c, want);
        g_fovSet = *(const float*)(c + 0x140);
    }
}

// ------------------------------------------------------------------ settings panel (MenuKey, F9)
// Up/Down = row, Left/Right = change (hold to repeat), Enter = toggle, MenuKey / Backspace = close.
// Closing saves everything into the .ini.
enum { MR_BOOL, MR_INT, MR_FLOAT, MR_FOV, MR_CLOSE };
struct MRow { const char* name; int type; void* p; float mn, mx, step; const char* key; int dec; };
static bool g_menu = false;
static int  g_menuSel = 0;
static const MRow kRows[] = {
    { "TRIGGERY",                            MR_BOOL,  &g_show,          0, 0, 0, "ShowOnStart", 0 },
    { "N\xC1ZVY OBJEKT\xD9",                  MR_INT,   &C.showAll,       0, 0, 0, "ShowAllObjects", 0 },
    { "KOLIZE",                              MR_INT,   &CC.show,         0, 0, 0, "ShowCollision", 0 },
    { "SKR\xDDT HUD",                          MR_INT,   &C.hideHud,       0, 0, 0, "HideHud", 0 },
    { "WIREFRAME",                           MR_INT,   (void*)&g_mtWire,    0, 0, 0, "Wireframe", 0 },
    { "FREE KAMERA",                         MR_INT,   (void*)&g_mtFreecam, 0, 0, 0, "Freecam", 0 },
    { "CHECK.BIN",                           MR_INT,   &C.showCheck,     0, 0, 0, "ShowCheck", 0 },
    { "ROAD.BIN",                            MR_INT,   &C.showRoad,      0, 0, 0, "ShowRoad", 0 },
    { "KOLIZE P\xD8" "ES ZDI",                MR_INT,   &CC.xray,         0, 0, 0, "CollisionSeeThrough", 0 },
    { "POPISKY KOLIZ\xCD",                    MR_INT,   &CC.labels,       0, 0, 0, "CollisionLabels", 0 },
    { "L\xC9T\xC1N\xCD",                     MR_INT,   &C.fly,           0, 0, 0, "Flying", 0 },
    { "GOD MODE",                            MR_INT,   &C.god,           0, 0, 0, "GodMode", 0 },
    { "NEKONE\xC8N\xC9 N\xC1" "BOJE",               MR_INT,   &C.infAmmo,       0, 0, 0, "InfiniteAmmo", 0 },
    { "DEBUG TEXT",                          MR_INT,   &g_dtShow,        0, 0, 0, "ShowDebugText", 0 },
    { "ENGLISH",                             MR_INT,   &C.langEn,        0, 0, 0, "LanguageEN", 0 },
    { "FOV KAMERY",                          MR_FOV,   &C.fov,           39, 130, 1, "CameraFov", 0 },
    { "ZAV\xD8\xCDT A ULO\x8EIT",            MR_CLOSE, NULL,             0, 0, 0, NULL, 0 },
};
enum { N_MROWS = sizeof kRows / sizeof kRows[0] };

static void FmtF(char* d, size_t cap, float f, int dec) {
    d[0] = 0;
    if (f < 0) { SCat(d, cap, "-"); f = -f; }
    uint32_t k = dec == 2 ? 100 : dec == 1 ? 10 : 1;
    uint32_t t = (uint32_t)(f * (float)k + 0.5f);
    SCatU(d, cap, t / k);
    if (dec) { SCat(d, cap, "."); uint32_t r = t % k; if (dec == 2 && r < 10) SCat(d, cap, "0"); SCatU(d, cap, r); }
}
static void SaveCfg() {
    for (int i = 0; i < N_MROWS; ++i) {
        const MRow& r = kRows[i]; if (!r.key) continue;
        char v[32];
        if (r.type == MR_BOOL) { v[0] = 0; SCatU(v, sizeof v, *(bool*)r.p ? 1 : 0); }
        else if (r.type == MR_INT) { v[0] = 0; SCatU(v, sizeof v, *(int*)r.p ? 1 : 0); }
        else { float f = *(float*)r.p; if (r.type == MR_FOV && f < 40) f = 0; FmtF(v, sizeof v, f, r.dec); }
        WritePrivateProfileStringA("DeveloperTools", r.key, v, g_ini);
    }
    Log("settings saved");
}
static void MenuChange(int dir, bool toggle) {
    const MRow& r = kRows[g_menuSel];
    switch (r.type) {
    case MR_BOOL: *(bool*)r.p = !*(bool*)r.p; break;
    case MR_INT:  *(int*)r.p = !*(int*)r.p; break;
    case MR_FLOAT: case MR_FOV: {
        if (toggle) break;
        float* f = (float*)r.p;
        float v = *f; if (r.type == MR_FOV && v < 40) v = 39;
        v += dir * r.step;
        if (v < r.mn) v = r.mn; if (v > r.mx) v = r.mx;
        if (r.type == MR_FOV && v < 40) v = 0;
        *f = v; break; }
    case MR_CLOSE: if (toggle || dir) { g_menu = false; SaveCfg(); } break;
    }
}
// edge + hold-repeat key state (one bit per key we use)
static bool KeyHit(int vk, uint32_t now, bool repeat) {
    static bool s_down[256]; static uint32_t s_next[256];
    bool d = KeyDown(vk);
    bool hit = false;
    if (d && !s_down[vk]) { hit = true; s_next[vk] = now + 350; }
    else if (d && repeat && (int32_t)(now - s_next[vk]) >= 0) { hit = true; s_next[vk] = now + 45; }
    s_down[vk] = d;
    return hit;
}
static void MenuInput(bool focus) {
    uint32_t now = GetTickCount();
    if (!focus) return;
    if (KeyHit(VK_UP, now, true))    g_menuSel = (g_menuSel + N_MROWS - 1) % N_MROWS;
    if (KeyHit(VK_DOWN, now, true))  g_menuSel = (g_menuSel + 1) % N_MROWS;
    if (KeyHit(VK_LEFT, now, true))  MenuChange(-1, false);
    if (KeyHit(VK_RIGHT, now, true)) MenuChange(+1, false);
    if (KeyHit(VK_RETURN, now, false)) MenuChange(0, true);
    if (KeyHit(VK_BACK, now, false)) { g_menu = false; SaveCfg(); }
}
static void DrawMenu(float sw, float sh) {
    float k = sh / 600.0f; if (k < 0.6f) k = 0.6f;
    float px = 1.25f * k, rowH = 13.0f * k;
    float W = 330.0f * k, H = (N_MROWS + 3.2f) * rowH;
    float x0 = (sw - W) * 0.5f, y0 = (sh - H) * 0.5f;
    Rect(x0, y0, x0 + W, y0 + H, 0xD0101418);
    Rect(x0, y0, x0 + W, y0 + 1.6f * rowH, 0xE0203040);
    Rect(x0, y0 + 1.6f * rowH - 1.5f * k, x0 + W, y0 + 1.6f * rowH, 0xFF00C8FF);
    Text("DEVELOPER TOOLS - NASTAVEN\xCD", x0 + 8 * k, y0 + 0.35f * rowH, px * 1.15f, 0xFFFFFFFF);
    float y = y0 + 2.0f * rowH;
    for (int i = 0; i < N_MROWS; ++i, y += rowH) {
        const MRow& r = kRows[i];
        bool sel = i == g_menuSel;
        if (sel) Rect(x0 + 3 * k, y - 2.5f * k, x0 + W - 3 * k, y + rowH - 3.5f * k, 0x6000A0FF);
        uint32_t tc = sel ? 0xFFFFFFFF : 0xFFC8D0D8;
        if (r.type == MR_CLOSE) { float tw = TextW(r.name, px); Text(r.name, x0 + (W - tw) * 0.5f, y, px, sel ? 0xFFFFFFFF : 0xFF80E0FF); continue; }
        Text(r.name, x0 + 8 * k, y, px, tc);
        float vx = x0 + W * 0.58f, vw = W * 0.42f - 10 * k;
        char v[32];
        if (r.type == MR_BOOL || r.type == MR_INT) {
            bool on = r.type == MR_BOOL ? *(bool*)r.p : *(int*)r.p != 0;
            Rect(vx, y - 1 * k, vx + 8 * k, y + 7 * k, on ? 0xFF30E060 : 0xFF505860);
            Text(on ? "ZAP" : "VYP", vx + 12 * k, y, px, on ? 0xFF80FF90 : 0xFF909898);
        } else {
            float f = *(float*)r.p;
            bool off = r.type == MR_FOV && f < 40;
            if (off) { v[0] = 0; SCat(v, sizeof v, "HRA");
                if (g_gameFov > 0) { SCat(v, sizeof v, " "); char t[16]; FmtF(t, sizeof t, g_gameFov * 180.0f / PI, 0); SCat(v, sizeof v, t); } }
            else FmtF(v, sizeof v, f, r.dec);
            if (r.type == MR_FLOAT && (r.key[0] == 'M' || r.key[9] == 'D')) SCat(v, sizeof v, " M");
            float bw = vw - 50 * k, t = off ? 0 : (f - r.mn) / (r.mx - r.mn);
            if (t < 0) t = 0; if (t > 1) t = 1;
            Rect(vx, y + 2.5f * k, vx + bw, y + 4.5f * k, 0xFF505860);
            Rect(vx, y + 2.5f * k, vx + bw * t, y + 4.5f * k, sel ? 0xFF00C8FF : 0xFF4090B0);
            Rect(vx + bw * t - 2 * k, y - 0.5f * k, vx + bw * t + 2 * k, y + 7.5f * k, sel ? 0xFFFFFFFF : 0xFFB0B8C0);
            Text(v, vx + bw + 6 * k, y, px, tc);
        }
    }
    Text("NAHORU/DOLU: \xD8\xC1" "DEK   VLEVO/VPRAVO: ZM\xCCNA   BACKSPACE: ZAV\xD8\xCDT", x0 + 8 * k, y0 + H - 1.0f * rowH, px * 0.8f, 0xFF8890A0);
}

static uint8_t* g_lastGame = NULL;

// The game leaves its own render states on the device (alpha test with a high reference value, z test,
// culling - whatever it drew last, e.g. the pause menu). With them our translucent fill, dimmed rings and
// label backgrounds were thrown away. Set our own states around the draw and put the game's back after.
static IDirect3DDevice8* GetDev() {
    if (!g_ls3d) return NULL;
    IDirect3DDevice8** pd = (IDirect3DDevice8**)(g_ls3d + LS3DF_DEV_RVA);
    if (!Readable(pd, 4) || !Readable(*pd, 4)) return NULL;
    return *pd;
}
static const D3DRENDERSTATETYPE kRsT[] = { D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DRS_BLENDOP, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_ALPHAREF,
    D3DRS_ZFUNC };
static const DWORD kRsV[] = { FALSE, TRUE, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA,
    D3DBLENDOP_ADD, D3DZB_FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, 0, D3DCMP_LESSEQUAL };
enum { N_RS = sizeof kRsT / sizeof kRsT[0] };
static const struct { DWORD st; D3DTEXTURESTAGESTATETYPE t; DWORD v; } kTss[] = {
    { 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE },
    { 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 }, { 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE },
    { 1, D3DTSS_COLOROP, D3DTOP_DISABLE },    { 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE } };
enum { N_TSS = sizeof kTss / sizeof kTss[0] };
static DWORD g_rsOld[N_RS], g_tssOld[N_TSS];
static bool g_rsSet = false, g_rsLogged = false;

static void SetOurStates() {
    g_rsSet = false;
    IDirect3DDevice8* dev = C.ownStates ? GetDev() : NULL;
    if (!dev) return;
    for (int i = 0; i < N_RS; ++i) { g_rsOld[i] = 0; dev->GetRenderState(kRsT[i], &g_rsOld[i]); }
    for (int i = 0; i < N_TSS; ++i) { g_tssOld[i] = 0; dev->GetTextureStageState(kTss[i].st, kTss[i].t, &g_tssOld[i]); }
    if (!g_rsLogged) {
        g_rsLogged = true;
        char l[200]; l[0] = 0; SCat(l, sizeof l, "game render states: alphatest "); SCatU(l, sizeof l, g_rsOld[0]);
        SCat(l, sizeof l, " alpharef "); SCatU(l, sizeof l, g_rsOld[10]); SCat(l, sizeof l, " blend "); SCatU(l, sizeof l, g_rsOld[1]);
        SCat(l, sizeof l, " z "); SCatU(l, sizeof l, g_rsOld[5]); SCat(l, sizeof l, " cull "); SCatU(l, sizeof l, g_rsOld[7]);
        SCat(l, sizeof l, " colorop0 "); SCatU(l, sizeof l, g_tssOld[0]); SCat(l, sizeof l, " alphaop0 "); SCatU(l, sizeof l, g_tssOld[2]);
        Log(l);
    }
    for (int i = 0; i < N_RS; ++i) dev->SetRenderState(kRsT[i], kRsV[i]);
    for (int i = 0; i < N_TSS; ++i) dev->SetTextureStageState(kTss[i].st, kTss[i].t, kTss[i].v);
    g_rsSet = true;
}
static void RestoreStates() {
    if (!g_rsSet) return;
    g_rsSet = false;
    IDirect3DDevice8* dev = GetDev();
    if (!dev) return;
    for (int i = N_TSS - 1; i >= 0; --i) dev->SetTextureStageState(kTss[i].st, kTss[i].t, g_tssOld[i]);
    for (int i = N_RS - 1; i >= 0; --i) dev->SetRenderState(kRsT[i], g_rsOld[i]);
}
static bool PatchPtr(void** slot, void* fn, void** orig) {
    DWORD old;
    if (!VirtualProtect(slot, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *orig = *slot; *slot = fn;
    VirtualProtect(slot, 4, old, &old);
    return true;
}

// ------------------------------------------------------------------ real game menu (devtools.mnu)
// Our screen = tables\menu\devtools.mnu with a copy of the options screen vtable (slot 0 = name).
// Control ids (see gen_mnu.py): row i of kRows -> 0xDF100 + i, edit X/Y/Z 0xDF200..2,
// Teleport 0xDF210, "Moje pozice" 0xDF211, Zpet 0xDF2FF.
static const uint32_t M_CLOSE       = 0x005EAE10;  // close this screen (code), ret 4
static const uint32_t M_CLOSE_ALL   = 0x005EAE40;  // close this + parents (code, result), ret 8
static const uint32_t M_SET_FOCUS   = 0x005EB210;  // focus control (id), ret 4
static const uint32_t M_GET_WIDGET  = 0x005EB2F0;  // screen -> widget* by id
static const uint32_t M_VT_TEMPLATE = 0x0062733C;  // options screen vtable (12 slots)
static const uint32_t W_VT_CHCK     = 0x00626C00, W_VT_SLID = 0x00626A00, W_VT_EDIT = 0x00626B80;
static const uint32_t ID_ROW0 = 0xDF100, ID_EDIT0 = 0xDF200, ID_TELEPORT = 0xDF210, ID_MYPOS = 0xDF211, ID_BACK = 0xDF2FF;
// teleport (same sequence as the script command that moves an actor, 0x470F7D)
static const uint32_t A_ACT_SETPOS  = 0x0047AF50;  // actor thiscall (x, y, z)  -> actor+0x24
static const uint32_t A_FRAME_SETPOS= 0x00460980;  // frame thiscall (V3*)
static const uint32_t A_SECTOR_LINK = 0x0047ADF0;  // (mission+0x10) thiscall (frame)

typedef int      (THISC* Fn_MClose)(void* self, int code);
typedef int      (THISC* Fn_MCloseAll)(void* self, int code, int result);
typedef int      (THISC* Fn_MFocus)(void* self, uint32_t id);
typedef uint8_t* (THISC* Fn_MWidget)(void* self, uint32_t id);
typedef void     (THISC* Fn_WSetChk)(void* w, int on);
typedef void     (THISC* Fn_WSetFlt)(void* w, float v);
typedef void     (THISC* Fn_WSetTxt)(void* w, const char* s);
typedef void     (THISC* Fn_ActSetPos)(void* act, float x, float y, float z);
typedef void     (THISC* Fn_FrSetPos)(void* fr, const V3* p);
typedef void     (STDC*  Fn_FrUpdate)(void* fr);
typedef void     (THISC* Fn_SecLink)(void* sec, void* fr);

static uint32_t g_mvt[12];
static bool     g_keyHooked = false;
static uint8_t* g_scr = NULL;              // our open screen (NULL = closed)

static bool WriteCode32(uint32_t addr, uint32_t v) {
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)addr, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(volatile uint32_t*)(uintptr_t)addr = v;
    VirtualProtect((void*)(uintptr_t)addr, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)addr, 4);
    return true;
}
static uint8_t* Widget(uint32_t id, uint32_t vt) {
    if (!g_scr) return NULL;
    uint8_t* w = ((Fn_MWidget)M_GET_WIDGET)(g_scr, id);
    if (!Readable(w, 0x88) || *(uint32_t*)w != vt) return NULL;
    return w;
}
static bool ScreenAlive() { return g_scr && Readable(g_scr, 4) && *(uint32_t*)g_scr == (uint32_t)(uintptr_t)g_mvt; }

static float RowT(const MRow& r) {                    // value -> slider position 0..1
    float v = *(float*)r.p;
    if (r.type == MR_FOV && v < 40) v = r.mn;
    float t = (v - r.mn) / (r.mx - r.mn);
    return t < 0 ? 0 : t > 1 ? 1 : t;
}
static void MnuPush() {                               // our settings -> widgets
    for (int i = 0; i < N_MROWS; ++i) {
        const MRow& r = kRows[i]; uint8_t* w;
        if (r.type == MR_BOOL || r.type == MR_INT) {
            if (!(w = Widget(ID_ROW0 + i, W_VT_CHCK))) continue;
            int on = r.type == MR_BOOL ? *(bool*)r.p : (*(int*)r.p != 0);
            ((Fn_WSetChk)(*(void***)w)[0x5C / 4])(w, on);
        } else if (r.type == MR_FLOAT || r.type == MR_FOV) {
            if (!(w = Widget(ID_ROW0 + i, W_VT_SLID))) continue;
            ((Fn_WSetFlt)(*(void***)w)[0x4C / 4])(w, RowT(r));
        }
    }
}
static void MnuPull() {                               // widgets -> our settings (every frame while open)
    for (int i = 0; i < N_MROWS; ++i) {
        const MRow& r = kRows[i]; uint8_t* w;
        if (r.type == MR_BOOL || r.type == MR_INT) {
            if (!(w = Widget(ID_ROW0 + i, W_VT_CHCK))) continue;
            bool on = w[0x40] != 0;
            if (r.type == MR_BOOL) *(bool*)r.p = on; else *(int*)r.p = on ? 1 : 0;
        } else if (r.type == MR_FLOAT || r.type == MR_FOV) {
            if (!(w = Widget(ID_ROW0 + i, W_VT_SLID))) continue;
            float t = *(float*)(w + 0x40);
            if (!(t >= 0)) t = 0; if (t > 1) t = 1;
            float v = r.mn + t * (r.mx - r.mn);
            if (r.step > 0) { int n = (int)((v - r.mn) / r.step + 0.5f); v = r.mn + n * r.step; if (v > r.mx) v = r.mx; }
            if (r.type == MR_FOV && v < 40) v = 0;    // slider at the very left = the game's own FOV
            *(float*)r.p = v;
        }
    }
}
static float ParseF(const char* s, bool* ok) {
    while (*s == ' ') ++s;
    bool neg = false; if (*s == '-') { neg = true; ++s; } else if (*s == '+') ++s;
    float v = 0, frac = 0; bool dig = false;
    for (; *s >= '0' && *s <= '9'; ++s) { v = v * 10 + (*s - '0'); dig = true; }
    if (*s == '.' || *s == ',') for (++s, frac = 0.1f; *s >= '0' && *s <= '9'; ++s, frac *= 0.1f) { v += (*s - '0') * frac; dig = true; }
    while (*s == ' ') ++s;
    *ok = dig && *s == 0;
    return neg ? -v : v;
}
static void MnuMyPos() {
    uint8_t* pl = GetPlayer(); V3 p;
    if (!pl || !ActorPos(pl, p)) { Log("teleport: no player"); return; }
    float c[3] = { p.x, p.y, p.z };
    for (int k = 0; k < 3; ++k) {
        uint8_t* w = Widget(ID_EDIT0 + k, W_VT_EDIT); if (!w) continue;
        char t[32]; FmtF(t, sizeof t, c[k], 2);
        ((Fn_WSetTxt)(*(void***)w)[0x40 / 4])(w, t);
    }
}
static bool Teleport(float x, float y, float z) {
    uint8_t* pl = GetPlayer(); if (!pl) { Log("teleport: no player"); return false; }
    uint8_t* fr = *(uint8_t**)(pl + 0x68);
    if (!Readable(fr, 0x80)) { Log("teleport: player has no frame"); return false; }
    ((Fn_ActSetPos)A_ACT_SETPOS)(pl, x, y, z);
    V3 p = { x, y, z };
    ((Fn_FrSetPos)A_FRAME_SETPOS)(fr, &p);
    ((Fn_FrUpdate)(*(void***)fr)[0x18 / 4])(fr);
    uint8_t* mission = *(uint8_t**)A_MISSION;
    if (Readable(mission, 0x14)) {
        void* sec = *(void**)(mission + 0x10);
        if (Readable(sec, 4)) ((Fn_SecLink)A_SECTOR_LINK)(sec, fr);
    }
    char l[96]; l[0] = 0; SCat(l, sizeof l, "teleport to ");
    char t[24]; FmtF(t, sizeof t, x, 2); SCat(l, sizeof l, t); SCat(l, sizeof l, " ");
    FmtF(t, sizeof t, y, 2); SCat(l, sizeof l, t); SCat(l, sizeof l, " ");
    FmtF(t, sizeof t, z, 2); SCat(l, sizeof l, t); Log(l);
    return true;
}
static void MnuClose(void* self) {
    if (ScreenAlive()) MnuPull();
    g_scr = NULL;
    SaveCfg();
    ((Fn_MClose)M_CLOSE)(self, -1);                   // back to the ESC game menu
}
static const char* THISC ScrName(void*) { return "devtools"; }
static int THISC ScrInit(void* self) {
    g_scr = (uint8_t*)self;
    MnuPush();
    MnuMyPos();
    ((Fn_MFocus)M_SET_FOCUS)(self, ID_BACK);
    Log("menu opened");
    return 0;
}
static int THISC ScrKey(void* self, int key, int) {
    if ((key & 0xFF) == 1) MnuClose(self);           // ESC
    return 0;
}
static int THISC ScrClick(void* self, uint32_t id) {
    if (id == ID_BACK) { MnuClose(self); return 0; }
    if (id == ID_MYPOS) { MnuMyPos(); return 0; }
    if (id == ID_TELEPORT) {
        float c[3]; bool okAll = true;
        for (int k = 0; k < 3; ++k) {
            uint8_t* w = Widget(ID_EDIT0 + k, W_VT_EDIT); bool ok = false;
            c[k] = w ? ParseF((const char*)(w + 0x40), &ok) : 0;
            okAll = okAll && ok;
        }
        if (!okAll) { Log("teleport: bad coordinates"); return 0; }
        if (Teleport(c[0], c[1], c[2])) {
            if (ScreenAlive()) MnuPull();
            g_scr = NULL; SaveCfg();
            ((Fn_MCloseAll)M_CLOSE_ALL)(self, -1, 0x24);   // close everything + resume the game
        }
        return 0;
    }
    return 0;
}
// The ESC game menu (tables\menu\gamemenu.mnu) gets an item id 0x75 "DEVELOPER TOOLS"; its click
// handler (game menu vtable slot 9) is hooked and opens our screen as a child of the game menu.
static const uint32_t M_NEW         = 0x006106FC;  // operator new (cdecl)
static const uint32_t M_MENU_CTOR   = 0x005EA1F0;  // GM_Menu base ctor (thiscall)
static const uint32_t M_OPEN_CHILD  = 0x005EAD50;  // open child screen (obj, id), ret 8
static const uint32_t M_VT_GAME_CLICK = 0x00625E7C; // game menu vtable slot 9 (orig 0x5E0980)
static const uint32_t ID_GAME_ITEM  = 0x75;
typedef void* (__cdecl* Fn_MNew)(uint32_t size);
typedef void* (THISC*   Fn_MCtor)(void* self);
typedef int   (THISC*   Fn_MOpen)(void* self, void* child, uint32_t id);
typedef int   (THISC*   Fn_MClick)(void* self, uint32_t id);
static Fn_MClick g_origGameClick = NULL;

static int THISC GameClickHook(void* self, uint32_t id) {
    if (id == ID_GAME_ITEM) {
        void* s = ((Fn_MNew)M_NEW)(0x30);
        if (!s) return 0;
        ((Fn_MCtor)M_MENU_CTOR)(s);
        *(uint32_t*)s = (uint32_t)(uintptr_t)g_mvt;
        *(uint32_t*)((uint8_t*)s + 0x2C) |= 0x80000;  // child screen
        Log("game menu: developer tools");
        ((Fn_MOpen)M_OPEN_CHILD)(self, s, id);
        return 0;
    }
    return g_origGameClick(self, id);
}
static void InstallGameMenu() {
    if (!Readable((void*)(uintptr_t)M_VT_GAME_CLICK, 4) || *(uint32_t*)(uintptr_t)M_VT_GAME_CLICK != 0x005E0980 ||
        !Readable((void*)(uintptr_t)M_VT_TEMPLATE, 48)) { Log("ERROR: game menu code differs - menu item not hooked"); return; }
    for (int i = 0; i < 12; ++i) g_mvt[i] = ((const uint32_t*)(uintptr_t)M_VT_TEMPLATE)[i];
    g_mvt[0] = (uint32_t)(uintptr_t)&ScrName;
    g_mvt[1] = (uint32_t)(uintptr_t)&ScrInit;
    g_mvt[7] = (uint32_t)(uintptr_t)&ScrKey;
    g_mvt[9] = (uint32_t)(uintptr_t)&ScrClick;
    g_origGameClick = (Fn_MClick)0x005E0980;
    g_keyHooked = WriteCode32(M_VT_GAME_CLICK, (uint32_t)(uintptr_t)&GameClickHook);
    Log(g_keyHooked ? "game menu hooked - ESC menu item 'DEVELOPER TOOLS' opens devtools.mnu" : "ERROR: VirtualProtect on game menu vtable failed");
}

static bool g_keyPrev = false;
static void DrawRange(void* g, int a, int b) {
    for (int off = a; off + 3 <= b; off += 15000) {
        int cnt = b - off; if (cnt > 15000) cnt = 15000;
        o_dpl(g, 3 /*trilist*/, (uint32_t)(cnt / 3), g_v + off, 1 /*FVF 0x1C4*/);
    }
}
static void CheatsTick();
// mode 0 = everything, software occlusion (fallback when stage 1 did not run)
// mode 1 = 3D world only, inside the 3D scene (stage 1): real depth test against the game's depth buffer
// mode 2 = only the 2D part (menu, status line) - the world was drawn in stage 1 this frame
// ---- check.bin / road.bin, read from the loaded mission (Game.exe 1.2, g_act_mission 0x63788c):
// check.bin = C_web_path *(mission+0x2c): nodes +0x04 (stride 0x1e: pos, +0x0c category, +0x0e id,
//   +0x18 connections {u16 neighbour INDEX (not id - verified on tutorial check.bin), u16 flags (0x80 one-way), float cost} x count byte +0x1d), count +0x0c
// road.bin  = C_roads *(mission+0x30): crossings +0x08..+0x0c (stride 0xec: pos, 4 road slots of 0x34
//   at +0x1c, neighbour tagged id at slot+0), road nodes +0x18..+0x1c (stride 0x18: pos, +0x10/+0x12
//   tagged ids); tagged id & 0x8000 = road node (index & 0x7fff), else crossing
static bool g_navLogged = false;
static bool RoadPos(const uint8_t* cb, uint32_t nc, const uint8_t* nb, uint32_t nn, uint32_t tag, V3& out) {
    if (tag == 0xffff) return false;
    const float* f;
    if (tag & 0x8000) { uint32_t i = tag & 0x7fff; if (i >= nn) return false; f = (const float*)(nb + i * 0x18); }
    else { if (tag >= nc) return false; f = (const float*)(cb + tag * 0xec); }
    out = { f[0], f[1], f[2] };
    return FiniteV(&out.x);
}
static void DrawNav() {
    if (!C.showCheck && !C.showRoad) return;
    uint8_t* ms = *(uint8_t**)A_MISSION;
    if (!Readable(ms, 0x34)) return;
    float D = C.maxDist < 150 ? C.maxDist : 150, D2 = D * D;
    FocalSetup();
    const V3 up = { 0, 1, 0 };
    if (C.showCheck) {
        uint8_t* wp = *(uint8_t**)(ms + 0x2c);
        const uint8_t* nodes = Readable(wp, 0x10) ? *(const uint8_t* const*)(wp + 4) : NULL;
        uint32_t n = nodes ? *(const uint32_t*)(wp + 0xc) : 0;
        if (n > 200000 || !Readable(nodes, (size_t)n * 0x1e)) n = 0;
        if (!g_navLogged && n) {
            const float* f = (const float*)nodes;
            char l[160]; l[0] = 0; SCat(l, sizeof l, "check.bin: "); SCatU(l, sizeof l, n); SCat(l, sizeof l, " nodes, first at ");
            SCatF1(l, sizeof l, f[0]); SCat(l, sizeof l, " "); SCatF1(l, sizeof l, f[1]); SCat(l, sizeof l, " "); SCatF1(l, sizeof l, f[2]); Log(l);
        }
        for (uint32_t i = 0; n && i < n && g_nv < MAX_VTX * 3 / 4; ++i) {
            const uint8_t* nd = nodes + i * 0x1e;
            const float* f = (const float*)nd;
            V3 p = { f[0], f[1] + 0.05f, f[2] };
            if (!FiniteV(&p.x) || !Near(p, D2)) continue;
            uint16_t cat = *(const uint16_t*)(nd + 0x0c);
            uint32_t col = (cat & 0x10) ? 0xFFFF9030 : (cat & 0x0c) ? 0xFFB080FF : (cat & 0x02) ? 0xFF40E0FF : 0xFF60FF60;
            if (!OffScreen(p, 1.0f)) {
                Line3(p, Add(p, Mul(up, 0.6f)), col);
                V3 a = { p.x - 0.25f, p.y, p.z }, b = { p.x + 0.25f, p.y, p.z }, c = { p.x, p.y, p.z - 0.25f }, d = { p.x, p.y, p.z + 0.25f };
                Line3(a, b, col); Line3(c, d, col);
            }
            const uint8_t* cn = *(const uint8_t* const*)(nd + 0x18);
            uint32_t cc = nd[0x1d];
            if (!cc || !Readable(cn, cc * 8)) continue;
            for (uint32_t k = 0; k < cc; ++k) {
                uint16_t j = *(const uint16_t*)(cn + k * 8), fl = *(const uint16_t*)(cn + k * 8 + 2);   // neighbour = array INDEX
                if (j == 0xffff || j >= n) continue;
                if (j < i) continue;                                       // both ways: drawn once
                const float* g2 = (const float*)(nodes + j * 0x1e);
                V3 q = { g2[0], g2[1] + 0.05f, g2[2] };
                if (!FiniteV(&q.x)) continue;
                V3 m = Mul(Add(p, q), 0.5f);
                if (OffScreen(m, Len(Sub(q, p)) * 0.5f + 0.5f)) continue;
                Line3(p, q, WithAlpha(col, 0.75f)); (void)fl;
            }
        }
    }
    if (C.showRoad) {
        uint8_t* rd = *(uint8_t**)(ms + 0x30);
        if (!Readable(rd, 0x24)) return;
        const uint8_t* cb = *(const uint8_t* const*)(rd + 0x08); const uint8_t* ce = *(const uint8_t* const*)(rd + 0x0c);
        const uint8_t* nb = *(const uint8_t* const*)(rd + 0x18); const uint8_t* ne = *(const uint8_t* const*)(rd + 0x1c);
        uint32_t nc = (cb && ce > cb) ? (uint32_t)((ce - cb) / 0xec) : 0, nn = (nb && ne > nb) ? (uint32_t)((ne - nb) / 0x18) : 0;
        if (nc > 20000 || (nc && !Readable(cb, (size_t)nc * 0xec))) nc = 0;
        if (nn > 200000 || (nn && !Readable(nb, (size_t)nn * 0x18))) nn = 0;
        if (!g_navLogged) {
            char l[120]; l[0] = 0; SCat(l, sizeof l, "road.bin: "); SCatU(l, sizeof l, nc); SCat(l, sizeof l, " crossings, ");
            SCatU(l, sizeof l, nn); SCat(l, sizeof l, " road nodes"); Log(l);
        }
        const uint32_t kCross = 0xFFFF5050, kRoad = 0xFF5090FF;
        const V3 X = { 1, 0, 0 }, Z = { 0, 0, 1 };
        for (uint32_t i = 0; i < nc && g_nv < MAX_VTX * 3 / 4; ++i) {
            const uint8_t* cr = cb + i * 0xec;
            const float* f = (const float*)cr;
            V3 p = { f[0], f[1] + 0.15f, f[2] };
            if (!FiniteV(&p.x) || !Near(p, D2)) continue;
            if (!OffScreen(p, 2.0f)) { g_ringN = 20; Ring(p, X, Z, 1.5f, kCross); Line3(p, Add(p, Mul(up, 2.0f)), kCross); }
            for (int k = 0; k < 4; ++k) {
                uint16_t t = *(const uint16_t*)(cr + 0x1c + k * 0x34);
                float rl = *(const float*)(cr + 0x1c + k * 0x34 + 4);
                V3 q; if (!RoadPos(cb, nc, nb, nn, t, q)) continue;
                if (!(t & 0x8000) && !(rl <= Len(Sub(q, p)) * 1.05f + 1.0f)) continue;   // curved road: its nodes show it
                q.y += 0.15f;
                V3 m = Mul(Add(p, q), 0.5f);
                if (OffScreen(m, Len(Sub(q, p)) * 0.5f + 0.5f)) continue;
                Line3(p, q, WithAlpha(kCross, 0.8f));
            }
        }
        for (uint32_t i = 0; i < nn && g_nv < MAX_VTX * 3 / 4; ++i) {
            const uint8_t* rn = nb + i * 0x18;
            const float* f = (const float*)rn;
            V3 p = { f[0], f[1] + 0.15f, f[2] };
            if (!FiniteV(&p.x) || !Near(p, D2)) continue;
            if (!OffScreen(p, 1.0f)) Line3(p, Add(p, Mul(up, 0.8f)), kRoad);
            for (int k = 0; k < 2; ++k) {
                uint16_t t = *(const uint16_t*)(rn + 0x10 + k * 2);
                V3 q; if (!RoadPos(cb, nc, nb, nn, t, q)) continue;
                q.y += 0.15f;
                V3 m = Mul(Add(p, q), 0.5f);
                if (OffScreen(m, Len(Sub(q, p)) * 0.5f + 0.5f)) continue;
                Line3(p, q, kRoad);
            }
        }
    }
    g_navLogged = true;
}
static bool g_st1Logged = false;
static float g_flySpeed = 10.0f;                   // flying speed m/s (mouse wheel)
static const uint32_t A_IGRAPH_PTR  = 0x00647EE0;   // IGraph* (LS3DF)
static bool g_flying = false;                      // flying (menu option), see PlUpdateHook
static bool g_st1OK = false;                       // the last mode-1 call drew with the real depth
static void FrameTVi(void* g, bool inScene, int mode);
static void FrameTV(void* g, bool inScene, int mode) {
    g_mtMenuOpen = Readable((void*)(uintptr_t)0x006BD890, 4) && *(void**)(uintptr_t)0x006BD890 != NULL;   // a game menu is open
    g_mtBypass = 1; FrameTVi(g, inScene, mode); g_mtBypass = 0;
}
static void FrameTVi(void* g, bool inScene, int mode) {
    // no hotkeys: everything is controlled from the ESC game menu (DEVELOPER TOOLS -> devtools.mnu)
    if (g_scr) { if (ScreenAlive()) MnuPull(); else g_scr = NULL; }

    uint8_t* game = GetGame();
    if (game) CheatsTick();
    if (game != g_lastGame) { ClearProgs(); g_lastGame = game; g_camLogged = false; g_navLogged = false; Log("game changed - program table cleared"); }
    if (game) ApplyFov();
    int sw = o_sx(g), sh = o_sy(g);
    if (sw < 64 || sh < 64) return;
    g_nv = 0; g_ntv = 0;
    int nvZ = 0;
    { IDirect3DDevice8* fd = GetDev(); if (fd) BuildFont(fd); }
    float k6 = (float)sh / 600.0f; if (k6 < 0.6f) k6 = 0.6f;
    bool gpu = false;
    if (mode == 1) g_st1OK = false;
    if (mode != 2 && game && (g_show || CC.show) && SetupProjection(sw, sh)) {
        CamSetup();
        if (mode == 1) {
            // the scene's own projection is still set on the device: z = _33 + _43 / w
            IDirect3DDevice8* d = GetDev(); D3DMATRIX pm; memset(&pm, 0, sizeof pm);
            if (g_devMode && d && SUCCEEDED(d->GetTransform(D3DTS_PROJECTION, &pm))) {
                g_zQ = pm._33; g_zQN = -pm._43; gpu = true; g_st1OK = true;
            }
            if (!g_st1Logged) {
                g_st1Logged = true;
                DWORD ze = 0; if (d) d->GetRenderState(D3DRS_ZENABLE, &ze);
                char l[220]; l[0] = 0; SCat(l, sizeof l, gpu ? "stage 1 (in the 3D scene): drawing with the game's depth buffer, P11 " : "stage 1: device projection not usable, P11 ");
                SCatF(l, sizeof l, pm._11); SCat(l, sizeof l, " P33 "); SCatF(l, sizeof l, pm._33);
                SCat(l, sizeof l, " P43 "); SCatF(l, sizeof l, pm._43); SCat(l, sizeof l, " zenable "); SCatU(l, sizeof l, ze);
                Log(l);
            }
            if (!gpu) return;                                       // stage 2 / Present draws the fallback
        }
        // one menu slider drives every draw distance (collision capped - it is the heavy one)
        C.allDist = C.maxDist;                                     // fixed distances from the .ini (no slider)
        IDirect3DDevice8* dev = C.ownStates ? GetDev() : NULL;
        bool zOK = gpu; (void)dev;                                 // mode 1: real depth test
        g_zOn = gpu;
        { uint8_t* cf = GetCameraFrame(); uint8_t* pl = GetPlayer();
          if (cf) g_cam = RdV3(cf + 0x40); else if (pl) ActorPos(pl, g_cam); }
        if (gpu) g_szOK = false; else BuildSoftZ();
        if (CC.show) {
            g_w = 1.0f * (k6 > 1.0f ? k6 : 1.0f); g_zBias = 0.06f; g_zRel = 0.0015f;   // thin lines, small fixed pull (no blinking, no bleeding)
            g_soft = !gpu && !CC.xray;
            DrawCollision();
            g_soft = false;
            LabelDepthOff();
        }
        g_zRel = 0.001f;
        g_collEnd = CC.show && CC.xray ? g_nv - g_nv % 3 : 0;
        g_nv = g_collEnd ? g_collEnd : g_nv;
        g_w = C.lineWidth * 0.5f * k6; if (g_w < 1.0f) g_w = 1.0f; g_zBias = 0.05f;
        g_zOn = gpu; g_soft = !gpu;
        g_labelBg = true;
        if (g_show) DrawProgs(game);
        g_labelBg = false;
        if (C.showAll) { DrawAllNames(game, g_cam); DrawSceneNames(game, g_cam); }   // names of all objects (menu switch)
        DrawNav();                                                 // check.bin / road.bin
        g_soft = false;
        g_zOn = false; LabelDepthOff();
        if (zOK) { nvZ = g_nv - g_nv % 3; g_nv = nvZ; }
    }
    if (mode != 1 && !C.hideHud && g_show && C.hud && game) {
        char l[128]; l[0] = 0; SCat(l, sizeof l, "DEVELOPER TOOLS  SCRIPTS:"); SCatU(l, sizeof l, (uint32_t)g_alive);
        SCat(l, sizeof l, "  SHOWN:"); SCatU(l, sizeof l, (uint32_t)g_drawn);
        if (CC.show) { SCat(l, sizeof l, "  COLL:"); SCatU(l, sizeof l, (uint32_t)g_collDrawn); }
        if (g_nv >= MAX_VTX * 2 / 3) SCat(l, sizeof l, "  (BUFFER FULL)");
        Text(l, 8 * k6, 8 * k6, 1.2f * k6, 0xD0FFFFFF);
    }
    if (mode != 1 && !C.hideHud && g_flying) {
        char fl[96]; fl[0] = 0; SCat(fl, sizeof fl, "LETANI  RYCHLOST "); SCatU(fl, sizeof fl, (uint32_t)(g_flySpeed + 0.5f));
        SCat(fl, sizeof fl, " M/S   KOLECKO: RYCHLOST  SHIFT: X3"); Text(fl, 8 * k6, 22 * k6, 1.1f * k6, 0xE0FFD040);
    }
    if (mode != 1 && g_menu) DrawMenu((float)sw, (float)sh);
    if (g_nv < 3 && g_ntv < 3) return;
    if (!inScene) o_begin(g);
    o_setTex(g, NULL);
    SetOurStates();
    if (nvZ > 0 && g_rsSet) {
        IDirect3DDevice8* dev = GetDev();
        uint32_t zm = ZMode();
        if (gpu && g_rsOld[5] != 0) zm = g_rsOld[5];                // exactly what the scene used
        static uint32_t s_zmLogged = 0;
        if (zm != s_zmLogged) { s_zmLogged = zm; Log(zm == D3DZB_USEW ? "depth: game uses W-buffer" : "depth: game uses Z-buffer"); }
        D3DMATRIX oldProj; bool projSet = false;
        if (!gpu && dev && zm == D3DZB_USEW && dev->GetTransform(D3DTS_PROJECTION, &oldProj) == D3D_OK) {
            dev->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX*)g_camProj);   // w range = the camera's near/far
            projSet = true;
        }
        int c0 = g_collEnd < nvZ ? g_collEnd : nvZ;
        if (c0 > 0) DrawRange(g, 0, c0);                                // collision x-ray: no depth test
        if (dev) dev->SetRenderState(D3DRS_ZENABLE, zm);
        DrawRange(g, c0, nvZ);
        DrawTV(dev);                                                    // label text, same depth test
        if (dev) dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        if (projSet) dev->SetTransform(D3DTS_PROJECTION, &oldProj);
        DrawRange(g, nvZ, g_nv);
    } else { DrawRange(g, 0, g_nv); DrawTV(GetDev()); }
    RestoreStates();
    if (!inScene) o_end(g);
}

// ------------------------------------------------------------------ god mode + infinite ammo (Game.exe 1.2)
// C_player vtable 0x625878 (player ctor 0x58f1c4): slot 30 Collision (0x5941c0, 6 args, ret 0x18),
// slot 31 Hit (0x594210, 8 args, ret 0x20 -> human_hit 0x5762a0 + HUD health). With god mode on both
// return 0 without doing anything - the same thing the game itself does while the player's controls
// are locked (cutscenes), so bullets, shotgun, explosions and cars hitting the player do nothing.
// Health: human+0x604.. = max values (Energy, 4 limbs), +0x644.. = current; refilled every frame too.
// Ammo: weapon in hand = human+0x4a0 id (u16), +0x4a4 rounds in the magazine, +0x4a8 reserve;
// item table [0x6d4c14] (count [0x6d4c18], 0xbc per item): +0x60 magazine size, +0x64 reserve max.
static const uint32_t PV_COLL = 0x006258F0, PV_HIT = 0x006258F4;
typedef char (THISC *Fn_PlColl)(void* self, void* a, void* b, void* c, void* d, float e, void* f);
typedef char (THISC *Fn_PlHit)(void* self, int t, void* a, void* b, void* c, float dmg, void* act, uint32_t u, void* fr);
static Fn_PlColl o_plColl = NULL;
static Fn_PlHit  o_plHit = NULL;
static char THISC PlCollHook(void* self, void* a, void* b, void* c, void* d, float e, void* f) {
    if (C.god) return 0;
    return o_plColl(self, a, b, c, d, e, f);
}
static char THISC PlHitHook(void* self, int t, void* a, void* b, void* c, float dmg, void* act, uint32_t u, void* fr) {
    if (C.god) return 0;
    return o_plHit(self, t, a, b, c, dmg, act, u, fr);
}
// ---- language: G_TextDatabase::GetText 0x60fb40 / GetTextOrNULL 0x60fba0 (thiscall id, ret 4; db 0x6d8714).
// With 'English' on, ids found in tables\devtools_en.def (same format as textdb_xx.def: u32 count,
// u32 0, count x {u32 id, u32 file offset}, cp1250 strings) are returned from it, everything else from
// the game's own textdb. Menu widgets read their text when the screen opens -> applies on reopening.
typedef const char* (THISC *Fn_GetText)(void* db, uint32_t id);
static Fn_GetText o_getText = NULL, o_getTextN = NULL;
static uint8_t* g_enBuf = NULL; static uint32_t g_enN = 0;
static const char* EnText(uint32_t id) {
    if (!C.langEn || !g_enBuf) return NULL;
    const uint32_t* r = (const uint32_t*)(g_enBuf + 8);
    for (uint32_t i = 0; i < g_enN; ++i) if (r[i * 2] == id) return (const char*)(g_enBuf + r[i * 2 + 1]);
    return NULL;
}
static const char* THISC GetTextHook(void* db, uint32_t id) { const char* t = EnText(id); return t ? t : o_getText(db, id); }
static const char* THISC GetTextNHook(void* db, uint32_t id) { const char* t = EnText(id); return t ? t : o_getTextN(db, id); }
static void InstallLang() {
    char p[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, p, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    while (n && p[n - 1] != '\\') --n; p[n] = 0;
    SCat(p, MAX_PATH, "tables\\devtools_en.def");
    HANDLE f = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(f, NULL), rd = 0;
        if (sz >= 8 && sz < 4 * 1024 * 1024) {
            g_enBuf = (uint8_t*)VirtualAlloc(NULL, sz + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (g_enBuf && ReadFile(f, g_enBuf, sz, &rd, NULL) && rd == sz) {
                g_enBuf[sz] = 0; g_enN = *(uint32_t*)g_enBuf;
                if (8 + (uint64_t)g_enN * 8 > sz) g_enN = 0;
                for (uint32_t i = 0; i < g_enN; ++i) if (((uint32_t*)(g_enBuf + 8))[i * 2 + 1] >= sz) g_enN = 0;
            } else g_enN = 0;
        }
        CloseHandle(f);
    }
    { char l[200]; l[0] = 0; SCat(l, sizeof l, "language: tables\\devtools_en.def "); SCat(l, sizeof l, g_enN ? "loaded, texts " : "NOT found - English off");
      if (g_enN) SCatU(l, sizeof l, g_enN); Log(l); }
    static const uint8_t k[8] = { 0x8B, 0x51, 0x04, 0xB8, 0x00, 0x10, 0x00, 0x00 };
    o_getText  = (Fn_GetText)InlineHook(0x0060FB40, k, 8, (void*)&GetTextHook, "textdb GetText");
    o_getTextN = (Fn_GetText)InlineHook(0x0060FBA0, k, 8, (void*)&GetTextNHook, "textdb GetTextOrNULL");
}
static void InstallCheats() {
    if (!Readable((void*)(uintptr_t)PV_COLL, 8) || *(uint32_t*)(uintptr_t)PV_COLL != 0x005941C0 ||
        *(uint32_t*)(uintptr_t)PV_HIT != 0x00594210) { Log("ERROR: player vtable differs - god mode not available"); return; }
    o_plColl = (Fn_PlColl)(uintptr_t)0x005941C0; o_plHit = (Fn_PlHit)(uintptr_t)0x00594210;
    bool ok = WriteCode32(PV_COLL, (uint32_t)(uintptr_t)&PlCollHook) && WriteCode32(PV_HIT, (uint32_t)(uintptr_t)&PlHitHook);
    Log(ok ? "god mode hook installed (player Hit / Collision)" : "ERROR: VirtualProtect on player vtable failed");
}
// ---- flying (noclip). C_player vtable slot 14 (0x6258b0) = Update (thiscall this, dt; jmp human_update
// 0x572b80). After the game's own update the player is put back at our position, which moves with
// W/S (where the camera looks), A/D (sideways), SPACE up, CTRL down, SHIFT fast. Active while
// while the menu option "Létání" is on.
static const uint32_t PV_UPDATE = 0x006258B0;
typedef void (THISC *Fn_PlUpdate)(void* self, uint32_t dt);
static Fn_PlUpdate o_plUpdate = NULL;
static V3 g_flyPos = { 0, 0, 0 };
static DWORD g_flyT = 0, g_flySecT = 0;
static bool GameFocused() {
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
static void FlySetPos(uint8_t* pl, V3 p, bool link) {
    uint8_t* fr = *(uint8_t**)(pl + 0x68);
    if (!Readable(fr, 0x80)) return;
    ((Fn_ActSetPos)A_ACT_SETPOS)(pl, p.x, p.y, p.z);
    ((Fn_FrSetPos)A_FRAME_SETPOS)(fr, &p);
    ((Fn_FrUpdate)(*(void***)fr)[0x18 / 4])(fr);
    if (link) {
        uint8_t* mission = *(uint8_t**)A_MISSION;
        if (Readable(mission, 0x14)) { void* sec = *(void**)(mission + 0x10); if (Readable(sec, 4)) ((Fn_SecLink)A_SECTOR_LINK)(sec, fr); }
    }
}
// human+0x418 = fall speed (movement integrator adds 9.81*dt; above 2.0 the human switches to
// movement mode +0x40c = 3 = falling + fall animation). Kept at 0 while flying -> no falling at all,
// WSAD plays the normal walk / run animations.
static void NoFall(uint8_t* pl) {
    *(float*)(pl + 0x418) = 0.0f;
    if (*(uint32_t*)(pl + 0x40c) == 3) *(uint32_t*)(pl + 0x40c) = 1;
}
// The movement integrator (thiscall this, dt; ret 4) - gravity, ground test, the switch to 'falling'.
// For the flying player it is skipped completely: the position is ours, the controls still pick the
// walk / run animation.
typedef void (THISC *Fn_Move)(void* self, uint32_t dt);
static Fn_Move o_move = NULL;
static void THISC MoveHook(void* self, uint32_t dt) {
    if (g_flying && self == (void*)GetPlayer()) return;
    o_move(self, dt);
}
// C_human::Do_Jump 0x586190 (thiscall, no args, plain ret; the player's SPACE at 0x590098) - while
// flying SPACE means 'up', the jump would start its animation and lock the player in it
typedef void (THISC *Fn_Jump)(void* self);
static Fn_Jump o_jump = NULL;
static void THISC JumpHook(void* self) {
    if (g_flying && self == (void*)GetPlayer()) return;
    o_jump(self);
}
// mouse wheel (IGraph vt 0xE8, 120 per notch): while flying it sets the speed and the game gets 0
// (no weapon switching)
static Fn_Int o_rz = NULL;
extern "C" STDC int H_rz(void* g) { int v = o_rz(g); return g_flying ? 0 : v; }
static void THISC PlUpdateHook(void* self, uint32_t dt) {
    uint8_t* pl = (uint8_t*)self;
    bool isPl = pl == GetPlayer();
    if (isPl && g_flying) NoFall(pl);
    o_plUpdate(self, dt);
    if (!isPl) return;
    bool focus = GameFocused() && !g_scr;
    bool want = C.fly && !*(void**)(pl + 0x98);                    // menu "Létání" on, not in a vehicle
    if (want != g_flying) {
        g_flying = want;
        if (g_flying) { ActorPos(pl, g_flyPos); g_flyT = GetTickCount(); }
        Log(g_flying ? "flying ON" : "flying OFF");
    }
    if (!g_flying) return;
    DWORD now = GetTickCount();
    float t = (now - g_flyT) / 1000.0f; g_flyT = now;
    if (t > 0.1f) t = 0.1f;
    if (focus) {
        uint8_t* cf = GetCameraFrame();
        V3 f = { 0, 0, 1 }, r = { 1, 0, 0 };
        if (cf) { f = RdV3(cf + 0x30); r = RdV3(cf + 0x10); }
        float lf = Len(f), lr = Len(r);
        if (lf > 1e-4f) f = Mul(f, 1.0f / lf); if (lr > 1e-4f) r = Mul(r, 1.0f / lr);
        r.y = 0; lr = Len(r); if (lr > 1e-4f) r = Mul(r, 1.0f / lr);
        V3 m = { 0, 0, 0 };
        if (KeyDown('W')) m = Add(m, f);
        if (KeyDown('S')) m = Sub(m, f);
        if (KeyDown('D')) m = Add(m, r);
        if (KeyDown('A')) m = Sub(m, r);
        if (KeyDown(VK_SPACE)) m.y += 1;
        if (KeyDown(VK_CONTROL)) m.y -= 1;
        void** pg = (void**)(uintptr_t)A_IGRAPH_PTR;
        if (o_rz && Readable(pg, 4) && *pg) {
            int z = o_rz(*pg);
            if (z) {
                int n = z / 120; if (!n) n = z > 0 ? 1 : -1;
                for (; n > 0; --n) g_flySpeed *= 1.25f;
                for (; n < 0; ++n) g_flySpeed /= 1.25f;
                if (g_flySpeed < 1.0f) g_flySpeed = 1.0f; if (g_flySpeed > 300.0f) g_flySpeed = 300.0f;
            }
        }
        float sp = KeyDown(VK_SHIFT) ? g_flySpeed * 3.0f : g_flySpeed;
        float lm = Len(m);
        if (lm > 1e-4f) g_flyPos = Add(g_flyPos, Mul(m, sp * t / lm));
    }
    bool link = now - g_flySecT > 500; if (link) g_flySecT = now;
    NoFall(pl);
    FlySetPos(pl, g_flyPos, link);
}
static void InstallFly() {
    if (!Readable((void*)(uintptr_t)PV_UPDATE, 4) || *(uint32_t*)(uintptr_t)PV_UPDATE != 0x00594050) { Log("ERROR: player vtable differs - flying not available"); return; }
    o_plUpdate = (Fn_PlUpdate)(uintptr_t)0x00594050;
    static const uint8_t kMove[7] = { 0x83, 0xEC, 0x60, 0x8B, 0x44, 0x24, 0x64 };
    o_move = (Fn_Move)InlineHook(0x0057B660, kMove, 7, (void*)&MoveHook, "human movement (flying)");
    static const uint8_t kJump[6] = { 0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1 };
    o_jump = (Fn_Jump)InlineHook(0x00586190, kJump, 6, (void*)&JumpHook, "human jump (flying)");
    Log(WriteCode32(PV_UPDATE, (uint32_t)(uintptr_t)&PlUpdateHook) ? "flying hook installed (menu L\xE9t\xE1n\xED)" : "ERROR: VirtualProtect on player vtable failed (flying)");
}
typedef void (THISC *Fn_HudHealth)(void* hud, int percent);
static void CheatsTick() {
    if (!C.god && !C.infAmmo) return;
    uint8_t* pl = GetPlayer();
    if (!pl || *(const int*)(pl + 0x10) != 2) return;              // kind 2 = C_player
    if (C.god) {
        bool changed = false;
        for (int i = 1; i <= 5; ++i) {                              // Energy + the four limbs
            float mx = *(float*)(pl + 0x600 + i * 4);
            float* cur = (float*)(pl + 0x640 + i * 4);
            if (mx > 0.5f && mx < 100000.0f && *cur < mx) { *cur = mx; if (i == 1) changed = true; }
        }
        if (changed) ((Fn_HudHealth)(uintptr_t)0x005F88E0)((void*)(uintptr_t)0x006BF980, 100);
    }
    if (C.infAmmo) {
        uint32_t id = *(const uint16_t*)(pl + 0x4a0);
        uint32_t cnt = *(const uint32_t*)(uintptr_t)0x006D4C18;
        const uint8_t* tab = *(const uint8_t* const*)(uintptr_t)0x006D4C14;
        if (id > 1 && id < cnt && Readable(tab + id * 0xbc, 0xbc)) {
            const uint8_t* it = tab + id * 0xbc;
            uint32_t mag = *(const uint32_t*)(it + 0x60), res = *(const uint32_t*)(it + 0x64);
            uint32_t* curMag = (uint32_t*)(pl + 0x4a4); uint32_t* curRes = (uint32_t*)(pl + 0x4a8);
            if (mag > 0 && mag < 10000 && *curMag < mag) *curMag = mag;
            if (res > 0 && res < 100000 && *curRes < res) *curRes = res;
        }
    }
}

// The game's 2D render callback (0x5BD8D0, stage 2) runs inside the scene after the 3D world:
// 0x5BD8E8 call HUD render, then the open menu. We take that call: our 3D overlay is drawn first,
// so the HUD and every menu end up on top of it; the DebugText panel goes right after the HUD.
static const uint32_t A_2D_HUD_CALL = 0x005BD8E8;   // call 0x5FB060 (G_IndicatorsClass::Render, ecx = 0x6BF980)
static const uint32_t A_2D_HUD_FN   = 0x005FB060;
typedef void (THISC *Fn_HudRender)(void* hud);
static uint32_t g_hudNext = 0;                      // what the call pointed at (the game's HUD or another mod's hook)
static volatile bool g_drew2D = false;              // the 2D phase ran this frame - Present has nothing to draw
static volatile bool g_drew1 = false;               // stage 1 drew the world this frame
// The scene callback 0x5BD8D0 (stdcall stage, a, b; registered with flags 6 = stages 1 and 2). The game
// only uses stage 2 (HUD); stage 1 runs right after the 3D world, with its depth buffer and matrices
// still on the device - there our overlay is drawn so it sits in the scene like a model.
typedef int (STDC *Fn_SceneCb)(int stage, int a, int b);
static Fn_SceneCb o_sceneCb = NULL;
extern "C" int STDC SceneCbHook(int stage, int a, int b) {
    if (stage == 1) {
        void** pg = (void**)(uintptr_t)A_IGRAPH_PTR;
        if (o_sx && Readable(pg, 4) && *pg) {
            FrameTV(*pg, true, 1);
            g_drew1 = g_st1OK;
        }
    }
    return o_sceneCb(stage, a, b);
}
static void InstallStage1() {
    static const uint8_t k[7] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xE8, 0x02 };
    o_sceneCb = (Fn_SceneCb)InlineHook(0x005BD8D0, k, 7, (void*)&SceneCbHook, "scene callback (stage 1)");
    if (o_sceneCb) Log("drawing hooked into the 3D scene (stage 1) - real depth test");
}
static void THISC Hud2DHook(void* hud) {
    void** pg = (void**)(uintptr_t)A_IGRAPH_PTR;
    if (o_sx && Readable(pg, 4) && *pg) { FrameTV(*pg, true, g_drew1 ? 2 : 0); g_drew2D = true; }
    g_drew1 = false;
    if (!C.hideHud) {                                   // 'Skrýt HUD': the game HUD and the DebugText panel are skipped;
        ((Fn_HudRender)(uintptr_t)g_hudNext)(hud);      // the menus (drawn after this call) and the scene (sun glare) are untouched
        DT_AfterHud();
    }
}
static bool Install2D() {
    uint8_t* p = (uint8_t*)(uintptr_t)A_2D_HUD_CALL;
    if (!Readable(p, 5) || p[0] != 0xE8) { Log("ERROR: HUD render call not found - drawing on top of everything (Present)"); return false; }
    uint32_t tgt = A_2D_HUD_CALL + 5 + *(uint32_t*)(p + 1);
    if (tgt != A_2D_HUD_FN) Log("note: HUD render call already hooked by another mod - chaining");
    g_hudNext = tgt;
    DWORD old;
    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(uint32_t*)(p + 1) = (uint32_t)(uintptr_t)&Hud2DHook - (A_2D_HUD_CALL + 5);
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);
    Log("drawing hooked into the 2D phase (under HUD and menus)");
    return true;
}

extern "C" STDC void H_Present(void* g) {
    if (!g_drew2D) FrameTV(g, false, 0);
    g_drew1 = false;                // no 2D phase this frame (or hook missing): draw on top
    g_drew2D = false;
    o_present(g);
}

static bool HookIGraph() {
    g_ls3d = (uint8_t*)GetModuleHandleA("LS3DF.dll");
    if (!g_ls3d) { Log("ERROR: LS3DF.dll not loaded"); return false; }
    void** vt = (void**)(g_ls3d + LS3DF_VT_RVA);
    if (IsBadReadPtr(vt, 0x104) || vt[VT_DPL / 4] != (void*)(g_ls3d + LS3DF_DPL_RVA)) {
        Log("ERROR: IGraph vtable not where expected (other LS3DF.dll version?)"); return false;
    }
    o_begin = (Fn_Void)vt[VT_BEGIN / 4]; o_end = (Fn_Void)vt[VT_END / 4];
    o_setTex = (Fn_SetTex)vt[VT_SETTEX / 4]; o_dpl = (Fn_DPL)vt[VT_DPL / 4];
    o_sx = (Fn_Int)vt[VT_SX / 4]; o_sy = (Fn_Int)vt[VT_SY / 4];
    bool ok = PatchPtr(&vt[VT_PRESENT / 4], (void*)&H_Present, (void**)&o_present);
    if (ok && !PatchPtr(&vt[0xE8 / 4], (void*)&H_rz, (void**)&o_rz)) Log("ERROR: mouse wheel hook failed");
    Log(ok ? "IGraph Present hooked" : "ERROR: VirtualProtect on IGraph vtable failed");
    return ok;
}

// ------------------------------------------------------------------ entry
static DWORD WINAPI InitThread(LPVOID) {
    for (int i = 0; i < 200 && !GetModuleHandleA("LS3DF.dll"); ++i) Sleep(50);
    HookIGraph();
    return 0;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(self);
    char path[MAX_PATH]; DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return TRUE;
    int dot = (int)n; while (dot > 0 && path[dot] != '.' && path[dot] != '\\') --dot;
    if (dot <= 0 || path[dot] != '.') dot = (int)n;
    path[dot] = 0;
    g_ini[0] = 0; SCat(g_ini, MAX_PATH, path); SCat(g_ini, MAX_PATH, ".ini");
    g_logPath[0] = 0; SCat(g_logPath, MAX_PATH, path); SCat(g_logPath, MAX_PATH, ".log");
    // one DeveloperTools per game process: a second copy (e.g. the same .asi in both the game folder and
    // scripts\) would hook everything again with its own .ini and draw on top of the first one
    {
        char mx[64]; mx[0] = 0; SCat(mx, sizeof mx, "Local\\DeveloperTools_Mafia12_"); SCatU(mx, sizeof mx, GetCurrentProcessId());
        CreateMutexA(NULL, FALSE, mx);   // handle kept for the life of the process
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            g_log = CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
            Log("Developer Tools for Mafia 1.2");
            Log("ANOTHER COPY OF DeveloperTools IS ALREADY LOADED - this one does nothing. Delete the duplicate .asi:");
            Log(path);
            if (g_log != INVALID_HANDLE_VALUE) { CloseHandle(g_log); g_log = INVALID_HANDLE_VALUE; }
            return TRUE;
        }
    }
    LoadCfg(); LoadCollCfg();
    if (C.log) g_log = CreateFileA(g_logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    Log("Developer Tools for Mafia 1.2");
    if (!C.enabled) { Log("Enabled=0 - nothing hooked"); return TRUE; }
    g_show = C.showOnStart != 0;
    ClearProgs();
    HookScript();
    InstallGameMenu();
    InstallCheats();
    InstallLang();
    InstallFly();
    Install2D();
    InstallStage1();
    if (!DT_Start(path)) Log("DebugText not started - see developer_tools_DebugText.log");
    { char tl[MAX_PATH]; tl[0] = 0; SCat(tl, MAX_PATH, path); SCat(tl, MAX_PATH, "_Tools.log"); MT_Start(g_ini, tl); }   // wireframe + free camera (menu)
    if (GetModuleHandleA("LS3DF.dll")) HookIGraph();
    else { HANDLE t = CreateThread(NULL, 0, InitThread, NULL, 0, NULL); if (t) CloseHandle(t); }
    return TRUE;
}
