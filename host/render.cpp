// The frame's drawing as events for mods (mml.h MML_EVENT_RENDER_*, MML_EVENT_PRESENT, render_device,
// render_camera; docs/MODDING.md "Drawing in the 3D world").
//
// LS3D draws a frame's scene in I3D_driver::Render (game_build.h `render`, stdcall(driver, scene); both builds),
// the mission's world or a menu's scene, and calls the scene's callback (game_build.h `draw2d`, the game's) with
// stage 1 once the 3D world is drawn and with stage 2 for the 2D things (the game draws its HUD, menus and console
// there; at stage 1 it does nothing: 1.2's 0x5BD8D0 tests stage == 2 only). The game's scenes ask for both stages
// (their mask, I3D_scene +0x144 in 1.2, is 6). So: RENDER_BEGIN and RENDER_END around the outermost Render (LS3D
// draws other scenes from inside the world's: those are not told), RENDER_WORLD at its stage 1 (draw.cpp's hook of
// the callback calls RenderStage), PRESENT in the device's Present (screenshot.cpp's hook calls RenderPresent,
// before a screenshot is copied: what mods draw there is in it). The graphics bridge (gfx.cpp) hangs on the same
// points of the world's scene: its depth buffer swapped around Render, its passes at stage 1 after RENDER_WORLD.
//
// While mods draw with the game's device (RENDER_WORLD, PRESENT), the loader keeps the game's state: on Direct3D 9
// (the game on a wrapper) a state block of everything (D3DSBT_ALL) is captured before and applied after, and the
// render target and the depth buffer, which a state block does not hold, are put back. On plain Direct3D 8 nothing
// is kept (a mod puts back what it changes). Nothing is done in a frame no mod subscribed to the event.
//
// Bridge method (game thread): render.state (the hook, frames, the last scene, whether it was the world, stage 1
// seen, the device and its Direct3D 9 interface, the camera).
#include "host.h"
#include <d3d9.h>
#include <cmath>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

RenderFn oRender;   // (host.h)
bool g_hooked;
int g_depth;                    // Render inside Render
void *g_scene;                  // the outermost Render's scene, while it draws
bool g_world;
uint64_t g_renders, g_worldRenders, g_stage1, g_presents;
bool g_stage1ThisRender;
void *g_lastScene;
bool g_lastWorld;

// Direct3D 8 by its vtable (no headers): the slots used here.
enum { kDev8GetTransform = 38, kDev8GetViewport = 41 };
template <class F> F Slot(void *obj, int i) { return reinterpret_cast<F>((*reinterpret_cast<void ***>(obj))[i]); }

void *Device8() {
    if (!game::g_ls3d || !game::Ls3d().d3dDevice) return nullptr;
    void *dev = nullptr;
    MemRead(ModuleBase("LS3DF.dll") + game::Ls3d().d3dDevice, &dev, sizeof dev);
    return dev;
}

// Its Direct3D 9 interface (a wrapper's), looked up once a device.
void *g_dev9For;
IDirect3DDevice9 *g_dev9;
IDirect3DDevice9 *Device9(void *dev8) {
    if (!dev8) return nullptr;
    if (dev8 == g_dev9For) return g_dev9;
    g_dev9For = dev8;
    g_dev9 = nullptr;
    IDirect3DDevice9 *d9 = nullptr;
    auto qi = Slot<HRESULT(__stdcall *)(void *, REFIID, void **)>(dev8, 0);
    if (SUCCEEDED(qi(dev8, __uuidof(IDirect3DDevice9), reinterpret_cast<void **>(&d9))) && d9) {
        d9->Release();   // (the wrapper's device holds it)
        g_dev9 = d9;
    }
    return g_dev9;
}

bool IsWorld(void *scene) {
    if (!game::g_exe || !scene) return false;
    void *mission = nullptr, *world = nullptr;
    if (!MemRead(game::Exe().missionPtr, &mission, sizeof mission) || !mission) return false;
    MemRead(reinterpret_cast<uintptr_t>(mission) + game::kMissionScene, &world, sizeof world);
    return world == scene;
}

