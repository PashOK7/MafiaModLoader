// Screenshots of the game's own frame: the back buffer, copied when the frame is complete (in the device's Present,
// before the frame is shown), written raw to <game>\mml\shots\<n>.bgra; the MCP server makes the PNG.
//
// The device is the IDirect3DDevice8 LS3D renders with (game_build.h `d3dDevice`). With a wrapper that runs the game
// on Direct3D 9 (dxwrapper's d3d8to9, as in the lab), the device answers QueryInterface(IDirect3DDevice9): the back
// buffer is then resolved (it may be multisampled) with StretchRect into a plain render target and read back
// (mafia-ragdoll's `gfx grab` does the same). On plain Direct3D 8, CopyRects into an image surface (a multisampled
// back buffer cannot be read there). Present is hooked at the first request (the function the device's vtable
// points at) and stays hooked; it costs nothing while no shot is asked for.
//
// The file: a 16-byte header {u32 magic 'MMLS', u32 width, u32 height, u32 0}, then width*height BGRA pixels, top
// row first (alpha 255).
//
// Bridge methods (game thread): screenshot (asks for the next frame presented; returns the shot's id),
// screenshot.state {id} (done, ok or the error, the file, the size).
#include "host.h"
#include <d3d9.h>
#include <atomic>
#include <cstdio>
#include <mutex>
#include "json.hpp"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

// Direct3D 8 by its vtables (no headers): the slots used here.
enum { kDevPresent = 15, kDevGetBackBuffer = 16, kDevCreateImageSurface = 27, kDevCopyRects = 28 };
enum { kUnkRelease = 2, kSurfGetDesc = 8, kSurfLockRect = 9, kSurfUnlockRect = 10 };
struct SurfaceDesc { uint32_t format, type, usage, pool, size, multiSample, width, height; };   // D3DSURFACE_DESC (8)
struct LockedRect { int pitch; void *bits; };
enum { kFmtA8R8G8B8 = 21, kFmtX8R8G8B8 = 22, kFmtR5G6B5 = 23, kFmtX1R5G5B5 = 24, kFmtA1R5G5B5 = 25 };

template <class F> F Slot(void *obj, int i) { return reinterpret_cast<F>((*reinterpret_cast<void ***>(obj))[i]); }
ULONG Release(void *o) { return Slot<ULONG(__stdcall *)(void *)>(o, kUnkRelease)(o); }

using PresentFn = HRESULT(__stdcall *)(void *dev, const RECT *src, const RECT *dst, HWND wnd, const void *dirty);
PresentFn g_present;
std::atomic<bool> g_hooked;
std::mutex g_mx;
int g_nextId = 1, g_wantId = 0;   // the shot asked for (0: none)
struct Result { int id; bool ok; std::string path, error; int width, height; };
std::vector<Result> g_results;    // the last few

std::string WriteShot(int id, uint32_t w, uint32_t h, const uint8_t *bits, int pitch) {
    std::string dir = MmlDir() + "shots\\";
    MakeDir(dir);
    std::string path = dir + std::to_string(id) + ".bgra";
    FILE *f = FileOpen(path, "wb");
    if (!f) return "cannot write " + path;
    uint32_t head[4] = {0x534C4D4D, w, h, 0};
    fwrite(head, 4, 4, f);
    std::vector<uint32_t> row(w);
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *src = reinterpret_cast<const uint32_t *>(bits + static_cast<size_t>(pitch) * y);
        for (uint32_t x = 0; x < w; x++) row[x] = src[x] | 0xFF000000u;
        fwrite(row.data(), 4, w, f);
    }
    fclose(f);
    return "";
}

// IID_IDirect3DDevice9 {D0223B96-BF7A-43FD-92BD-A43B0D82B9EB}
const GUID kIidDevice9 = {0xD0223B96, 0xBF7A, 0x43FD, {0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB}};

