// The mission's scene drawn again from mods' cameras (mml.h scene_view; docs/MODDING.md "The scene from another
// camera"). After mafia-ragdoll's reflection.cpp and scene_camera.cpp, for any number of views of any frustum.
//
// LS3D's I3D_driver::Render is re-entrant and draws from whatever its scene's camera says. Before the frame's own
// Render (render.cpp's hook: the outermost call, the mission's world), for each view:
//   1. its function places it (the camera given filled with the game camera's pose, frustum and range);
//   2. the scene's camera (an I3D_camera) is kept byte for byte (its world matrix, flags, serial, and everything from
//      its field of view to its end: the projections, the view, the frustum's planes, the sector), and turned into the
//      view's: its world matrix written, its field of view and aspect set by its own setters to a symmetric frustum
//      that holds the view's (LS3D culls with that one), its update called (the view, the frustum, the sector: held at
//      the game camera's unless the view asks for its own, I3D_scene::GetSector hooked for that call), then its three
//      projections (the world's, the backdrop's, the near pass') given the view's sides (_11, _22, _31, _32), their
//      depth kept, and the matrices LS3D multiplies from them made again; its serial one the game camera never has;
//   3. the scene's callback replaced by the loader's (stage 1 only: MML_EVENT_VIEW_WORLD, the view's linear depth),
//      its 3D sound listener detached, its rectangle the view's size, its lens flares kept out (QueueVisual hooked:
//      'LENS' visuals not queued while a view draws; their list emptied for the drawing and put back: the frame's
//      flares keep their fades);
//   4. the graphics bridge binds the view's targets (gfx.cpp GfxViewBegin: colour, a readable depth buffer, the clip
//      plane) and LS3D's Render draws the scene;
//   5. everything is put back, and Render is told to work the game camera's lights and sectors out again.
// A fault in any step puts back what was changed; the view says why it was not drawn.
//
// Bridge methods (game thread): view.state (the hooks, every view: its mod, size, times drawn, skipped, the last
// reason, the CPU time, the camera drawn), view.show {handle, rect: [x, y, w, h] in 800x600 units, or none} (an
// agent's eyes: the view copied onto the frame).
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

// LS3D's scene and camera (the same layout in 1.2 and 1.0: game_build.h "The scene drawn from another camera").
constexpr int kSceneCallback = 0x140, kSceneMask = 0x144, kSceneCamera = 0x17C, kSceneRect = 0x23C, kSceneListener = 0x284,
              kSceneLensCount = 0x290;
constexpr int kCamWorld = 0x10, kCamFlags = 0xAC, kCamSerial = 0x118, kCamParams = 0x140, kCamEnd = 0x4F4;
constexpr int kCamNear = 0x144, kCamFar = 0x148, kCamOrtho = 0x15C;
// the matrices the update and the setters make: the view, the projection, view x projection; the backdrop's (the
// view without its position) and the near pass' (its own field of view and range)
constexpr int kCamView = 0x164, kCamProj = 0x1A4, kCamViewProj = 0x1E4, kCamBackView = 0x264, kCamBackProj = 0x2E4,
              kCamBackViewProj = 0x324, kCamNearProj = 0x364, kCamNearViewProj = 0x3A4;
constexpr uint32_t kWorldValid = 0x20;
constexpr int kVtUpdate = 0x44, kVtSetFov = 0x50, kVtSetNearFov = 0x54, kVtSetRange = 0x58, kVtSetAspect = 0x68;
constexpr int kVisualType = 0x1D4;
constexpr uint32_t kTypeLens = 0x534E454C;   // 'LENS'
constexpr int kMaxViews = 8;

using GetSectorFn = void *(__stdcall *)(void *scene, const float *pos, void *hint);
using QueueFn = void(__fastcall *)(void *visual, void *edx);
using UpdateFn = void(__fastcall *)(void *self, void *edx);
using SetFloatFn = int(__stdcall *)(void *cam, float v);
using SetRangeFn = int(__stdcall *)(void *cam, float lo, float hi);

struct View {
    MmlMod *mod;
    int handle;
    int w, h;
    MmlViewFn fn;
    void *user;
    unsigned drawn = 0, skipped = 0, faults = 0;
    double ms = 0;   // smoothed CPU time of a drawing
    std::string last = "not drawn yet";
    float eye[3] = {}, fwd[3] = {};
};
std::vector<View> g_views;
int g_nextHandle = 1;

