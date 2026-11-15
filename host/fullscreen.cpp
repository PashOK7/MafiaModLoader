// The game's full screen as a borderless window while a mod with "gfx": true is on. Setup's Fullscreen makes LS3D ask
// for an exclusive full-screen device (IGraph::Init: a topmost window the mode's size, Windowed false, the mode's
// size and refresh). There one swap chain alone may show, so the bridge's showing (gfx_show: frame generation's
// pictures) could only be shown on the game's thread, with the waits between the pictures on it: the game's rate fell
// to its own over (pictures + 1) (measured 2026-10-25, docs/JOURNAL.md). Made a window instead, at CreateDevice and at
// each Reset (LS3D resets with its own parameters: the full screen's again), the device keeps the mode's back buffer;
// LS3D's window loses its frame and covers the adapter's monitor, the picture is shown at the mode's proportions in the
// middle of it (Present's destination: the game's and the showing's), the bars beside it painted black (LS3D's window
// erases nothing). The monitor's own mode is left alone; the game's mouse is DirectInput's moves, not the window's.
// Nothing when no mod has "gfx": true: the game's exclusive full screen as it was.
#include "host.h"
#include <d3d9.h>
#include <algorithm>
#include <cstdio>
#include "json.hpp"
#include "mml/game/game_build.h"