// Through Direct3D 9 under a wrapper; false: the device is not Direct3D 9 underneath.
bool Capture9(void *dev8, int id, int *w, int *h, std::string *err) {
    IDirect3DDevice9 *d9 = nullptr;
    if (FAILED(static_cast<IUnknown *>(dev8)->QueryInterface(kIidDevice9, reinterpret_cast<void **>(&d9))) || !d9) return false;
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sys = nullptr;
    D3DSURFACE_DESC d{};
    HRESULT hr = d9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (SUCCEEDED(hr)) {
        bb->GetDesc(&d);
        *w = static_cast<int>(d.Width);
        *h = static_cast<int>(d.Height);
        hr = d9->CreateRenderTarget(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
    }
    if (SUCCEEDED(hr)) hr = d9->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE);
    if (SUCCEEDED(hr)) hr = d9->CreateOffscreenPlainSurface(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = d9->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(hr)) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (SUCCEEDED(hr)) {
        *err = WriteShot(id, d.Width, d.Height, static_cast<const uint8_t *>(lr.pBits), lr.Pitch);
        sys->UnlockRect();
    } else {
        char b[128];
        snprintf(b, sizeof b, "Direct3D 9 copy failed (0x%08X; back buffer format %d, multisample %d)", static_cast<unsigned>(hr),
                 static_cast<int>(d.Format), static_cast<int>(d.MultiSampleType));
        *err = b;
    }
    if (sys) sys->Release();
    if (rt) rt->Release();
    if (bb) bb->Release();
    d9->Release();
    return true;
}

// Plain Direct3D 8 with a multisampled back buffer: nothing can read that buffer, so the front buffer is taken
// (GetFrontBuffer: what the screen shows, desktop-sized in a window) and the game window's client area cut out of
// it. It is the frame shown before this one, and only what is visible on the screen: a window over the game's is in
// the picture.
enum { kDevGetCreationParameters = 9, kDevGetDisplayMode = 8, kDevGetFrontBuffer = 30 };
std::string CaptureFront(void *dev, int id, int *w, int *h) {
    struct DisplayMode { UINT width, height, refresh; uint32_t format; } mode{};
    struct CreationParams { UINT adapter; int type; HWND focus; DWORD flags; } cp{};
    if (FAILED(Slot<HRESULT(__stdcall *)(void *, DisplayMode *)>(dev, kDevGetDisplayMode)(dev, &mode)) || !mode.width)
        return "GetDisplayMode failed";
    Slot<HRESULT(__stdcall *)(void *, CreationParams *)>(dev, kDevGetCreationParameters)(dev, &cp);
    // The front buffer is the screen: only taken while the game's window is the one in front, else it would hold
    // whatever else the owner has open over it (seen once: a chat window instead of the game).
    HWND fg = GetForegroundWindow();
    if (!cp.focus || !fg || GetAncestor(fg, GA_ROOT) != GetAncestor(cp.focus, GA_ROOT) || IsIconic(cp.focus))
        return "the game's window is not in front: on plain Direct3D 8 with a multisampled back buffer the loader can only "
               "copy the screen, and the screen shows other windows now (bring the game to the front, or turn its "
               "antialiasing off, or run it through a Direct3D 9 wrapper)";
    void *img = nullptr;
    if (FAILED(Slot<HRESULT(__stdcall *)(void *, UINT, UINT, uint32_t, void **)>(dev, kDevCreateImageSurface)(dev, mode.width, mode.height, kFmtA8R8G8B8, &img)) || !img)
        return "CreateImageSurface for the front buffer failed";
    std::string err;
    if (FAILED(Slot<HRESULT(__stdcall *)(void *, void *)>(dev, kDevGetFrontBuffer)(dev, img))) {
        err = "GetFrontBuffer failed";
    } else {
        RECT r{0, 0, static_cast<LONG>(mode.width), static_cast<LONG>(mode.height)};
        if (cp.focus && GetClientRect(cp.focus, &r)) {   // windowed: the client area on the screen
            POINT tl{r.left, r.top}, br{r.right, r.bottom};
            ClientToScreen(cp.focus, &tl);
            ClientToScreen(cp.focus, &br);
            r = {std::max<LONG>(tl.x, 0), std::max<LONG>(tl.y, 0), std::min<LONG>(br.x, static_cast<LONG>(mode.width)), std::min<LONG>(br.y, static_cast<LONG>(mode.height))};
        }
        LockedRect lr{};
        if (r.right <= r.left || r.bottom <= r.top) err = "the game's window is not on the screen";
        else if (FAILED(Slot<HRESULT(__stdcall *)(void *, LockedRect *, const RECT *, DWORD)>(img, kSurfLockRect)(img, &lr, nullptr, 0x10)))
            err = "LockRect failed";
        else {
            *w = r.right - r.left;
            *h = r.bottom - r.top;
            std::vector<uint32_t> px(static_cast<size_t>(*w) * *h);
            for (int y = 0; y < *h; y++) {
                const uint32_t *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(lr.bits) + static_cast<size_t>(lr.pitch) * (r.top + y));
                for (int x = 0; x < *w; x++) px[static_cast<size_t>(y) * *w + x] = row[r.left + x] | 0xFF000000u;
            }
            Slot<HRESULT(__stdcall *)(void *)>(img, kSurfUnlockRect)(img);
            err = WriteShot(id, static_cast<uint32_t>(*w), static_cast<uint32_t>(*h), reinterpret_cast<const uint8_t *>(px.data()), *w * 4);
        }
    }
    Release(img);
    return err;
}

