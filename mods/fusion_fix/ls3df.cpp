// ls3df.cpp: the field of view at the screen's aspect and the videos fitted (Fusion Fix's LS3DF.ixx), the borderless
// window and the shadows' z-bias (its dllmain.cpp InitLS3DF), on the loader: ff.h.
#include "ff.h"
#include <d3d9.h>
#include <tuple>
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>

namespace ff {

struct FovSite
{
    uint8_t cameraRegister;
    uint32_t field;
    float multiplier = 0.5f;
};
std::array<FovSite, 16> fovSites;

template<size_t I> void CorrectFov(MmlMidContext& regs)
{
    auto& site = fovSites[I];
    uintptr_t camera = site.cameraRegister == 5 ? regs.ebp : site.cameraRegister == 3 ? regs.ebx : regs.edx;
    double angle = Field<float>(camera, site.field);
    // Change the existing half-angle multiplier before the native FLD. This
    // leaves the x87 stack, camera FOV fields, and projection code intact.
    site.multiplier = angle == 0.0 ? 0.5f : static_cast<float>(
        std::atan(std::tan(angle * 0.5) * GetCameraFovScale()) / angle);
}

template<size_t... I> constexpr auto FovCallbacks(std::index_sequence<I...>)
{
    return std::array{&CorrectFov<I>...};
}

void RefreshCameraFov(MmlMidContext& regs)
{
    auto camera = regs.ecx;
    if (Field<uint8_t>(camera, 0x15C)) return; // orthographic camera
    double scale = GetCameraFovScale();
    for (auto [field, projection] : { std::pair{0x140u, 0x1A4u}, std::pair{0x14Cu, 0x364u} })
    {
        double angle = Field<float>(camera, field);
        if (!std::isfinite(angle) || angle <= 0.0 || angle >= std::numbers::pi) return;
        double expected = 1.0 / (std::tan(angle * 0.5) * scale);
        double cached = Field<float>(camera, projection);
        if (std::isfinite(cached) && std::abs(cached - expected) <= expected * 0.00001) continue;

        // SetFOV rebuilds both native projection matrices and advances the
        // camera's revision. Update also rebuilds its view matrices and frustum
        // before Render can use caches prepared earlier in the same frame.
        // A gameplay camera prepared during the intro must not retain that
        // cutscene projection after the bars are disabled (or vice versa).
        auto table = *reinterpret_cast<uintptr_t**>(camera);
        using SetFOV = void(__stdcall*)(void*, float);
        reinterpret_cast<SetFOV>(table[0x50 / sizeof(uintptr_t)])(
            reinterpret_cast<void*>(camera), Field<float>(camera, 0x140));
        using Update = void(__thiscall*)(void*);
        reinterpret_cast<Update>(table[0x44 / sizeof(uintptr_t)])(reinterpret_cast<void*>(camera));
        return;
    }
}

void InitFOV()
{
    auto module = Ls3dBase();
    if (!module) return;
    std::vector<std::pair<uintptr_t, uintptr_t>> matches;
    // 1.0 duplicates projection calculations in eight camera methods; later
    // engines have a shared method. Find both FOV fields in either layout.
    for (auto bytes : {"D9 85 40 01 00 00", "D9 83 40 01 00 00", "D9 82 40 01 00 00",
                       "D9 85 4C 01 00 00", "D9 83 4C 01 00 00", "D9 82 4C 01 00 00"})
    {
        auto pattern = find_module_pattern<UINT32_MAX>(module, bytes);
        for (size_t i = 0; i < pattern.size(); ++i)
        {
            auto load = reinterpret_cast<uintptr_t>(pattern.get(i).get<void>());
            for (size_t offset : {6u, 8u, 11u})
            {
                auto multiply = load + offset;
                if (Field<uint16_t>(multiply, 0) != 0x0DD8) continue;
                auto* constant = Field<float*>(multiply, 2);
                if (*constant != 0.5f) continue;
                bool cosine = false, sine = false;
                for (size_t j = 6; j < 22; ++j)
                {
                    cosine |= Field<uint16_t>(multiply, j) == 0xFFD9;
                    sine |= Field<uint16_t>(multiply, j) == 0xFED9;
                }
                if (cosine && sine) matches.emplace_back(load, multiply);
            }
        }
    }
    if (matches.size() != 2 && matches.size() != 16)
        return;
    constexpr auto callbacks = FovCallbacks(std::make_index_sequence<16>{});
    for (size_t i = 0; i < matches.size(); ++i)
    {
        auto [load, multiply] = matches[i];
        auto& site = fovSites[i];
        site.cameraRegister = Field<uint8_t>(load, 1) & 7;
        site.field = Field<uint32_t>(load, 2);
        site.multiplier = 0.5f;
        if (Mid(load, callbacks[i])) WriteMemory(multiply + 2, &site.multiplier);
    }
    // Called for the active camera on every scene render, before the renderer
    // checks its revision. Projection setters alone miss cutscene transitions.
    auto pCamera = find_module_pattern(module,
        "56 8B F1 8B 86 F4 03 00 00 85 C0 8A 86 AC 00 00 00 74 ? A8 20");
    if (pCamera.size() == 1)
    {
        Mid(pCamera.get_first(), RefreshCameraFov);
    }
}

// (The loader: Fusion Fix's patches taken back, the game's camera keeps the projection they made until the game sets
// its field of view again, which it does only when it changes: SetFOV and Update called once on it here, as
// RefreshCameraFov does, with the game's own multiplier back.)
void ResetFOV()
{
    void* gameObj = game::Game();
    if (!gameObj || !game::Exe().gameCamera) return;
    auto camera = Field<uintptr_t>(reinterpret_cast<uintptr_t>(gameObj) + game::Exe().gameCamera, 4);
    if (!camera || Field<uint8_t>(camera, 0x15C)) return; // none, or an orthographic camera
    auto table = *reinterpret_cast<uintptr_t**>(camera);
    using SetFOV = void(__stdcall*)(void*, float);
    reinterpret_cast<SetFOV>(table[0x50 / sizeof(uintptr_t)])(reinterpret_cast<void*>(camera), Field<float>(camera, 0x140));
    using Update = void(__thiscall*)(void*);
    reinterpret_cast<Update>(table[0x44 / sizeof(uintptr_t)])(reinterpret_cast<void*>(camera));
}

void InitMovie()
{
    auto module = Ls3dBase();
    if (!module) return;
    auto pattern = find_module_pattern(module,
        "8B 4C 24 14 8B 54 24 00 8D 41 28 89 10 8B 54 24 04",
        "8B 4C 24 14 8B 54 24 00 8D 41 2C 89 10 8B 54 24 04");
    if (pattern.size() != 1) return;
    Mid(pattern.get_first(), [](MmlMidContext& regs)
    {
        auto& rect = Field<RECT>(regs.esp, 0);
        if (!S.zoomFmvs)
        {
            rect.left = static_cast<LONG>(std::nearbyint((Screen.fWidth - rect.right / fMovieAspect) * 0.5));
            rect.right = static_cast<LONG>(std::nearbyint(rect.right / fMovieAspect));
        }
        else
        {
            rect.top = static_cast<LONG>(std::nearbyint((Screen.fHeight - rect.bottom * fMovieAspect) * 0.5));
            rect.bottom = static_cast<LONG>(std::nearbyint(rect.bottom * fMovieAspect));
        }
    });
}


// ---- dllmain.cpp InitLS3DF, with PostFX.ixx's device pointer

namespace {
IUnknown** pD3D8Device = nullptr;

void InitDevicePointer()
{
    if (pD3D8Device) return;
    auto pattern = find_module_pattern(Ls3dBase(), "A1 ? ? ? ? 53 6A ? 89 1D ? ? ? ? ? ? 50 FF 91 ? ? ? ? 8B 4C 24");
    if (!pattern.empty())
        pD3D8Device = *pattern.get_first<IUnknown**>(1);
}

IDirect3DDevice9* GetDevice9()
{
    InitDevicePointer();
    if (!pD3D8Device || !*pD3D8Device) return nullptr;
    IDirect3DDevice9* dev9 = nullptr;
    if (FAILED((*pD3D8Device)->QueryInterface(__uuidof(IDirect3DDevice9), (void**)&dev9)))
        return nullptr;
    dev9->Release();
    return dev9;
}

int WINAPI GetSystemMetricsHook(int nIndex)
{
    return 0;
}

std::tuple<int32_t, int32_t> GetDesktopRes()
{
    HMONITOR monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {};
    info.cbSize = sizeof(MONITORINFO);
    GetMonitorInfo(monitor, &info);
    return { info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top };
}

HWND WINAPI CreateWindowExAHook(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
    auto hwnd = ::CreateWindowExA(dwExStyle, lpClassName, lpWindowName, dwStyle & ~WS_OVERLAPPEDWINDOW, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);

    if (dwStyle != 0x50000000)
    {
        auto [DesktopResW, DesktopResH] = GetDesktopRes();
        tagRECT rect;
        rect.left = (LONG)(((float)DesktopResW / 2.0f) - (nWidth / 2.0f));
        rect.top = (LONG)(((float)DesktopResH / 2.0f) - (nHeight / 2.0f));
        rect.right = nWidth;
        rect.bottom = nHeight;
        SetWindowPos(hwnd, NULL, rect.left, rect.top, rect.right, rect.bottom, SWP_NOACTIVATE | SWP_NOZORDER);
    }

    return hwnd;
}

// The loader: Fusion Fix's two imports take effect when LS3D makes its window, which it has made before mods load;
// the window there is made borderless at once as CreateWindowExAHook makes a new one (its frame off, its client
// area kept, centred on the desktop), and given back as it was when the setting goes off.
HWND g_window;
LONG g_windowStyle;
RECT g_windowRect;

void BorderlessNow()
{
    void* graph = *reinterpret_cast<void**>(game::Exe().igraph);
    if (!graph) return;
    using GetWindow = HWND(__stdcall*)(void*);
    HWND window = Virtual<GetWindow>(graph, 16)(graph);
    if (!window) return;
    LONG style = GetWindowLongA(window, GWL_STYLE);
    if (!(style & WS_CAPTION) || (style & WS_CHILD)) return;   // full screen, or already without a frame
    RECT client{};
    GetClientRect(window, &client);
    g_window = window;
    g_windowStyle = style;
    GetWindowRect(window, &g_windowRect);
    SetWindowLongA(window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
    auto [DesktopResW, DesktopResH] = GetDesktopRes();
    int w = client.right - client.left, h = client.bottom - client.top;
    SetWindowPos(window, NULL, (DesktopResW - w) / 2, (DesktopResH - h) / 2, w, h, SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
}
void ZBias(uintptr_t args)
{
    constexpr auto D3DRS_ZBIAS = 47;
    constexpr auto D3DRS_ZVISIBLE = 30;
    auto& State = Field<uint32_t>(args, 4);
    auto& Value = Field<uint32_t>(args, 8);

    if (State != D3DRS_ZBIAS)
        return;

    if (Value)
        Value = 16;

    // On real D3D8 the driver biases slope dependently, so ZBIAS alone fixes the flickering.
    // d3d8to9 only turns it into a constant D3DRS_DEPTHBIAS, so set the bias on the D3D9 device
    // ourselves and hand d3d8to9 a state it ignores (ZVISIBLE returns D3D_OK at once), otherwise
    // its own conversion would overwrite the values set here.
    auto pDevice9 = GetDevice9();
    if (!pDevice9)
        return;

    constexpr float fDepthBiasUnit = -1.0f / ((1 << 20) - 1); // 24-bit depth, same as CalcDepthBias
    const float fDepthBias = Value * fDepthBiasUnit;
    const float fSlopeDepthBias = -0.125f * static_cast<float>(Value); // per ZBIAS unit, -2.0 at 16
    pDevice9->SetRenderState(D3DRS_DEPTHBIAS, *reinterpret_cast<const DWORD*>(&fDepthBias));
    pDevice9->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, *reinterpret_cast<const DWORD*>(&fSlopeDepthBias));

    State = D3DRS_ZVISIBLE;
}
} // namespace

void ResetLS3DF()
{
    if (g_window && IsWindow(g_window))
    {
        SetWindowLongA(g_window, GWL_STYLE, g_windowStyle);
        SetWindowPos(g_window, NULL, g_windowRect.left, g_windowRect.top, g_windowRect.right - g_windowRect.left,
            g_windowRect.bottom - g_windowRect.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    g_window = nullptr;
}

void InitLS3DF()
{
    InitProjectorOpacity();

    if (S.borderlessWindowed)
    {
        auto pattern = find_module_pattern(Ls3dBase(), "8B 35 ? ? ? ? 0F 84 ? ? ? ? 8B 3D ? ? ? ? 55 55 55 55");
        if (pattern.size() == 1)
            WriteMemory(*pattern.get_first<uint32_t*>(2), reinterpret_cast<uint32_t>(GetSystemMetricsHook));
        pattern = find_module_pattern(Ls3dBase(), "FF 15 ? ? ? ? 3B C5 A3 ? ? ? ? 0F 84 ? ? ? ? 50");
        if (pattern.size() == 1)
            WriteMemory(*pattern.get_first<uint32_t*>(2), reinterpret_cast<uint32_t>(CreateWindowExAHook));
        BorderlessNow();
    }

    // Shadows Z-Fighting
    {
        std::vector<std::pair<std::string, int>> patterns = {
            { "FF 92 ? ? ? ? A1 ? ? ? ? ? ? 6A ? 6A ? 50 FF 91 ? ? ? ? F6 86", 0 },
            { "FF 92 ? ? ? ? A1 ? ? ? ? ? ? 6A ? 6A ? 50 FF 91 ? ? ? ? 8B 96", 0 },
            { "FF 92 ? ? ? ? ? ? ? ? 56", 0 },
            { "51 6A ? 89 0D ? ? ? ? ? ? 50 FF 92 ? ? ? ? C3", 12 },
            { "FF A2 ? ? ? ? 8B 44 24 ? 39 05 ? ? ? ? 0F 84 ? ? ? ? 8B 0D ? ? ? ? 50 6A ? 6A ? A3 ? ? ? ? ? ? 51 FF 92 ? ? ? ? 5B C2 ? ? 8B 44 24 ? 39 05 ? ? ? ? 0F 84 ? ? ? ? 8B 0D ? ? ? ? 50 6A ? 6A ? A3 ? ? ? ? ? ? 51 FF 92 ? ? ? ? 5B C2 ? ? 8B 44 24 ? 8B 0D", 0 },
        };

        for (const auto& p : patterns)
        {
            auto pattern = find_module_pattern(Ls3dBase(), p.first.c_str());
            if (pattern.size() != 1)
                continue;
            // IDirect3DDevice8::SetRenderState(device, state, value): its arguments from esp at a call. (The loader:
            // at the jump of the last pattern, a tail call, they are past the return address, from esp + 4; Fusion
            // Fix reads them from esp there too, so its bias is not set at that site.)
            bool tail = p.first.rfind("FF A2", 0) == 0;
            Mid(pattern.get_first(p.second), tail ? MidFn([](MmlMidContext& regs) { ZBias(regs.esp + 4); })
                                                 : MidFn([](MmlMidContext& regs) { ZBias(regs.esp); }));
        }
    }
}

} // namespace ff