namespace host {
namespace {

// Direct3D 8's D3DPRESENT_PARAMETERS, 13 dwords
enum { kW = 0, kH = 1, kSwap = 5, kWnd = 6, kWindowed = 7, kRefresh = 11, kInterval = 12 };
constexpr int kVtGetAdapterMonitor = 14;   // IDirect3D8's vtable

bool g_on;                 // the device is a window made from a full screen
HWND g_wnd;
RECT g_monitor;            // the adapter's monitor (screen coordinates)
RECT g_dest;               // the picture in the window's client area
UINT g_modeW, g_modeH;
unsigned g_paint;          // Presents until the bars are painted again
std::string g_state = "the game's own (no full screen asked, or no mod with \"gfx\": true)";

void Place() {
    const int mw = g_monitor.right - g_monitor.left, mh = g_monitor.bottom - g_monitor.top;
    // the mode's proportions in the monitor, whole pixels, in the middle
    double s = std::min(static_cast<double>(mw) / g_modeW, static_cast<double>(mh) / g_modeH);
    int w = std::min(mw, static_cast<int>(g_modeW * s + 0.5)), h = std::min(mh, static_cast<int>(g_modeH * s + 0.5));
    g_dest = {(mw - w) / 2, (mh - h) / 2, (mw - w) / 2 + w, (mh - h) / 2 + h};
    LONG_PTR style = GetWindowLongPtrW(g_wnd, GWL_STYLE), ex = GetWindowLongPtrW(g_wnd, GWL_EXSTYLE);
    SetWindowLongPtrW(g_wnd, GWL_STYLE, (style & ~(WS_OVERLAPPEDWINDOW | WS_DLGFRAME | WS_BORDER)) | WS_POPUP | WS_CLIPSIBLINGS);
    SetWindowLongPtrW(g_wnd, GWL_EXSTYLE, ex & ~(WS_EX_TOPMOST | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME));
    // (not topmost: a borderless window over its monitor; no activation: a lab's game may be in the background)
    SetWindowPos(g_wnd, HWND_NOTOPMOST, g_monitor.left, g_monitor.top, mw, mh, SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    g_paint = 0;
    char s2[256];
    snprintf(s2, sizeof s2, "a borderless window over the monitor at (%ld, %ld) %dx%d, the picture %ux%u shown at %dx%d in its "
             "middle (Setup's full screen made a window for the graphics bridge's showing)",
             g_monitor.left, g_monitor.top, mw, mh, g_modeW, g_modeH, w, h);
    g_state = s2;
}

// The parameters of a full screen made a window's: the same back buffer, no refresh or interval of a mode.
bool MakeWindowed(DWORD *pp, const char *when) {
    if (!pp || pp[kWindowed] || ModsWantingGfx().empty()) return false;
    g_modeW = pp[kW], g_modeH = pp[kH];
    if (!g_modeW || !g_modeH) return false;
    pp[kWindowed] = TRUE;
    pp[kRefresh] = 0;
    pp[kInterval] = 0;   // (D3DPRESENT_INTERVAL_DEFAULT: a window's)
    HLOG("gfx: %s: the full screen %ux%u made a borderless window (a mod with \"gfx\": true is on: the bridge's showing "
         "needs a window)", when, static_cast<unsigned>(g_modeW), static_cast<unsigned>(g_modeH));
    return true;
}

void Bars() {
    HDC dc = GetDC(g_wnd);
    if (!dc) return;
    RECT c{};
    GetClientRect(g_wnd, &c);
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RECT r[4] = {{0, 0, c.right, g_dest.top}, {0, g_dest.bottom, c.right, c.bottom},
                 {0, g_dest.top, g_dest.left, g_dest.bottom}, {g_dest.right, g_dest.top, c.right, g_dest.bottom}};
    for (const RECT &b : r) if (b.right > b.left && b.bottom > b.top) FillRect(dc, &b, black);
    ReleaseDC(g_wnd, dc);
}

} // namespace

void FullscreenCreate(void *d3d8, UINT adapter, HWND focus, DWORD *pp) {
    g_on = false;
    if (!MakeWindowed(pp, "CreateDevice")) return;
    g_wnd = pp[kWnd] ? reinterpret_cast<HWND>(static_cast<uintptr_t>(pp[kWnd])) : focus;
    using MonitorFn = HMONITOR(STDMETHODCALLTYPE *)(void *, UINT);
    HMONITOR mon = d3d8 ? reinterpret_cast<MonitorFn>((*static_cast<void ***>(d3d8))[kVtGetAdapterMonitor])(d3d8, adapter) : nullptr;
    if (!mon) mon = MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    if (!g_wnd || !GetMonitorInfoW(mon, &mi)) {
        g_state = "a window made from the full screen, but its window or monitor was not found: placed as LS3D made it";
        HWARN("gfx: %s", g_state.c_str());
        return;
    }
    g_monitor = mi.rcMonitor;
    g_on = true;
    Place();
}

void FullscreenReset(DWORD *pp) {
    if (!g_on) return;
    if (MakeWindowed(pp, "Reset")) Place();   // (the mode may be another: the options' resolution)
}

const RECT *FullscreenDest() {
    return g_on ? &g_dest : nullptr;
}

void FullscreenPresent() {
    if (!g_on) return;
    // the bars at the first frames after a placement, then every 120th frame (in case something drew over them)
    if (g_paint == 0 || g_paint == 2 || g_paint % 120 == 0) Bars();
    g_paint++;
}

std::string FullscreenState() {
    return g_state;
}

namespace {
// Windowed LS3D uses nonexclusive DirectInput, while the game hides the system cursor.
// Keep that invisible pointer over this window so Windows cannot wheel-scroll another application.
// Include the frame: resize arrows, caption buttons and the sizing loop must remain reachable.
HWND g_cursorWnd;
HWND g_cursorTarget;
WNDPROC g_cursorProc;
RECT g_cursorRect{};
bool g_cursorClipped, g_cursorSuspended;
unsigned g_clientCursorHides;

bool ClientCursorActive() {
    return g_cursorWnd && !g_cursorSuspended && GetForegroundWindow() == g_cursorWnd &&
           IsWindowVisible(g_cursorWnd) && IsWindowEnabled(g_cursorWnd) && !IsIconic(g_cursorWnd);
}

void HideClientCursor() {
    // The game draws its own cursor. Do not rely on another component's ShowCursor display count,
    // and do not decrement that count every frame: select no system cursor for this client instead.
    if (GetCursor()) { SetCursor(nullptr); ++g_clientCursorHides; }
}

void RefreshClientCursor() {
    if (!ClientCursorActive()) return;
    POINT point{};
    if (!GetCursorPos(&point)) return;
    HWND under = WindowFromPoint(point);
    if (under != g_cursorWnd && under != g_cursorTarget) return;
    RECT client{};
    if (!ScreenToClient(g_cursorWnd, &point) || !GetClientRect(g_cursorWnd, &client) || !PtInRect(&client, point)) return;
    HideClientCursor();
}

void ReleaseCursorClip() {
    if (!g_cursorClipped) return;
    RECT current{};
    // ClipCursor is shared desktop state. Do not undo a newer restriction from somebody else.
    if (GetClipCursor(&current) && EqualRect(&current, &g_cursorRect)) ClipCursor(nullptr);
    g_cursorClipped = false;
}

void UpdateCursorClip() {
    if (!g_cursorWnd || g_cursorSuspended || GetForegroundWindow() != g_cursorWnd ||
        !IsWindowVisible(g_cursorWnd) || IsIconic(g_cursorWnd)) {
        ReleaseCursorClip();
        return;
    }
    RECT bounds{}, current{};
    if (!GetWindowRect(g_cursorWnd, &bounds) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        ReleaseCursorClip();
        return;
    }
    if (g_cursorClipped && EqualRect(&bounds, &g_cursorRect) &&
        GetClipCursor(&current) && EqualRect(&current, &g_cursorRect)) return;
    if (ClipCursor(&bounds)) {
        // Windows intersects the requested rectangle with the desktop. Keep the actual restriction,
        // including windows partly outside a monitor, so release still recognizes our own clip.
        g_cursorClipped = GetClipCursor(&g_cursorRect) != FALSE;
    }
}

LRESULT CALLBACK CursorClipProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    const bool inactive = (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) ||
                          (msg == WM_ACTIVATEAPP && !wp);
    if (inactive || msg == WM_ENTERSIZEMOVE || msg == WM_ENTERMENULOOP || msg == WM_NCDESTROY ||
        (msg == WM_ENABLE && !wp)) {
        g_cursorSuspended = true;
        ReleaseCursorClip();   // immediately, even when the next game frame never runs
    }
    if ((msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) || (msg == WM_ACTIVATEAPP && wp) ||
        msg == WM_EXITSIZEMOVE || msg == WM_EXITMENULOOP || (msg == WM_ENABLE && wp))
        g_cursorSuspended = false;
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && HIWORD(lp) && ClientCursorActive() &&
        (reinterpret_cast<HWND>(wp) == wnd || reinterpret_cast<HWND>(wp) == g_cursorTarget)) {
        HideClientCursor();
        return TRUE;   // stop DefWindowProc from restoring the window class's cursor
    }
    LRESULT result = CallWindowProcW(g_cursorProc, wnd, msg, wp, lp);
    if (msg == WM_NCDESTROY) { g_cursorWnd = nullptr; g_cursorTarget = nullptr; g_cursorProc = nullptr; }
    else if (msg == WM_ACTIVATE || msg == WM_ACTIVATEAPP || msg == WM_WINDOWPOSCHANGED ||
             msg == WM_EXITSIZEMOVE || msg == WM_EXITMENULOOP || msg == WM_ENABLE)
        UpdateCursorClip();
    if (msg == WM_ACTIVATE || msg == WM_ACTIVATEAPP || msg == WM_SETFOCUS ||
        msg == WM_EXITSIZEMOVE || msg == WM_EXITMENULOOP || msg == WM_ENABLE)
        RefreshClientCursor();
    return result;
}

void CursorClipWindow(HWND wnd, HWND target) {
    if (!wnd) return;
    if (g_cursorWnd == wnd) { g_cursorTarget = target; return; }
    if (g_cursorWnd) return;   // only the game's primary window, never an auxiliary/overlay window
    auto previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CursorClipProc)));
    if (!previous) { HWARN("input: cannot subclass the game window for cursor confinement (%lu)", GetLastError()); return; }
    g_cursorWnd = wnd; g_cursorTarget = target; g_cursorProc = previous; g_cursorSuspended = false;
    HLOG("input: foreground cursor stays inside the game window; released on deactivation and move/size");
    UpdateCursorClip();
}