std::string Capture(void *dev, int id, int *w, int *h) {
    std::string err9;
    if (Capture9(dev, id, w, h, &err9)) return err9;
    void *bb = nullptr, *img = nullptr;
    std::string err;
    if (FAILED(Slot<HRESULT(__stdcall *)(void *, UINT, int, void **)>(dev, kDevGetBackBuffer)(dev, 0, 0, &bb)) || !bb)
        return "GetBackBuffer failed";
    SurfaceDesc d{};
    Slot<HRESULT(__stdcall *)(void *, SurfaceDesc *)>(bb, kSurfGetDesc)(bb, &d);
    *w = static_cast<int>(d.width);
    *h = static_cast<int>(d.height);
    if (d.multiSample) {
        Release(bb);
        return CaptureFront(dev, id, w, h);
    } else if (FAILED(Slot<HRESULT(__stdcall *)(void *, UINT, UINT, uint32_t, void **)>(dev, kDevCreateImageSurface)(dev, d.width, d.height, d.format, &img)) || !img)
        err = "CreateImageSurface failed (format " + std::to_string(d.format) + ")";
    else if (FAILED(Slot<HRESULT(__stdcall *)(void *, void *, const RECT *, UINT, void *, const POINT *)>(dev, kDevCopyRects)(dev, bb, nullptr, 0, img, nullptr)))
        err = "CopyRects failed";
    else {
        LockedRect lr{};
        if (FAILED(Slot<HRESULT(__stdcall *)(void *, LockedRect *, const RECT *, DWORD)>(img, kSurfLockRect)(img, &lr, nullptr, 0x10 /* READONLY */)))
            err = "LockRect failed";
        else {
            std::vector<uint32_t> px(static_cast<size_t>(d.width) * d.height);
            for (uint32_t y = 0; y < d.height; y++) {
                const uint8_t *row = static_cast<const uint8_t *>(lr.bits) + static_cast<size_t>(lr.pitch) * y;
                uint32_t *out = px.data() + static_cast<size_t>(y) * d.width;
                for (uint32_t x = 0; x < d.width; x++) {
                    uint32_t c;
                    if (d.format == kFmtA8R8G8B8 || d.format == kFmtX8R8G8B8) c = reinterpret_cast<const uint32_t *>(row)[x];
                    else {
                        uint16_t s = reinterpret_cast<const uint16_t *>(row)[x];
                        if (d.format == kFmtR5G6B5) c = ((s >> 11) * 255 / 31) << 16 | (((s >> 5) & 63) * 255 / 63) << 8 | (s & 31) * 255 / 31;
                        else c = (((s >> 10) & 31) * 255 / 31) << 16 | (((s >> 5) & 31) * 255 / 31) << 8 | (s & 31) * 255 / 31;
                    }
                    out[x] = c | 0xFF000000u;
                }
            }
            Slot<HRESULT(__stdcall *)(void *)>(img, kSurfUnlockRect)(img);
            if (d.format != kFmtA8R8G8B8 && d.format != kFmtX8R8G8B8 && d.format != kFmtR5G6B5 && d.format != kFmtX1R5G5B5 && d.format != kFmtA1R5G5B5)
                err = "back buffer format " + std::to_string(d.format) + " not read";
            else
                err = WriteShot(id, d.width, d.height, reinterpret_cast<const uint8_t *>(px.data()), static_cast<int>(d.width * 4));
        }
    }
    if (img) Release(img);
    Release(bb);
    return err;
}