// The game's Direct3D 9 state kept around mods' drawing.
IDirect3DStateBlock9 *g_block;
IDirect3DDevice9 *g_blockFor;
struct Kept { IDirect3DSurface9 *rt = nullptr, *ds = nullptr; bool block = false; };
Kept Keep(IDirect3DDevice9 *d) {
    Kept k;
    if (!d) return k;
    if (g_blockFor != d) {
        if (g_block && g_blockFor) g_block->Release();   // (a device made anew: the old block went with the old one)
        g_block = nullptr;
        g_blockFor = d;
        if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &g_block))) g_block = nullptr;
    }
    k.block = g_block && SUCCEEDED(g_block->Capture());
    d->GetRenderTarget(0, &k.rt);
    d->GetDepthStencilSurface(&k.ds);
    return k;
}
void PutBack(IDirect3DDevice9 *d, Kept &k) {
    if (!d) return;
    if (k.block) g_block->Apply();
    if (k.rt) { d->SetRenderTarget(0, k.rt); k.rt->Release(); }
    for (DWORD i = 1; i < 4; i++) d->SetRenderTarget(i, nullptr);
    d->SetDepthStencilSurface(k.ds);
    if (k.ds) k.ds->Release();
}

MmlEvent Ev(MmlEventType type, void *dev) {
    MmlEvent ev{};
    ev.size = sizeof ev;
    ev.type = type;
    ev.scene = g_scene;
    ev.world = g_world ? 1 : 0;
    ev.device = dev;
    return ev;
}

// A drawing event, the device's state kept around it.
void DispatchDrawing(MmlEventType type, void *dev8) {
    if (!Subscribed(type)) return;
    IDirect3DDevice9 *d9 = Device9(dev8);
    Kept k = Keep(d9);
    Dispatch(Ev(type, dev8));
    PutBack(d9, k);
}

int __stdcall hkRender(void *driver, void *scene) {
    if (++g_depth != 1) {
        int r = oRender(driver, scene);
        g_depth--;
        return r;
    }
    g_scene = scene;
    g_world = IsWorld(scene);
    g_stage1ThisRender = false;
    g_renders++;
    if (g_world) g_worldRenders++;
    void *dev = Device8();
    if (dev) ScreenshotHookPresent(nullptr);   // (PRESENT: the device's Present hooked once it exists)
    GfxFrameBegin();   // (gfx.cpp: the frame's GPU time, to its Present)
    if (g_world) HumansBeforeRender();   // (humans.cpp: the poses the bridge holds; the mods' bone_set come after)
    if (Subscribed(MML_EVENT_RENDER_BEGIN)) Dispatch(Ev(MML_EVENT_RENDER_BEGIN, dev));
    if (g_world) HumansFlushPoses();
    if (g_world) {
        SkinFrameBegin();   // (skin.cpp: the frame's count, for the palettes drawn before)
        GeomFrameBegin();   // (geom.cpp: the game's geometry hidden, surveyed, captured)
        GameCameraFrame();   // (gamecam.cpp: a mod's field of view held on the game's camera)
        FrameCameraBegin(scene);   // (view.cpp: the frame drawn from a mod's camera; the views start from it)
        ScaleBeforeRender();   // (scale.cpp: the actors drawn at the mods' sizes: the shadows and the views too)
        ShadowsBeforeWorld(driver, scene, oRender);   // (shadows.cpp: the map from the sun, LS3D drawing it, before the views)
        GfxViewsNewFrame();
        {
            ProfScope ps(nullptr, "scene views");
            ProfPhaseScope ph(2);
            ViewsDraw(driver, scene, oRender);   // (view.cpp: the scene from the mods' cameras, before the frame's own)
        }
        GfxBeforeWorld();   // (gfx.cpp: the readable depth buffer, when a mod has a pass)
        GeomFrameDrawing(true);
    }
    int r;
    {
        ProfPhaseScope ph(1);
        r = oRender(driver, scene);
    }
    if (g_world) {
        ScaleAfterRender();   // (their own sizes back before anything of the game's reads them)
        FrameCameraEnd();   // (the game's camera back)
        GeomFrameDrawing(false);
        GfxAfterWorld();
        GeomFrameEnd();
        ShadersFrameEnd();   // (shaders.cpp: the survey done)
    }
    if (Subscribed(MML_EVENT_RENDER_END)) Dispatch(Ev(MML_EVENT_RENDER_END, dev));
    g_lastScene = scene;
    g_lastWorld = g_world;
    g_scene = nullptr;
    g_depth--;
    return r;
}