uintptr_t g_ls3d;
bool g_hooksTried, g_hooked;
std::string g_hookWhy;
GetSectorFn oGetSector;
QueueFn oQueue;
const float *g_holdPos;
void *g_holdSector;
bool g_drawing;
bool g_shadowDrawing;   // (the scene drawn from the sun for shadows.cpp: no mod's view, no stage-1 work)
unsigned g_serial = 0xB0000000u;   // the views' camera serials: apart from the game camera's (and mafia-ragdoll's)
LARGE_INTEGER g_freq;

void *__stdcall hkGetSector(void *scene, const float *pos, void *hint) {
    if (g_holdSector && pos == g_holdPos) return g_holdSector;
    return oGetSector(scene, pos, hint);
}
void __fastcall hkQueue(void *visual, void *edx) {
    if (g_drawing && *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(visual) + kVisualType) == kTypeLens) return;
    oQueue(visual, edx);
}

bool EnsureHooks() {
    if (g_hooksTried) return g_hooked;
    g_hooksTried = true;
    const game::Ls3dBuild &l = game::Ls3d();
    g_ls3d = ModuleBase("LS3DF.dll");
    if (!l.cameraVtable || !l.sceneGetSector || !l.queueVisual || !l.camRecompute || !l.frameUpdateWMatrix) {
        g_hookWhy = "not known for this LS3DF.dll";
        return false;
    }
    std::string err;
    if (!HookAdd(nullptr, g_ls3d + l.sceneGetSector, reinterpret_cast<void *>(&hkGetSector), reinterpret_cast<void **>(&oGetSector), &err)) {
        g_hookWhy = "I3D_scene::GetSector not hooked: " + err;
        return false;
    }
    // Without it the lens flares cannot be kept out (the frame's would start over every frame): no views then.
    if (!HookAdd(nullptr, g_ls3d + l.queueVisual, reinterpret_cast<void *>(&hkQueue), reinterpret_cast<void **>(&oQueue), &err)) {
        HookRemoveLink(nullptr, g_ls3d + l.sceneGetSector, reinterpret_cast<void *>(&hkGetSector));
        g_hookWhy = "QueueVisual not hooked: " + err;
        return false;
    }
    g_hooked = true;
    HLOG("view: I3D_scene::GetSector and QueueVisual hooked (scene views)");
    return true;
}

template <class T> T &At(void *p, int off) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(p) + off); }
template <class F> F Slot(void *obj, int off) { return reinterpret_cast<F>((*reinterpret_cast<void ***>(obj))[off / 4]); }

void Mul44(const float *a, const float *b, float *out) {   // out = a * b (row vectors)
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(out, r, sizeof r);
}
void Cross(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
bool Normalize(float *v) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(l > 1e-6f)) return false;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}

// The scene's camera, its world matrix up to date; null when it is not LS3D's I3D_camera of this build.
uint8_t *CameraOf(void *scene) {
    uint8_t *cam = At<uint8_t *>(scene, kSceneCamera);
    if (!cam || *reinterpret_cast<uintptr_t *>(cam) != g_ls3d + game::Ls3d().cameraVtable) return nullptr;
    if (!(At<uint32_t>(cam, kCamFlags) & kWorldValid))
        reinterpret_cast<UpdateFn>(g_ls3d + game::Ls3d().frameUpdateWMatrix)(cam, nullptr);
    return cam;
}

// The game camera as a view's starting point.
void GameCamera(uint8_t *cam, MmlViewCamera *c) {
    memset(c, 0, sizeof *c);
    c->size = sizeof *c;
    memcpy(c->world, cam + kCamWorld, sizeof c->world);
    const float *p = reinterpret_cast<const float *>(cam + kCamProj);
    c->tan_left = c->tan_right = p[0] != 0 ? 1.0f / p[0] : 1.0f;
    c->tan_down = c->tan_up = p[5] != 0 ? 1.0f / p[5] : 0.75f;
    c->near_z = At<float>(cam, kCamNear);
    c->far_z = At<float>(cam, kCamFar);
}