void CaptureTo(void *dev, int id, int *w, int *h, std::string *err) { *err = Capture(dev, id, w, h); }

DWORD SafeCapture(void *dev, int id, int *w, int *h, std::string *err) {
    __try { CaptureTo(dev, id, w, h, err); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

HRESULT __stdcall PresentHook(void *dev, const RECT *src, const RECT *dst, HWND wnd, const void *dirty) {
    RenderPresent(dev);   // (mods' drawing over the whole frame first: it is in the shot)
    int id = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        id = g_wantId;
        g_wantId = 0;
    }
    if (id) {
        int w = 0, h = 0;
        std::string err;
        if (DWORD ex = SafeCapture(dev, id, &w, &h, &err)) err = "a fault while copying the frame (exception " + std::to_string(ex) + ")";
        std::lock_guard<std::mutex> lk(g_mx);
        g_results.push_back({id, err.empty(), MmlDir() + "shots\\" + std::to_string(id) + ".bgra", err, w, h});
        if (g_results.size() > 16) g_results.erase(g_results.begin());
    }
    ProfPresentBegin();   // (the frame's record: its end, the time Present blocks)
    HRESULT hr = g_present(dev, src, dst, wnd, dirty);
    ProfPresentEnd(hr);
    return hr;
}

bool EnsureHooked(std::string *err) {
    if (g_hooked) return true;
    if (!game::g_ls3d || !game::Ls3d().d3dDevice) { *err = "the device is not known for this build"; return false; }
    void *dev = nullptr;
    MemRead(ModuleBase("LS3DF.dll") + game::Ls3d().d3dDevice, &dev, 4);
    if (!dev) { *err = "no Direct3D device yet"; return false; }
    void *present = nullptr;
    if (!MemRead(reinterpret_cast<uintptr_t>(*reinterpret_cast<void ***>(dev) + kDevPresent), &present, 4) || !present) {
        *err = "cannot read the device's vtable";
        return false;
    }
    if (!HookAdd(nullptr, reinterpret_cast<uintptr_t>(present), reinterpret_cast<void *>(&PresentHook), reinterpret_cast<void **>(&g_present), err))
        return false;
    HLOG("screenshot: Present hooked at %s", DescribeAddress(reinterpret_cast<uintptr_t>(present)).c_str());
    g_hooked = true;
    return true;
}

} // namespace

bool ScreenshotHookPresent(std::string *err) {
    std::string e;
    return EnsureHooked(err ? err : &e);
}

// Game thread (the bridge's jobs run there): hooking is done on the thread that renders.
bool ScreenshotMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "screenshot" && method != "screenshot.state") return false;
    *ok = false;
    if (method == "screenshot") {
        std::string err;
        if (!EnsureHooked(&err)) { *reply = "screenshot: " + err; return true; }
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_wantId) g_wantId = g_nextId++;   // one asked for and not taken yet: the same frame answers both
        *reply = json{{"id", g_wantId}}.dump(-1, ' ', false, json::error_handler_t::replace);
        *ok = true;
        return true;
    }
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    int id = p.is_object() ? p.value("id", 0) : 0;
    std::lock_guard<std::mutex> lk(g_mx);
    for (const Result &r : g_results)
        if (r.id == id) {
            *reply = json{{"id", id}, {"done", true}, {"ok", r.ok}, {"path", r.path}, {"error", r.error}, {"width", r.width}, {"height", r.height}}.dump(-1, ' ', false, json::error_handler_t::replace);
            *ok = true;
            return true;
        }
    *reply = json{{"id", id}, {"done", false}}.dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