bool Camera(void *dev8, MmlRenderCamera *c) {
    if (!dev8) return false;
    D3DMATRIX v{}, p{};
    D3DVIEWPORT9 vp{};   // (D3DVIEWPORT8: the same layout)
    auto getT = Slot<HRESULT(__stdcall *)(void *, D3DTRANSFORMSTATETYPE, D3DMATRIX *)>(dev8, kDev8GetTransform);
    auto getV = Slot<HRESULT(__stdcall *)(void *, D3DVIEWPORT9 *)>(dev8, kDev8GetViewport);
    if (FAILED(getT(dev8, D3DTS_VIEW, &v)) || FAILED(getT(dev8, D3DTS_PROJECTION, &p))) return false;
    getV(dev8, &vp);
    uint32_t size = c->size;
    *c = MmlRenderCamera{};
    c->size = size;
    memcpy(c->view, &v, sizeof c->view);
    memcpy(c->proj, &p, sizeof c->proj);
    c->right = {v._11, v._21, v._31};
    c->up = {v._12, v._22, v._32};
    c->fwd = {v._13, v._23, v._33};
    // the view's translation is -eye in the camera's axes: eye = -(t.x right + t.y up + t.z fwd)
    c->eye = {-(v._41 * c->right.x + v._42 * c->up.x + v._43 * c->fwd.x),
              -(v._41 * c->right.y + v._42 * c->up.y + v._43 * c->fwd.y),
              -(v._41 * c->right.z + v._42 * c->up.z + v._43 * c->fwd.z)};
    if (p._11 != 0) c->fov_x = 2.0f * std::atan(1.0f / p._11);
    if (p._22 != 0) c->fov_y = 2.0f * std::atan(1.0f / p._22);
    if (p._33 != 0) c->near_z = -p._43 / p._33;
    if (p._33 != 1) c->far_z = p._43 / (1.0f - p._33);
    c->width = static_cast<int>(vp.Width);
    c->height = static_cast<int>(vp.Height);
    return true;
}
DWORD SafeCamera(void *dev8, MmlRenderCamera *c, bool *ok) {
    __try { *ok = Camera(dev8, c); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { *ok = false; return GetExceptionCode(); }
}

// The camera the last frame of the mission's world was drawn with (taken at its stage 1): camera_project, camera_ray.
MmlRenderCamera g_frameCamera{sizeof(MmlRenderCamera)};
bool g_haveFrameCamera;
} // namespace

int RenderProject(const float w[3], float out[3]) {
    if (!g_haveFrameCamera) return 0;
    const MmlRenderCamera &c = g_frameCamera;
    float d[3] = {w[0] - c.eye.x, w[1] - c.eye.y, w[2] - c.eye.z};
    float z = d[0] * c.fwd.x + d[1] * c.fwd.y + d[2] * c.fwd.z;
    if (!(z > c.near_z * 0.5f)) return 0;   // behind the camera
    float x = d[0] * c.right.x + d[1] * c.right.y + d[2] * c.right.z, y = d[0] * c.up.x + d[1] * c.up.y + d[2] * c.up.z;
    // NDC from the projection (row vectors): x' = x _11 + z _31, w' = z
    float nx = (x * c.proj[0] + z * c.proj[8]) / z, ny = (y * c.proj[5] + z * c.proj[9]) / z;
    out[0] = (nx + 1) * 400.0f;
    out[1] = (1 - ny) * 300.0f;
    out[2] = z;
    return 1;
}

bool RenderFrameCamera(MmlRenderCamera *out) {
    if (!g_haveFrameCamera) return false;
    *out = g_frameCamera;
    return true;
}

