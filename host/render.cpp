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
    if (Subscribed(MML_EVENT_RENDER_BEGIN)) Dispatch(Ev(MML_EVENT_RENDER_BEGIN, dev));
    if (g_world) {
        GfxViewsNewFrame();
        ViewsDraw(driver, scene, oRender);   // (view.cpp: the scene from the mods' cameras, before the frame's own)
        GfxBeforeWorld();   // (gfx.cpp: the readable depth buffer, when a mod has a pass)
    }
    int r = oRender(driver, scene);
    if (g_world) GfxAfterWorld();
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
} // namespace

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
    GfxQuiet(true);                     // (the mods' drawing is not the world's light)
    DispatchDrawing(MML_EVENT_RENDER_WORLD, Device8());
    GfxQuiet(false);
    if (g_world) GfxStage1();           // the graphics bridge: the frame with the mods' drawing in it to the passes
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

bool RenderMethod(const std::string &method, const std::string &, std::string *reply, bool *ok) {
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
    if (!g_hooked) out["note"] = "no game known: nothing is drawn here";
    *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
