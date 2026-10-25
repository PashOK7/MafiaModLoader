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

} // namespace host