int RenderRay(float x, float y, float origin[3], float dir[3]) {
    if (!g_haveFrameCamera) return 0;
    const MmlRenderCamera &c = g_frameCamera;
    float nx = x / 400.0f - 1, ny = 1 - y / 300.0f;
    float vx = (nx - c.proj[8]) / c.proj[0], vy = (ny - c.proj[9]) / c.proj[5];
    float d[3] = {c.right.x * vx + c.up.x * vy + c.fwd.x, c.right.y * vx + c.up.y * vy + c.fwd.y, c.right.z * vx + c.up.z * vy + c.fwd.z};
    float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(l > 0)) return 0;
    origin[0] = c.eye.x; origin[1] = c.eye.y; origin[2] = c.eye.z;
    dir[0] = d[0] / l; dir[1] = d[1] / l; dir[2] = d[2] / l;
    return 1;
}

void RenderInit() {
    if (!game::g_ls3d || !game::Ls3d().render) return;
    std::string err;
    uintptr_t at = ModuleBase("LS3DF.dll") + game::Ls3d().render;
    g_hooked = HookAdd(nullptr, at, reinterpret_cast<void *>(&hkRender), reinterpret_cast<void **>(&oRender), &err);
    if (!g_hooked) HWARN("render: I3D_driver::Render not hooked: %s", err.c_str());
}

void RenderStage(int stage) {
    if (stage != 1 || g_depth != 1 || g_stage1ThisRender) return;   // the outermost scene's, once
    g_stage1ThisRender = true;
    g_stage1++;
    if (g_world) GfxTakeWorldLight();   // the world's fog and sun: what was drawn up to here
    if (g_world) { bool ok = false; SafeCamera(Device8(), &g_frameCamera, &ok); g_haveFrameCamera = g_haveFrameCamera || ok; }
    GfxQuiet(true);                     // (the mods' drawing is not the world's light)
    DispatchDrawing(MML_EVENT_RENDER_WORLD, Device8());
    if (g_world && DrawWorldWanted()) {   // (worlddraw.cpp: the mods' and the bridge's shapes, after the mods' drawing)
        IDirect3DDevice9 *d9 = Device9(Device8());
        Kept k = Keep(d9);
        DrawWorldNow(d9);
        PutBack(d9, k);
    }
    GfxQuiet(false);
    if (g_world) {                      // the graphics bridge: the frame with the mods' drawing in it to the passes
        ProfScope ps(nullptr, "graphics bridge");
        ProfPhaseScope ph(3);
        GfxStage1();
    }
    if (g_world) GfxViewsShowNow();     // view.show: views copied onto the frame
}

void RenderViewWorld(int handle) {
    if (!Subscribed(MML_EVENT_VIEW_WORLD)) return;
    void *dev8 = Device8();
    IDirect3DDevice9 *d9 = Device9(dev8);
    Kept k = Keep(d9);
    MmlEvent ev = Ev(MML_EVENT_VIEW_WORLD, dev8);
    ev.view = handle;
    GfxQuiet(true);
    Dispatch(ev);
    GfxQuiet(false);
    PutBack(d9, k);
}

void RenderPresent(void *dev8) {
    g_presents++;
    DispatchDrawing(MML_EVENT_PRESENT, dev8);
}

void *RenderDevice(int version) {
    void *dev = Device8();
    if (version == 8) return dev;
    if (version == 9) return Device9(dev);
    return nullptr;
}

int RenderCamera(MmlRenderCamera *out) {
    if (!out) return 0;
    bool ok = false;
    SafeCamera(Device8(), out, &ok);
    return ok ? 1 : 0;
}