HWND g_resizeWnd;
HWND g_resizeChild;
WNDPROC g_resizeProc;
bool g_sizing, g_resizing, g_resizeReady = true;
ULONGLONG g_retryAt;
unsigned g_renderW, g_renderH, g_resets;
ULONGLONG g_sizeAt;
std::string g_resizeError;
constexpr unsigned kMinW = 640, kMinH = 480, kMaxSize = 8192;
unsigned g_resizeCursorShows;
void RestoreResizeCursor() {
    while (g_resizeCursorShows) { ShowCursor(FALSE); --g_resizeCursorShows; }
}
void ShowResizeCursor(HWND wnd, int hit) {
    int shape = hit == HTLEFT || hit == HTRIGHT ? 32644 : hit == HTTOP || hit == HTBOTTOM ? 32645 :
                hit == HTTOPLEFT || hit == HTBOTTOMRIGHT ? 32642 : 32643;
    SetCursor(LoadCursorW(nullptr,MAKEINTRESOURCEW(shape)));
    // The game hides the Windows cursor while drawing its own. DefWindowProc selects the right resize
    // shape but does not make a hidden cursor visible until its modal sizing loop begins. Borrow only
    // the increments needed to reach zero; give exactly those back on client entry/leave/deactivation.
    int count = ShowCursor(TRUE);
    if (count > 0) ShowCursor(FALSE);
    else {
        ++g_resizeCursorShows;
        while (count < 0) { count = ShowCursor(TRUE); ++g_resizeCursorShows; }
    }
    TRACKMOUSEEVENT track{sizeof track,TME_NONCLIENT|TME_LEAVE,wnd,0};
    TrackMouseEvent(&track);
}