// What the camera's change touches, kept to be put back: its world matrix, flags, serial and everything from its
// field of view to its end (the projections, the view, the frustum's planes, the sector).
struct CamSaved {
    uint8_t world[64];
    uint32_t flags, serial;
    uint8_t params[kCamEnd - kCamParams];
};
void SaveCam(uint8_t *cam, CamSaved *s) {
    memcpy(s->world, cam + kCamWorld, sizeof s->world);
    s->flags = At<uint32_t>(cam, kCamFlags);
    s->serial = At<uint32_t>(cam, kCamSerial);
    memcpy(s->params, cam + kCamParams, sizeof s->params);
}
void RestoreCam(uint8_t *cam, const CamSaved &s) {
    memcpy(cam + kCamWorld, s.world, sizeof s.world);
    At<uint32_t>(cam, kCamFlags) = s.flags;
    At<uint32_t>(cam, kCamSerial) = s.serial;
    memcpy(cam + kCamParams, s.params, sizeof s.params);
    *reinterpret_cast<uint8_t *>(g_ls3d + game::Ls3d().camRecompute) = 1;   // the camera's lights, sectors again
}

// The camera turned into `c` (made orthonormal; the frustum LS3D culls with, the projections' sides, the sector held
// at the camera's or looked up at the new eye). False with why: nothing changed. `saved` gets what was there first.
bool PutCamera(uint8_t *cam, const MmlViewCamera &c, CamSaved *saved, std::string *why) {
    float m[16];
    memcpy(m, c.world, sizeof m);
    float *r = m, *u = m + 4, *f = m + 8;
    if (!Normalize(f)) { *why = "its forward is zero"; return false; }
    Cross(u, f, r);   // right = up x forward (left-handed)
    if (!Normalize(r)) { *why = "its up is along its forward"; return false; }
    Cross(f, r, u);
    m[3] = m[7] = m[11] = 0;
    m[15] = 1;
    float tl = c.tan_left, tr = c.tan_right, td = c.tan_down, tu = c.tan_up;
    if (!(tl + tr > 1e-4f) || !(td + tu > 1e-4f) || !std::isfinite(tl + tr + td + tu)) { *why = "its frustum is empty"; return false; }
    if (At<uint8_t>(cam, kCamOrtho)) { *why = "the game camera is orthographic"; return false; }
    SaveCam(cam, saved);
    // the symmetric frustum LS3D culls with: it holds the view's
    float tx = std::max(std::max(tl, tr), 1e-3f), ty = std::max(std::max(td, tu), 1e-3f);
    float fov = std::min(2.0f * std::atan(tx), 3.1f), aspect = std::min(std::max(tx / ty, 0.011f), 99.0f);
    Slot<SetFloatFn>(cam, kVtSetFov)(cam, fov);
    Slot<SetFloatFn>(cam, kVtSetNearFov)(cam, fov);
    Slot<SetFloatFn>(cam, kVtSetAspect)(cam, aspect);
    float nearZ = c.near_z, farZ = c.far_z;
    if ((nearZ != At<float>(cam, kCamNear) || farZ != At<float>(cam, kCamFar)) && nearZ > 0.001f && farZ > nearZ + 0.01f)
        Slot<SetRangeFn>(cam, kVtSetRange)(cam, nearZ, farZ);
    // the pose, its view, frustum and sector
    memcpy(cam + kCamWorld, m, sizeof m);
    At<uint32_t>(cam, kCamFlags) = saved->flags | kWorldValid;
    At<uint32_t>(cam, kCamSerial) = g_serial++;
    if (g_serial < 0xB0000000u || g_serial >= 0xC0000000u) g_serial = 0xB0000000u;
    if (!(c.flags & MML_VIEW_OWN_SECTOR)) {
        g_holdPos = reinterpret_cast<const float *>(cam + kCamWorld + 48);
        g_holdSector = At<void *>(cam, 0x3F8);   // the camera's
    }
    Slot<UpdateFn>(cam, kVtUpdate)(cam, nullptr);
    g_holdSector = nullptr;
    // the sides in each projection, their depth kept: NDC x = _11 x/z + _31 is -1 at -tl and 1 at tr
    for (int off : {kCamProj, kCamBackProj, kCamNearProj}) {
        float *p = reinterpret_cast<float *>(cam + off);
        p[0] = 2.0f / (tl + tr);
        p[8] = (tl - tr) / (tl + tr);
        p[5] = 2.0f / (td + tu);
        p[9] = (td - tu) / (td + tu);
    }
    auto M = [&](int o) { return reinterpret_cast<float *>(cam + o); };
    Mul44(M(kCamView), M(kCamProj), M(kCamViewProj));
    Mul44(M(kCamBackView), M(kCamBackProj), M(kCamBackViewProj));
    Mul44(M(kCamView), M(kCamNearProj), M(kCamNearViewProj));
    return true;
}