// ---- LS3D's driver states a measure changes (render.set): I3D_driver's SetState and GetState (its vtable +0x14,
// +0x18, stdcall(driver, id[, value])); RS_USE_OCCLUSION (39: the occluders hide what is behind them, a sector's
// visuals tested against them every frame) and RS_LOD_QUALITY (20: a float, 1 by default, the levels of detail's
// distance scale). Game thread.
namespace {
constexpr int kRsLodQuality = 20, kRsOcclusion = 39;
using DrvSetFn = int(__stdcall *)(void *self, int id, int value);
using DrvGetFn = int(__stdcall *)(void *self, int id);
void *Driver() {
    if (!game::g_exe || !game::Exe().i3dDriver) return nullptr;
    void *d = nullptr;
    __try { d = *reinterpret_cast<void **>(game::Exe().i3dDriver); } __except (EXCEPTION_EXECUTE_HANDLER) { d = nullptr; }
    return d;
}
int DrvGet(void *d, int id) { return (*reinterpret_cast<DrvGetFn **>(d))[0x18 / 4](d, id); }
void DrvSet(void *d, int id, int v) { (*reinterpret_cast<DrvSetFn **>(d))[0x14 / 4](d, id, v); }
float AsFloat(int v) { float f; memcpy(&f, &v, 4); return f; }
int AsInt(float f) { int v; memcpy(&v, &f, 4); return v; }

// ---- The whole city (render.set's whole_city), everything the game would leave out of a far view put in:
// - the city's buildings (cache.bin's blocks), streamed by the game within 200 m of the camera (game_build.h
//   cityCache): held, CacheTick does not run; in its place every block not active is made so by the game's own
//   ActivateBlock and their fade-ins ended at once (they are timed by the game's time, which a measure may hold).
//   After a mission's load the new cache's blocks are made active at its first tick. Given back, the game's tick
//   puts the far blocks away itself at its next turn;
// - the occluders (RS_USE_OCCLUSION off: what they hide is tested by the frustum alone; their state given back);
// - the draw distance of each visual, its last LOD's (hkGetLod below);
// - the game camera's far plane (HoldFar below; camera.set's camera has its own `far`).
constexpr int kBlockSize = 0x74;
using CacheTickFn = char(__fastcall *)(void *cache, void *edx, int dtMs);
using ActivateFn = void(__thiscall *)(void *cache, int block);
using FinishFadesFn = void(__thiscall *)(void *mission);
CacheTickFn oCacheTick;
bool g_wholeCity, g_cacheHooked;
int g_occlusionBefore = 1;
uint64_t g_blocksActivated;

struct Blocks { uint8_t *begin; int count; };
Blocks CityBlocks() {
    Blocks b{};
    if (!game::g_exe || !game::Exe().cityCache) return b;
    __try {
        uint8_t *cache = reinterpret_cast<uint8_t *>(game::Exe().cityCache);
        if (!cache[0x24]) return b;   // (no city cache in this mission)
        uint8_t *begin = *reinterpret_cast<uint8_t **>(cache + 4), *end = *reinterpret_cast<uint8_t **>(cache + 8);
        if (begin && end > begin) b = {begin, static_cast<int>((end - begin) / kBlockSize)};
    } __except (EXCEPTION_EXECUTE_HANDLER) { b = {}; }
    return b;
}
int ActiveBlocks(const Blocks &b) {
    int n = 0;
    for (int i = 0; i < b.count; i++) n += b.begin[i * kBlockSize + 4] != 0;
    return n;
}
// Every block active, their fade-ins ended: the count made active now.
int ActivateAll() {
    const Blocks b = CityBlocks();
    const game::ExeBuild &e = game::Exe();
    int made = 0;
    for (int i = 0; i < b.count; i++)
        if (!b.begin[i * kBlockSize + 4]) { reinterpret_cast<ActivateFn>(e.cacheActivate)(reinterpret_cast<void *>(e.cityCache), i); made++; }
    if (made) {
        void *mission = *reinterpret_cast<void **>(e.missionPtr);
        if (mission) reinterpret_cast<FinishFadesFn>(e.missionFinishFades)(mission);
        g_blocksActivated += made;
    }
    return made;
}
// The game camera's far plane (the mission's scene's camera, I3D_scene +0x17C: the one CacheTick streams around),
// set by the game once at a mission's load (600 m in the city): held at g_cityFar by its own SetRange (I3D_camera's
// vtable +0x58), the game's put back after. A far plane the game sets meanwhile (a mission's load) is the one put back.
constexpr int kCamNearZ = 0x144, kCamFarZ = 0x148, kCamSetRange = 0x58 / 4;
using CamSetRangeFn = int(__stdcall *)(void *cam, float lo, float hi);
float g_cityFar = 5000.0f, g_gameFar;
void *SceneCamera() {
    void *mission = *reinterpret_cast<void **>(game::Exe().missionPtr);
    void *scene = mission ? game::at<void *>(mission, 0x10) : nullptr;
    return scene ? game::at<void *>(scene, 0x17C) : nullptr;
}
void HoldFar(bool hold) {
    void *cam = SceneCamera();
    if (!cam) return;
    const float nearZ = game::at<float>(cam, kCamNearZ), farZ = game::at<float>(cam, kCamFarZ);
    auto setRange = (*reinterpret_cast<CamSetRangeFn **>(cam))[kCamSetRange];
    if (hold && farZ != g_cityFar) { g_gameFar = farZ; setRange(cam, nearZ, g_cityFar); }
    else if (!hold && farZ == g_cityFar && g_gameFar > nearZ) setRange(cam, nearZ, g_gameFar);
}
char __fastcall hkCacheTick(void *cache, void *edx, int dtMs) {
    if (!g_wholeCity) return oCacheTick(cache, edx, dtMs);
    ActivateAll();
    HoldFar(true);
    return 0;
}
// No visual left out by its distance: past the start of its last LOD's fade (0.5 * the last LOD's distance + the LOD
// before's, or the last LOD's own when that is nearer: game_build.h getObjectLod) the caller's squared distance is
// made that start, so the game's own code picks its last LOD and draws it whole (alpha 1); the levels of detail
// nearer are the game's. The caller's variable is written: m_AddFGroups reads its fade from it after.
using GetLodFn = int(__fastcall *)(void *visual, float *dist2);
GetLodFn oGetLod;
bool g_lodHooked;
const float *g_lodQuality;   // LS3D's RSLodQuality
uint64_t g_lodKept;   // visuals drawn past their last LOD's distance (or its fade), all frames
int __fastcall hkGetLod(void *visual, float *dist2) {
    if (g_wholeCity) {
        const uint8_t *mesh = game::at<uint8_t *>(visual, 0x1E0);
        const int n = mesh ? *reinterpret_cast<const int *>(mesh + 8) : 0;
        const uint8_t *last = n > 0 ? *reinterpret_cast<uint8_t *const *>(mesh + 0xC + (n - 1) * 4) : nullptr;
        const float far2 = last ? *reinterpret_cast<const float *>(last + 0x38) : 0.0f;
        if (far2 > 0) {
            float fade = 0.5f * far2;
            if (n > 1)
                if (const uint8_t *before = *reinterpret_cast<uint8_t *const *>(mesh + 0xC + (n - 2) * 4))
                    fade += *reinterpret_cast<const float *>(before + 0x38);
            const float q = *g_lodQuality * game::at<float>(visual, 0x114);
            const float limit = std::min(fade, far2) * 0.999f * q * q;
            if (*dist2 > limit && limit > 0) { *dist2 = limit; g_lodKept++; }
        }
    }
    return oGetLod(visual, dist2);
}
json CityJson() {
    const Blocks b = CityBlocks();
    json j = {{"whole", g_wholeCity}, {"blocks", b.count}, {"active", ActiveBlocks(b)}, {"activated", g_blocksActivated},
              {"lods_kept", g_lodKept}, {"far", g_cityFar}};
    if (g_gameFar > 0) j["game_far"] = g_gameFar;
    return j;
}
} // namespace