RECT Client() { RECT r{}; if (g_resizeWnd) GetClientRect(g_resizeWnd, &r); return r; }
RECT Outer(unsigned w, unsigned h) {
    RECT r{0, 0, LONG(w), LONG(h)};
    AdjustWindowRectEx(&r, DWORD(GetWindowLongPtrW(g_resizeWnd, GWL_STYLE)), GetMenu(g_resizeWnd) != nullptr,
                       DWORD(GetWindowLongPtrW(g_resizeWnd, GWL_EXSTYLE)));
    return r;
}
bool SizeWindow(unsigned w, unsigned h) {
    RECT r = Outer(w, h);
    return SetWindowPos(g_resizeWnd, nullptr, 0, 0, r.right-r.left, r.bottom-r.top,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}
// No rendering, resets or TLS in the window procedure. DefWindowProc's move/size loop may re-enter it.
LRESULT CALLBACK ResizeProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCMOUSELEAVE || msg == WM_ENTERSIZEMOVE || msg == WM_EXITSIZEMOVE ||
        msg == WM_NCDESTROY || msg == WM_KILLFOCUS || (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE))
        RestoreResizeCursor();
    if (msg == WM_SETCURSOR) {
        const int hit = LOWORD(lp);
        if (hit >= HTLEFT && hit <= HTBOTTOMRIGHT && !IsZoomed(wnd)) {
            ShowResizeCursor(wnd,hit);
            return TRUE;
        }
        RestoreResizeCursor();
    }
    if (msg == WM_ENTERSIZEMOVE) g_sizing = true;
    if (msg == WM_EXITSIZEMOVE) { g_sizing = false; g_sizeAt = 0; }
    if (msg == WM_SIZE) {
        g_sizeAt = GetTickCount64();
        if (wp != SIZE_MINIMIZED && g_resizeChild) {
            RECT c = Client();
            SetWindowPos(g_resizeChild, nullptr, 0, 0, c.right, c.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    LRESULT result = CallWindowProcW(g_resizeProc, wnd, msg, wp, lp);
    if (msg == WM_GETMINMAXINFO && lp) {
        RECT r = Outer(kMinW, kMinH);
        auto *m = reinterpret_cast<MINMAXINFO *>(lp);
        m->ptMinTrackSize.x = r.right-r.left; m->ptMinTrackSize.y = r.bottom-r.top;
    }
    if (msg == WM_NCDESTROY && wnd == g_resizeWnd) { g_resizeWnd=nullptr; g_resizeChild=nullptr; g_resizeProc=nullptr; }
    return result;
}

nlohmann::json WindowState() {
    RECT c = Client();
    unsigned size[2]{};
    if (game::g_ls3d) {
        uintptr_t at=ModuleBase("LS3DF.dll") + game::Ls3d().graphScreenSize;
        unsigned flags=0; MemRead(at+0x1C,&flags,4);
        MemRead(at+((flags&0x80)?0x10:0), size, sizeof size);
    }
    return {{"resizable", g_resizeWnd != nullptr}, {"client", {c.right, c.bottom}},
            {"cursor_confined", g_cursorClipped}, {"cursor_window", reinterpret_cast<uintptr_t>(g_cursorWnd)},
            {"cursor_shape", reinterpret_cast<uintptr_t>(GetCursor())}, {"cursor_client_hides", g_clientCursorHides},
            {"cursor_rect", {g_cursorRect.left, g_cursorRect.top, g_cursorRect.right, g_cursorRect.bottom}},
            {"render", {g_renderW, g_renderH}}, {"native", {size[0], size[1]}},
            {"minimized", g_resizeWnd && IsIconic(g_resizeWnd)}, {"maximized", g_resizeWnd && IsZoomed(g_resizeWnd)},
            {"sizing", g_sizing}, {"ready", g_resizeReady}, {"resets", g_resets}, {"last_error", g_resizeError},
            {"pending", g_resizeWnd && !IsIconic(g_resizeWnd) && (!g_resizeReady || unsigned(c.right)!=g_renderW || unsigned(c.bottom)!=g_renderH)}};
}
}

void WindowDevice(void *dev, HWND focus, DWORD *pp) {
    // Exclusive and bridge-borderless devices retain their existing mode. Only ordinary top-level windows resize.
    if (!pp || !pp[kWindowed] || !game::g_ls3d) return;
    HWND target = pp[kWnd] ? reinterpret_cast<HWND>(uintptr_t(pp[kWnd])) : focus;
    HWND wnd = target ? GetAncestor(target,GA_ROOT) : nullptr;
    if (!wnd) return;
    CursorClipWindow(wnd, target);   // ordinary and bridge-borderless windows; exclusive DirectInput already owns its mouse
    if (FullscreenDest()) return;
    if (g_resizeWnd && g_resizeWnd != wnd) return;
    g_resizeWnd = wnd; g_resizeReady = true;
    g_resizeChild = target != wnd ? target : nullptr;
    g_renderW = pp[kW]; g_renderH = pp[kH];
    RECT old = Client();
    if (!g_renderW) g_renderW = old.right;
    if (!g_renderH) g_renderH = old.bottom;
    if (!g_resizeProc) {
        SetLastError(0);
        g_resizeProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ResizeProc)));
        if (!g_resizeProc) { g_resizeWnd = nullptr; HWARN("window: cannot subclass (%lu)", GetLastError()); return; }
    }
    LONG_PTR style = GetWindowLongPtrW(wnd, GWL_STYLE);
    SetWindowLongPtrW(wnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
    RECT r = Outer(old.right, old.bottom);
    SetWindowPos(wnd, nullptr, 0, 0, r.right-r.left, r.bottom-r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    HLOG("window: resizable client %ux%u (native ResetDevice between frames)", g_renderW, g_renderH);
}

bool WindowRenderReady() { return g_resizeReady; }

void WindowScene(void *scene) {
    if (!g_resizeWnd || !g_resizeReady || !scene) return;
    // Reset does not resize scenes created at mission load. A stale rectangle can exceed the new back buffer:
    // native Render's SetViewport then fails and it returns without drawing even though Reset succeeded.
    auto *bytes = static_cast<uint8_t *>(scene);
    const int *rect = reinterpret_cast<const int *>(bytes+0x23C);
    if (rect[0] || rect[1] || (rect[2]==int(g_renderW) && rect[3]==int(g_renderH))) return;
    using SceneVp = int(__stdcall *)(void *, unsigned, unsigned, unsigned, unsigned);
    reinterpret_cast<SceneVp>(ModuleBase("LS3DF.dll")+game::Ls3d().sceneSetViewport)(scene,0,0,g_renderW,g_renderH);
    void *cam = *reinterpret_cast<void **>(bytes+0x17C);
    if (cam && *static_cast<uintptr_t *>(cam)==ModuleBase("LS3DF.dll")+game::Ls3d().cameraVtable) {
        using Aspect = void(__stdcall *)(void *, float);
        reinterpret_cast<Aspect>((*static_cast<void ***>(cam))[0x68/4])(cam,float(g_renderW)/g_renderH);
    }
}

void WindowFrame() {
    UpdateCursorClip();
    RefreshClientCursor();   // focus return need not deliver a mouse move/WM_SETCURSOR
    if (!g_resizeWnd || g_resizing || g_sizing || !IsWindow(g_resizeWnd) || IsIconic(g_resizeWnd)) return;
    RECT c = Client();
    unsigned w = c.right, h = c.bottom;
    if (w < kMinW || h < kMinH || w > kMaxSize || h > kMaxSize || (g_resizeReady && w == g_renderW && h == g_renderH)) return;
    if (!g_resizeReady && GetTickCount64()<g_retryAt) return;
    if (GetTickCount64() - g_sizeAt < 100) return; // coalesce programmatic WM_SIZE bursts; one reset after a drag
    g_resizing = true;
    const auto &l = game::Ls3d();
    uintptr_t base = ModuleBase("LS3DF.dll");
    auto *pp = reinterpret_cast<DWORD *>(base+l.graphPresentParams);
    auto *size = reinterpret_cast<unsigned *>(base+l.graphScreenSize);
    DWORD previous[13]; memcpy(previous, pp, sizeof previous);
    unsigned oldSize[2] = {size[0], size[1]};
    pp[kW] = w; pp[kH] = h;
    HLOG("window: native resize %ux%u -> %ux%u (format %lu, count %lu, swap %lu, wnd %08lX, windowed %lu, refresh %lu)",
         g_renderW,g_renderH,w,h,pp[2],pp[3],pp[kSwap],pp[kWnd],pp[kWindowed],pp[kRefresh]);
    using ResetFn = int(__fastcall *)(bool);
    int result = reinterpret_cast<ResetFn>(base+l.graphResetDevice)(true);
    if (result >= 0) {
        g_resizeReady = true;
        size[0] = w; size[1] = h;
        if (size[7]&0x80) { size[4]=w; size[5]=h; } // native child_rect dimensions, Scrn_sx/sy's usual windowed path
        // The native viewport cache is separate from D3D's. SetViewPort updates both.
        void *graph = nullptr; MemRead(game::Exe().igraph, &graph, sizeof graph);
        if (graph) {
            DWORD vp[6] = {0, 0, w, h, 0, 0x3F800000};
            using SetVp = int(__stdcall *)(void *, void *);
            reinterpret_cast<SetVp>((*static_cast<void ***>(graph))[19])(graph, vp);
        }
        using HudFn = void(__fastcall *)(uintptr_t, void *);
        reinterpret_cast<HudFn>(game::Exe().hudResolutionChanged)(game::Exe().hud, nullptr);
        g_renderW = w; g_renderH = h; g_resets++; g_resizeError.clear();
        if (game::Exe().menuScale) {
            float scale[2] = {float(w)/800.0f, float(h)/600.0f};
            memcpy(reinterpret_cast<void *>(game::Exe().menuScale), scale, sizeof scale);
        }
        MenusResolutionChanged();
        HLOG("window: resized to %ux%u; native viewport, HUD and menus updated", w, h);
    } else {
        memcpy(pp, previous, sizeof previous);
        size[0] = oldSize[0]; size[1] = oldSize[1];
        SizeWindow(g_renderW, g_renderH); // implicit back-buffer dimensions must see the old client too
        int restored = reinterpret_cast<ResetFn>(base+l.graphResetDevice)(true);
        g_resizeReady = restored>=0;
        g_retryAt = GetTickCount64()+1000;
        g_resizeError = "native ResetDevice failed; previous mode " + std::string(restored >= 0 ? "restored" : "could not be restored");
        HWARN("window: %s", g_resizeError.c_str());
    }
    g_resizing = false;
}

bool WindowMethod(const std::string &method, const std::string &params, std::string *reply, bool *ok) {
    if (method != "window.state" && method != "window.resize" && method != "window.focus") return false;
    *ok = false;
    if (method == "window.focus") {
        const HWND window = g_cursorWnd ? g_cursorWnd : g_resizeWnd;
        if (!window || !IsWindow(window)) { *reply = "window.focus: no primary game window"; return true; }
        const BOOL result = StartActivateWindow(window);
        const HWND foreground = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        auto state = WindowState();
        state["activation_result"] = result != FALSE;
        state["foreground_window"] = static_cast<unsigned>(reinterpret_cast<uintptr_t>(foreground));
        state["foreground_pid"] = pid;
        state["foreground"] = foreground == window;
        state["launch_guard_bypassed"] = true;
        if (foreground != window) state["note"] = "native activation did not make the game foreground; no real input or system policy changed";
        *reply = state.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        *ok = true;
        return true;
    }
    if (method == "window.resize") {
        auto p = nlohmann::json::parse(params.empty() ? "{}" : params, nullptr, false);
        if (!g_resizeWnd) { *reply = "window.resize requires an ordinary windowed game (launch fullscreen:false)"; return true; }
        if (!p.is_object()) { *reply = "window.resize: an object with width/height or state"; return true; }
        if (p.contains("state")) {
            std::string s = p.value("state", "");
            if (s == "minimize") ShowWindow(g_resizeWnd, SW_SHOWMINNOACTIVE);
            else if (s == "restore") ShowWindow(g_resizeWnd, SW_SHOWNOACTIVATE);
            else if (s == "maximize") {
                // SC_MAXIMIZE uses the same native path as the title-bar button, without sending real input.
                SendMessageW(g_resizeWnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
            } else { *reply = "window.resize state: minimize, restore or maximize"; return true; }
        } else {
            if (!p.contains("width") || !p.contains("height") || !p["width"].is_number_integer() || !p["height"].is_number_integer()) {
                *reply = "window.resize: integer width and height"; return true;
            }
            int64_t w=p["width"].get<int64_t>(), h=p["height"].get<int64_t>();
            if (w<kMinW || h<kMinH || w>kMaxSize || h>kMaxSize) { *reply = "window.resize: client size 640x480 to 8192x8192"; return true; }
            if (IsIconic(g_resizeWnd) || IsZoomed(g_resizeWnd)) ShowWindow(g_resizeWnd, SW_SHOWNOACTIVATE);
            if (!SizeWindow(unsigned(w), unsigned(h))) { *reply = "window.resize: SetWindowPos failed"; return true; }
        }
    }
    *reply = WindowState().dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