// What a view changes, kept to be put back: the camera's, and the scene's callback, listener, flares and rectangle.
struct Saved {
    CamSaved cam;
    void *callback, *listener;
    uint32_t mask, lensCount;
    int32_t rect[4];
};
Saved g_saved;
bool g_put;
void *g_putScene;

void Restore() {
    g_holdSector = nullptr;
    if (!g_put) return;
    g_put = false;
    void *scene = g_putScene;
    uint8_t *cam = At<uint8_t *>(scene, kSceneCamera);
    RestoreCam(cam, g_saved.cam);
    At<void *>(scene, kSceneCallback) = g_saved.callback;
    At<uint32_t>(scene, kSceneMask) = g_saved.mask;
    At<void *>(scene, kSceneListener) = g_saved.listener;
    At<uint32_t>(scene, kSceneLensCount) = g_saved.lensCount;
    memcpy(static_cast<uint8_t *>(scene) + kSceneRect, g_saved.rect, sizeof g_saved.rect);
}
DWORD SafeRestore() { __try { Restore(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { g_put = false; return GetExceptionCode(); } }

int __stdcall ViewStage(int stage, int, int);
int g_stageHandle;

// The scene's camera turned into the view's (see the top). False with why: nothing is left changed.
bool Put(void *scene, uint8_t *cam, const MmlViewCamera &c, int w, int h, std::string *why) {
    if (!PutCamera(cam, c, &g_saved.cam, why)) return false;
    g_saved.callback = At<void *>(scene, kSceneCallback);
    g_saved.mask = At<uint32_t>(scene, kSceneMask);
    g_saved.listener = At<void *>(scene, kSceneListener);
    g_saved.lensCount = At<uint32_t>(scene, kSceneLensCount);
    memcpy(g_saved.rect, static_cast<uint8_t *>(scene) + kSceneRect, sizeof g_saved.rect);
    g_put = true;
    g_putScene = scene;
    // the scene: the loader's callback, no listener, no lens flares on its list, the view's rectangle
    At<void *>(scene, kSceneCallback) = reinterpret_cast<void *>(&ViewStage);
    At<uint32_t>(scene, kSceneMask) = 1u << 1;
    At<void *>(scene, kSceneListener) = nullptr;
    At<uint32_t>(scene, kSceneLensCount) = 0;
    int32_t rect[4] = {0, 0, w, h};
    memcpy(static_cast<uint8_t *>(scene) + kSceneRect, rect, sizeof rect);
    return true;
}
DWORD SafePut(void *scene, uint8_t *cam, const MmlViewCamera &c, int w, int h, std::string *why, bool *ok) {
    __try { *ok = Put(scene, cam, c, w, h, why); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ok = false;
        DWORD code = GetExceptionCode();
        SafeRestore();
        return code;
    }
}
DWORD SafeRender(RenderFn render, void *driver, void *scene) {
    __try { render(driver, scene); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
DWORD SafeCamera(void *scene, uint8_t **cam) {
    __try { *cam = CameraOf(scene); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { *cam = nullptr; return GetExceptionCode(); }
}

int __stdcall ViewStage(int stage, int, int) {
    if (stage == 1 && !g_shadowDrawing) {
        RenderViewWorld(g_stageHandle);
        GfxViewStage1();
    }
    return 0;
}

struct PlaceCall { View *v; MmlViewCamera *c; int r; };
void CallPlace(void *p) {
    auto *pc = static_cast<PlaceCall *>(p);
    pc->r = pc->v->fn(pc->c, pc->v->user);
}

double NowMs() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return 1000.0 * t.QuadPart / g_freq.QuadPart; }

void DrawOne(View &v, void *driver, void *scene, uint8_t *cam, RenderFn render) {
    MmlViewCamera c;
    GameCamera(cam, &c);
    PlaceCall pc{&v, &c, 0};
    if (!ModCall(v.mod, CallPlace, &pc, "its scene view's function")) return;
    if (!pc.r) { v.skipped++; v.last = "skipped: its function said 0"; return; }
    c.size = sizeof c;
    double t0 = NowMs();
    std::string why;
    const float *clip = (c.flags & MML_VIEW_CLIP) ? c.clip : nullptr;
    unsigned w = static_cast<unsigned>(v.w), h = static_cast<unsigned>(v.h);   // (0, 0: the bridge gives the frame's)
    if (!GfxViewBegin(v.handle, &w, &h, clip, &why)) {
        v.skipped++;
        v.last = "skipped: " + why;
        return;
    }
    bool ok = false;
    DWORD code = SafePut(scene, cam, c, static_cast<int>(w), static_cast<int>(h), &why, &ok);
    float view[16] = {}, proj[16] = {}, eye[3] = {};
    bool drawn = false;
    if (ok) {
        memcpy(view, cam + kCamView, sizeof view);
        memcpy(proj, cam + kCamProj, sizeof proj);
        memcpy(eye, cam + kCamWorld + 48, sizeof eye);
        GfxViewBackdrop(reinterpret_cast<const float *>(cam + kCamBackView));
        g_stageHandle = v.handle;
        g_drawing = true;
        DWORD rc = SafeRender(render, driver, scene);
        g_drawing = false;
        g_stageHandle = 0;
        DWORD bc = SafeRestore();
        drawn = rc == 0 && bc == 0;
        char hex[16];
        snprintf(hex, sizeof hex, "%08X", static_cast<unsigned>(rc));
        if (rc) { v.faults++; v.last = std::string("a fault inside LS3D's drawing (exception ") + hex + ")"; }
        else if (bc) { v.faults++; v.last = "a fault while the camera was put back"; }
    } else {
        v.skipped++;
        v.last = code ? "skipped: a fault while the camera was set" : "skipped: " + why;
    }
    GfxViewEnd(drawn, view, proj, eye);
    if (!drawn) return;
    v.drawn++;
    v.last = "drawn";
    memcpy(v.eye, eye, sizeof eye);
    v.fwd[0] = view[2]; v.fwd[1] = view[6]; v.fwd[2] = view[10];
    double ms = NowMs() - t0;
    v.ms = v.drawn == 1 ? ms : v.ms * 0.9 + ms * 0.1;
}

// ---- the scene from the sun (shadows.cpp): LS3D's own orthographic camera
constexpr int kVtSetOrthoScale = 0x60, kVtSetOrtho = 0x64;   // stdcall(this, float) / (this, bool): the projections again
using SetBoolFn = int(__stdcall *)(void *cam, char on);

// The camera made orthographic at `world` (rows: right, up, forward, the eye), `half` metres each way, its depth from
// nearZ to farZ; its sector held at the game camera's. False with why: nothing changed.
bool PutOrtho(uint8_t *cam, const float world[16], float half, float nearZ, float farZ, CamSaved *saved, std::string *why) {
    if (At<uint8_t>(cam, kCamOrtho)) { *why = "the game camera is orthographic"; return false; }
    if (!(half > 0.01f) || !(farZ > nearZ + 0.01f) || !(nearZ > 0.001f)) { *why = "an empty box"; return false; }
    SaveCam(cam, saved);
    Slot<SetBoolFn>(cam, kVtSetOrtho)(cam, 1);
    Slot<SetFloatFn>(cam, kVtSetOrthoScale)(cam, 1.0f / half);
    Slot<SetFloatFn>(cam, kVtSetAspect)(cam, 1.0f);
    Slot<SetRangeFn>(cam, kVtSetRange)(cam, nearZ, farZ);
    memcpy(cam + kCamWorld, world, 64);
    At<uint32_t>(cam, kCamFlags) = saved->flags | kWorldValid;
    At<uint32_t>(cam, kCamSerial) = g_serial++;
    if (g_serial < 0xB0000000u || g_serial >= 0xC0000000u) g_serial = 0xB0000000u;
    g_holdPos = reinterpret_cast<const float *>(cam + kCamWorld + 48);
    g_holdSector = At<void *>(cam, 0x3F8);   // the camera's: the eye is above the city, in no room
    Slot<UpdateFn>(cam, kVtUpdate)(cam, nullptr);
    g_holdSector = nullptr;
    return true;
}

bool PutShadow(void *scene, uint8_t *cam, const float world[16], float half, float nearZ, float farZ, int size, std::string *why) {
    if (!PutOrtho(cam, world, half, nearZ, farZ, &g_saved.cam, why)) return false;
    g_saved.callback = At<void *>(scene, kSceneCallback);
    g_saved.mask = At<uint32_t>(scene, kSceneMask);
    g_saved.listener = At<void *>(scene, kSceneListener);
    g_saved.lensCount = At<uint32_t>(scene, kSceneLensCount);
    memcpy(g_saved.rect, static_cast<uint8_t *>(scene) + kSceneRect, sizeof g_saved.rect);
    g_put = true;
    g_putScene = scene;
    At<void *>(scene, kSceneCallback) = reinterpret_cast<void *>(&ViewStage);   // (stage 1: nothing in this view)
    At<uint32_t>(scene, kSceneMask) = 1u << 1;
    At<void *>(scene, kSceneListener) = nullptr;
    At<uint32_t>(scene, kSceneLensCount) = 0;
    int32_t rect[4] = {0, 0, size, size};
    memcpy(static_cast<uint8_t *>(scene) + kSceneRect, rect, sizeof rect);
    return true;
}
DWORD SafePutShadow(void *scene, uint8_t *cam, const float world[16], float half, float nearZ, float farZ, int size, std::string *why, bool *ok) {
    __try { *ok = PutShadow(scene, cam, world, half, nearZ, farZ, size, why); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) {
        *ok = false;
        DWORD code = GetExceptionCode();
        SafeRestore();
        return code;
    }
}

} // namespace

int ViewDrawingNow() { return g_drawing && !g_shadowDrawing ? g_stageHandle : 0; }
bool ShadowViewNow() { return g_shadowDrawing; }

bool ViewDrawShadow(void *driver, void *scene, RenderFn render, const float world[16], float half, float nearZ, float farZ, int size,
                    float viewProj[16], float backView[16], std::string *why) {
    if (!game::g_ls3d || !game::g_exe || g_drawing) { *why = "no game, or a view is being drawn"; return false; }
    if (!EnsureHooks()) { *why = g_hookWhy; return false; }
    uint8_t *cam = nullptr;
    if (SafeCamera(scene, &cam) || !cam) { *why = "the scene's camera is not LS3D's I3D_camera"; return false; }
    bool ok = false;
    DWORD code = SafePutShadow(scene, cam, world, half, nearZ, farZ, size, why, &ok);
    if (!ok) { if (code) *why = "a fault while the camera was set"; return false; }
    memcpy(viewProj, cam + kCamViewProj, 64);
    memcpy(backView, cam + kCamBackView, 64);
    ShadersShadowBegin(backView);   // (shaders.cpp: the sky told apart by its view)
    g_stageHandle = 0;
    g_drawing = true;
    g_shadowDrawing = true;
    DWORD rc = SafeRender(render, driver, scene);
    g_shadowDrawing = false;
    g_drawing = false;
    DWORD bc = SafeRestore();
    char hex[16];
    snprintf(hex, sizeof hex, "%08X", static_cast<unsigned>(rc ? rc : bc));
    if (rc) { *why = std::string("a fault inside LS3D's drawing (exception ") + hex + ")"; return false; }
    if (bc) { *why = "a fault while the camera was put back"; return false; }
    return true;
}

// ---- the frame's own camera overridden: one owner (the last mod that asked, or the bridge's camera.set)
namespace {
struct Override { MmlMod *mod; MmlViewFn fn; void *user; } g_ov;
struct HeldCam { bool on; float eye[3], fwd[3], up[3], fovY; DWORD until; } g_heldCam;
CamSaved g_frameSaved;
uint8_t *g_frameCam;
bool g_framePut;
unsigned g_ovFrames, g_ovSkipped, g_ovDeclined;
std::string g_ovLast = "none";

struct OvCall { MmlViewCamera *c; int r; };
void CallOv(void *p) {
    auto *oc = static_cast<OvCall *>(p);
    oc->r = g_ov.fn(oc->c, g_ov.user);
}
// The bridge's held camera into c: its pose; its field of view scaling both sides (the game's aspect kept).
void ApplyHeld(MmlViewCamera *c) {
    float f[3] = {g_heldCam.fwd[0], g_heldCam.fwd[1], g_heldCam.fwd[2]}, u[3] = {g_heldCam.up[0], g_heldCam.up[1], g_heldCam.up[2]}, r[3];
    Normalize(f);
    Cross(u, f, r);
    Normalize(r);
    Cross(f, r, u);
    float *w = c->world;
    for (int i = 0; i < 3; i++) { w[i] = r[i]; w[4 + i] = u[i]; w[8 + i] = f[i]; w[12 + i] = g_heldCam.eye[i]; }
    if (g_heldCam.fovY > 1 && g_heldCam.fovY < 170) {
        float t = std::tan(g_heldCam.fovY * 0.5f * 0.0174533f), k = t / std::max(c->tan_up, 1e-4f);
        c->tan_left *= k; c->tan_right *= k; c->tan_up = c->tan_down = t;
    }
    c->flags |= MML_VIEW_OWN_SECTOR;   // (anywhere in the city: its own room)
}
DWORD SafePutFrame(uint8_t *cam, const MmlViewCamera &c, std::string *why, bool *ok) {
    __try { *ok = PutCamera(cam, c, &g_frameSaved, why); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; return GetExceptionCode(); }
}
DWORD SafeRestoreFrame() {
    __try { RestoreCam(g_frameCam, g_frameSaved); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
} // namespace

int CameraOverride(MmlMod *mod, MmlViewFn fn, void *user) {
    if (!fn) { if (g_ov.mod == mod) g_ov = Override{}; return 1; }
    g_ov = Override{mod, fn, user};
    return 1;
}

void FrameCameraBegin(void *scene) {
    if (g_heldCam.on && static_cast<int>(g_heldCam.until - GetTickCount()) <= 0) g_heldCam.on = false;
    if (g_ov.fn && !ModAlive(g_ov.mod)) g_ov = Override{};
    if ((!g_ov.fn && !g_heldCam.on) || !game::g_ls3d || !game::g_exe || g_framePut) return;
    if (!EnsureHooks()) { g_ovLast = "skipped: " + g_hookWhy; return; }
    uint8_t *cam = nullptr;
    if (SafeCamera(scene, &cam) || !cam) { g_ovSkipped++; g_ovLast = "skipped: the scene's camera is not LS3D's I3D_camera"; return; }
    MmlViewCamera c;
    GameCamera(cam, &c);
    if (g_heldCam.on) {
        ApplyHeld(&c);
    } else {
        OvCall oc{&c, 0};
        if (!ModCall(g_ov.mod, CallOv, &oc, "its camera override's function")) { g_ovSkipped++; g_ovLast = "its function faulted"; return; }
        if (!oc.r) { g_ovDeclined++; g_ovLast = "the game's (its function returned 0)"; return; }
    }
    c.size = sizeof c;
    std::string why;
    bool ok = false;
    DWORD code = SafePutFrame(cam, c, &why, &ok);
    if (!ok) { g_ovSkipped++; g_ovLast = code ? "a fault while the camera was set" : "skipped: " + why; return; }
    g_frameCam = cam;
    g_framePut = true;
    g_ovFrames++;
    g_ovLast = "drawn";
}

void FrameCameraEnd() {
    if (!g_framePut) return;
    g_framePut = false;
    if (SafeRestoreFrame()) g_ovLast = "a fault while the game's camera was put back";
}

bool CameraMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "camera.state" && method != "camera.set" && method != "camera.release") return false;
    *ok = true;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto v3 = [&](const char *k, float *out) {
        if (!p.contains(k) || !p[k].is_array() || p[k].size() != 3) return false;
        for (int i = 0; i < 3; i++) out[i] = p[k][i].get<float>();
        return true;
    };
    if (method == "camera.release") {
        g_heldCam.on = false;
        *reply = json({{"held", false}, {"override", g_ov.fn ? ModIdOf(g_ov.mod) : "none"}}).dump();
        return true;
    }
    if (method == "camera.set") {
        HeldCam h{};
        if (!v3("eye", h.eye)) { *ok = false; *reply = "camera.set {eye: [x, y, z], at: [x, y, z] | dir: [x, y, z], up, fov (degrees, vertical), ms}"; return true; }
        float at[3];
        if (v3("at", at)) { for (int i = 0; i < 3; i++) h.fwd[i] = at[i] - h.eye[i]; }
        else if (!v3("dir", h.fwd)) { *ok = false; *reply = "camera.set: `at` (a point to look at) or `dir`"; return true; }
        if (!v3("up", h.up)) { h.up[0] = 0; h.up[1] = 1; h.up[2] = 0; }
        h.fovY = p.value("fov", 0.0f);
        h.until = GetTickCount() + static_cast<DWORD>(std::min(std::max(p.value("ms", 10000), 0), 3600000));
        h.on = true;
        float f[3] = {h.fwd[0], h.fwd[1], h.fwd[2]};
        if (!Normalize(f)) { *ok = false; *reply = "camera.set: the eye and the point are the same"; return true; }
        g_heldCam = h;
        *reply = json({{"held", true}, {"ms", static_cast<int>(h.until - GetTickCount())}, {"note", "drawn from the next frame; camera.release gives it back"}}).dump();
        return true;
    }
    json out = {{"override", g_ov.fn ? ModIdOf(g_ov.mod) : (g_heldCam.on ? "the bridge (camera.set)" : "none")}, {"frames_overridden", g_ovFrames},
                {"skipped", g_ovSkipped}, {"declined", g_ovDeclined}, {"last", g_ovLast}};
    float o[3], d[3];
    if (RenderRay(400, 300, o, d)) {
        out["frame_camera"] = {{"eye", {o[0], o[1], o[2]}}, {"fwd", {d[0], d[1], d[2]}}};
    }
    *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
    return true;
}

int ViewAdd(MmlMod *mod, int w, int h, MmlViewFn fn, void *user, std::string *err) {
    if (!fn) { *err = "no function"; return 0; }
    if (w < 0 || h < 0 || w > 8192 || h > 8192 || (!w) != (!h)) { *err = "a size of 1..8192 each, or 0, 0 (the frame's)"; return 0; }
    if (static_cast<int>(g_views.size()) >= kMaxViews) { *err = "eight views already"; return 0; }
    View v{mod, g_nextHandle++, w, h, fn, user};
    g_views.push_back(v);
    return v.handle;
}

void ViewRemove(MmlMod *mod, int handle) {
    if (handle == 0 && g_ov.mod == mod) g_ov = Override{};   // (its unload: its camera override goes)
    for (auto it = g_views.begin(); it != g_views.end();) {
        if (it->mod == mod && (handle == 0 || it->handle == handle)) {
            GfxViewForget(it->handle);
            it = g_views.erase(it);
        } else {
            ++it;
        }
    }
}

void ViewsDraw(void *driver, void *scene, RenderFn render) {
    if (g_views.empty() || !game::g_ls3d || !game::g_exe || g_drawing) return;
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    if (!EnsureHooks()) {
        for (View &v : g_views) { v.skipped++; v.last = "skipped: " + g_hookWhy; }
        return;
    }
    std::vector<int> handles;   // (a view's function may add or take back views)
    for (const View &v : g_views) handles.push_back(v.handle);
    for (int handle : handles) {
        auto it = std::find_if(g_views.begin(), g_views.end(), [&](const View &v) { return v.handle == handle; });
        if (it == g_views.end() || !ModAlive(it->mod)) continue;
        uint8_t *cam = nullptr;
        if (SafeCamera(scene, &cam) || !cam) { it->skipped++; it->last = "skipped: the scene's camera is not LS3D's I3D_camera"; continue; }
        View copy = *it;
        DrawOne(copy, driver, scene, cam, render);
        it = std::find_if(g_views.begin(), g_views.end(), [&](const View &v) { return v.handle == handle; });
        if (it != g_views.end()) *it = copy;
    }
}

bool ViewMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "view.state" && method != "view.show") return false;
    *ok = true;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (method == "view.show") {
        int handle = p.is_object() ? p.value("handle", 0) : 0;
        auto it = std::find_if(g_views.begin(), g_views.end(), [&](const View &v) { return v.handle == handle; });
        if (it == g_views.end()) { *ok = false; *reply = "no such view (view.state lists them)"; return true; }
        float rect[4] = {};
        if (p.contains("rect") && p["rect"].is_array() && p["rect"].size() == 4)
            for (int i = 0; i < 4; i++) rect[i] = p["rect"][i].get<float>();
        GfxViewShow(handle, rect);
        *reply = json({{"handle", handle}, {"rect", {rect[0], rect[1], rect[2], rect[3]}}, {"shown", rect[2] > 0}}).dump();
        return true;
    }
    json views = json::array();
    auto r3 = [](double v) { return std::round(v * 1000) / 1000; };
    for (const View &v : g_views)
        views.push_back({{"mod", ModIdOf(v.mod)}, {"handle", v.handle}, {"size", {v.w, v.h}}, {"drawn", v.drawn}, {"skipped", v.skipped},
                         {"faults", v.faults}, {"last", v.last}, {"cpu_ms", r3(v.ms)},
                         {"eye", {v.eye[0], v.eye[1], v.eye[2]}}, {"fwd", {v.fwd[0], v.fwd[1], v.fwd[2]}}});
    json out = {{"views", views}, {"hooked", g_hooked}};
    if (!game::g_ls3d) out["note"] = "no game known: no view is drawn";
    else if (g_hooksTried && !g_hooked) out["note"] = "no view is drawn: " + g_hookWhy;
    *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
    return true;
}

} // namespace host