bool RenderMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method == "render.set") {
        json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
        if (!p.is_object()) p = json::object();
        void *d = Driver();
        if (!d) { *ok = false; *reply = "render.set: no LS3D driver (no game known, or not made yet)"; return true; }
        if (p.contains("occlusion")) DrvSet(d, kRsOcclusion, p["occlusion"].get<bool>() ? 1 : 0);
        if (p.contains("lod_quality")) {
            const float q = p["lod_quality"].get<float>();
            if (!(q > 0.001f && q < 1000.0f)) { *ok = false; *reply = "render.set: lod_quality 0.001..1000 (1 the game's)"; return true; }
            DrvSet(d, kRsLodQuality, AsInt(q));
        }
        if (p.contains("batch")) {
            std::string err;
            if (!BatchSet(p["batch"].get<bool>(), &err)) { *ok = false; *reply = "render.set: batch: " + err; return true; }
        }
        if (p.contains("whole_city")) {
            const game::ExeBuild &e = game::Exe();
            const game::Ls3dBuild &l = game::Ls3d();
            if (!e.cacheTick || !l.getObjectLod) { *ok = false; *reply = "render.set: whole_city: the city's streaming is not known in this build"; return true; }
            std::string err;
            if (!g_cacheHooked) {
                if (!HookAdd(nullptr, e.cacheTick, reinterpret_cast<void *>(&hkCacheTick), reinterpret_cast<void **>(&oCacheTick), &err)) {
                    *ok = false; *reply = "render.set: whole_city: CacheTick not hooked (" + err + ")"; return true;
                }
                g_cacheHooked = true;
            }
            if (!g_lodHooked) {
                const uintptr_t ls = ModuleBase("LS3DF.dll");
                g_lodQuality = reinterpret_cast<const float *>(ls + l.rsLodQuality);
                if (!HookAdd(nullptr, ls + l.getObjectLod, reinterpret_cast<void *>(&hkGetLod), reinterpret_cast<void **>(&oGetLod), &err)) {
                    *ok = false; *reply = "render.set: whole_city: GetObjectLOD not hooked (" + err + ")"; return true;
                }
                g_lodHooked = true;
            }
            const bool on = p["whole_city"].get<bool>();
            if (p.contains("far")) {
                const float f = p["far"].get<float>();
                if (!(f >= 100.0f && f <= 100000.0f)) { *ok = false; *reply = "render.set: far 100..100000 (metres)"; return true; }
                if (g_wholeCity) HoldFar(false);   // (the game's put back first: the new one held from it)
                g_cityFar = f;
            }
            if (on && !g_wholeCity) g_occlusionBefore = DrvGet(d, kRsOcclusion);   // (the occluders' state given back after)
            if (on != g_wholeCity && !p.contains("occlusion")) DrvSet(d, kRsOcclusion, on ? 0 : g_occlusionBefore);
            g_wholeCity = on;
            HoldFar(on);
            if (on) ActivateAll();
            HLOG("render: the whole city %s", on ? "held (every block of the city's cache active, no occluders, no visual left out by its distance)"
                                                 : "given back to the game (its streaming, its occluders, its distances)");
        }
        *reply = json({{"occlusion", DrvGet(d, kRsOcclusion) != 0}, {"lod_quality", AsFloat(DrvGet(d, kRsLodQuality))},
                       {"city", CityJson()}, {"batch", json::parse(BatchStateJson())}})
                     .dump(-1, ' ', false, json::error_handler_t::replace);
        *ok = true;
        return true;
    }
    if (method == "render.batch_probe") {
        json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
        float at[3] = {0, 0, 0};
        if (p.is_object() && p.contains("at")) for (int i = 0; i < 3; i++) at[i] = p["at"][i].get<float>();
        *reply = BatchProbeJson(at, p.is_object() ? p.value("radius", 30.0f) : 30.0f);
        *ok = true;
        return true;
    }
    if (method != "render.state") return false;
    void *dev = Device8();
    json out = {{"hooked", g_hooked}, {"renders", g_renders}, {"world_renders", g_worldRenders}, {"stage1", g_stage1},
                {"presents", g_presents}, {"last_scene_world", g_lastWorld}};
    char b[32];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_lastScene)));
    out["last_scene"] = b;
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(dev)));
    out["device8"] = dev ? json(b) : json(nullptr);
    out["direct3d9"] = Device9(dev) != nullptr;
    MmlRenderCamera c{};
    c.size = sizeof c;
    if (RenderCamera(&c))
        out["camera"] = {{"eye", {c.eye.x, c.eye.y, c.eye.z}}, {"fwd", {c.fwd.x, c.fwd.y, c.fwd.z}},
                         {"fov_y_deg", c.fov_y * 57.29578f}, {"near", c.near_z}, {"far", c.far_z},
                         {"viewport", {c.width, c.height}}};
    if (void *d = Driver()) out["ls3d"] = {{"occlusion", DrvGet(d, kRsOcclusion) != 0}, {"lod_quality", AsFloat(DrvGet(d, kRsLodQuality))}};
    if (game::g_exe && game::Exe().cityCache) out["city"] = CityJson();
    out["batch"] = json::parse(BatchStateJson());
    if (!g_hooked) out["note"] = "no game known: nothing is drawn here";
    *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
