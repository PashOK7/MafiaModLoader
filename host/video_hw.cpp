// The GPU's part of the game's videos (video_hw.h; video.cpp plays them).
//
// Decoding: FFmpeg's hardware decoders through D3D11VA (a Direct3D 11 device of the loader's on the adapter the game
// renders with, matched by the LUID of the game's Direct3D 9Ex device), else DXVA2 (a Direct3D 9Ex device of the
// loader's on that adapter, with its device manager). Both devices are Windows' own (d3d11.dll, d3d9.dll and
// dxva2.dll loaded from System32 by full path: a player's DXVK d3d9.dll or d3d11.dll beside the game is not taken),
// made at the first video that wants them and kept. Whether the GPU decodes a stream is asked of the driver before
// the decoder is opened: its decoder profile for the codec and bit depth (H.264, HEVC Main and Main 10, VP9 profiles 0
// and 2, AV1 Main, MPEG-2, VC-1) at the stream's size (D3D11: GetVideoDecoderConfigCount; DXVA2: the service's
// GUIDs, its render target formats and GetDecoderConfigurations). AV1 is then decoded by FFmpeg's own AV1 decoder (it
// decodes only on a GPU), else by dav1d.
//
// Each picture (an NV12 or P010 texture of the decoder's) is turned into BGRA at its size on the screen by the GPU:
// D3D11's video processor (VideoProcessorBlt: the colour matrix and range from the stream, the scaling) or DXVA2's
// StretchRect (the driver's YUV to RGB). The result goes into one of a few slots: textures made by the game's
// Direct3D 9Ex device with a shared handle and opened by the loader's device, so the game draws the picture without
// it ever leaving the GPU. The loader's device waits for its own work with an event query before a slot is handed
// over, and a slot is used again only once the game's device has finished drawing it (an event query issued after
// its draw: Drawn, Retired). Where the game's device cannot open or make a shared texture (DXVK's Direct3D 9: Vulkan
// underneath; plain Direct3D 9), the picture at its size on the screen is copied through the CPU (a staging texture,
// GetRenderTargetData) and uploaded as the software path's pictures are: still decoded and scaled on the GPU.
#include "host.h"
#include "video_hw.h"
#include "mml/game/game_build.h"
#include <initguid.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxva2api.h>
#include <algorithm>
#include <cstring>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/hwcontext_dxva2.h>
#include <libavutil/pixdesc.h>
}

namespace host {
namespace videohw {
namespace {

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }
std::string Fmt(const char *f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

// Windows' own DLLs, by their System32 path
HMODULE SystemDll(const wchar_t *name) {
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    std::wstring path = std::wstring(dir) + L"\\" + name;
    return LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
}

// ---- the adapter the game renders with
bool GameLuid(IDirect3DDevice9 *game9, LUID *luid, UINT *ordinal) {
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (!game9 || FAILED(game9->GetCreationParameters(&cp))) return false;
    *ordinal = cp.AdapterOrdinal;
    IDirect3D9 *d3d = nullptr;
    if (FAILED(game9->GetDirect3D(&d3d)) || !d3d) return false;
    IDirect3D9Ex *ex = nullptr;
    bool ok = SUCCEEDED(d3d->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void **>(&ex))) && ex &&
              SUCCEEDED(ex->GetAdapterLUID(cp.AdapterOrdinal, luid));
    SafeRelease(ex);
    SafeRelease(d3d);
    return ok;
}

IDirect3DDevice9 *GameDevice9() {
    void *dev8 = nullptr;
    if (!game::g_ls3d || !game::Ls3d().d3dDevice) return nullptr;
    MemRead(ModuleBase("LS3DF.dll") + game::Ls3d().d3dDevice, &dev8, 4);
    if (!dev8) return nullptr;
    IDirect3DDevice9 *d9 = nullptr;
    if (FAILED(static_cast<IUnknown *>(dev8)->QueryInterface(__uuidof(IDirect3DDevice9), reinterpret_cast<void **>(&d9)))) return nullptr;
    return d9;
}

// ---- the loader's Direct3D 11 device and its FFmpeg device (made once)
struct D11 {
    bool tried = false;
    std::string why;
    ID3D11Device *dev = nullptr;
    ID3D11VideoDevice *vdev = nullptr;
    AVBufferRef *ff = nullptr;
    std::string adapter;
} g_d11;

using CreateDevice11Fn = HRESULT(WINAPI *)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                           ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
using CreateFactory1Fn = HRESULT(WINAPI *)(REFIID, void **);

bool MakeD11(IDirect3DDevice9 *game9) {
    if (g_d11.tried) return g_d11.ff != nullptr;
    g_d11.tried = true;
    HMODULE d11 = SystemDll(L"d3d11.dll"), dx = SystemDll(L"dxgi.dll");
    auto create = d11 ? reinterpret_cast<CreateDevice11Fn>(GetProcAddress(d11, "D3D11CreateDevice")) : nullptr;
    auto factory1 = dx ? reinterpret_cast<CreateFactory1Fn>(GetProcAddress(dx, "CreateDXGIFactory1")) : nullptr;
    if (!create || !factory1) { g_d11.why = "Windows' d3d11.dll or dxgi.dll not found"; return false; }
    LUID luid{};
    UINT ordinal = 0;
    bool haveLuid = GameLuid(game9, &luid, &ordinal);
    IDXGIFactory1 *f = nullptr;
    IDXGIAdapter1 *adapter = nullptr;
    if (SUCCEEDED(factory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&f)))) {
        for (UINT i = 0; f->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(adapter->GetDesc1(&d)) && (!haveLuid || (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart))) {
                char name[128];
                WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
                g_d11.adapter = name;
                break;
            }
            SafeRelease(adapter);
        }
        SafeRelease(f);
    }
    if (!adapter) { g_d11.why = "no DXGI adapter for the game's"; return false; }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                        D3D_FEATURE_LEVEL_9_3};
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 5, D3D11_SDK_VERSION,
                        &g_d11.dev, nullptr, &ctx);
    if (hr == E_INVALIDARG)
        hr = create(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels + 1, 4, D3D11_SDK_VERSION,
                    &g_d11.dev, nullptr, &ctx);
    SafeRelease(adapter);
    if (FAILED(hr)) { g_d11.why = Fmt("D3D11CreateDevice with video failed (0x%08lX)", hr); return false; }
    ID3D10Multithread *mt = nullptr;
    if (SUCCEEDED(g_d11.dev->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void **>(&mt)))) {
        mt->SetMultithreadProtected(TRUE);
        mt->Release();
    }
    if (FAILED(g_d11.dev->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void **>(&g_d11.vdev)))) {
        SafeRelease(ctx);
        g_d11.why = "the Direct3D 11 device has no video interface";
        return false;
    }
    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    auto *h = reinterpret_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(ref->data)->hwctx);
    h->device = g_d11.dev;
    g_d11.dev->AddRef();
    h->device_context = ctx;   // (FFmpeg's reference now; its own lock guards the context: Lock below)
    int r = av_hwdevice_ctx_init(ref);
    if (r < 0) { av_buffer_unref(&ref); g_d11.why = "FFmpeg refused the device"; return false; }
    g_d11.ff = ref;
    Log(MML_LOG_INFO, "video", "D3D11VA: a Direct3D 11 video device on %s", g_d11.adapter.c_str());
    return true;
}

AVD3D11VADeviceContext *D11Ctx() {
    return reinterpret_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(g_d11.ff->data)->hwctx);
}
struct D11Lock {   // FFmpeg's own lock of the immediate context (its decoding uses the context under it)
    AVD3D11VADeviceContext *h;
    D11Lock() : h(D11Ctx()) { h->lock(h->lock_ctx); }
    ~D11Lock() { h->unlock(h->lock_ctx); }
};

// ---- the loader's Direct3D 9Ex device for DXVA2 and its FFmpeg device (made once)
struct D9 {
    bool tried = false;
    std::string why;
    IDirect3DDevice9Ex *dev = nullptr;      // the device DXVA2 is given (the real one under dxwrapper's)
    IDirect3DDevice9Ex *wrapper = nullptr;  // dxwrapper's, kept alive while its real device is used
    IDirect3DDeviceManager9 *mgr = nullptr;
    IDirectXVideoDecoderService *svc = nullptr;
    HANDLE devHandle = nullptr;
    AVBufferRef *ff = nullptr;
} g_d9;

using Create9ExFn = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);
using CreateManagerFn = HRESULT(WINAPI *)(UINT *, IDirect3DDeviceManager9 **);

bool MakeD9(IDirect3DDevice9 *game9) {
    if (g_d9.tried) return g_d9.ff != nullptr;
    g_d9.tried = true;
    HMODULE d9 = SystemDll(L"d3d9.dll"), dxva = SystemDll(L"dxva2.dll");
    auto create = d9 ? reinterpret_cast<Create9ExFn>(GetProcAddress(d9, "Direct3DCreate9Ex")) : nullptr;
    auto manager = dxva ? reinterpret_cast<CreateManagerFn>(GetProcAddress(dxva, "DXVA2CreateDirect3DDeviceManager9")) : nullptr;
    if (!create || !manager) { g_d9.why = "Windows' d3d9.dll or dxva2.dll not found"; return false; }
    IDirect3D9Ex *d3d = nullptr;
    if (FAILED(create(D3D_SDK_VERSION, &d3d)) || !d3d) { g_d9.why = "Direct3DCreate9Ex failed"; return false; }
    LUID luid{};
    UINT ordinal = 0, use = D3DADAPTER_DEFAULT;
    if (GameLuid(game9, &luid, &ordinal))
        for (UINT i = 0; i < d3d->GetAdapterCount(); i++) {
            LUID l{};
            if (SUCCEEDED(d3d->GetAdapterLUID(i, &l)) && l.LowPart == luid.LowPart && l.HighPart == luid.HighPart) { use = i; break; }
        }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.BackBufferWidth = 64;
    pp.BackBufferHeight = 64;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = GetDesktopWindow();
    HRESULT hr = d3d->CreateDeviceEx(use, D3DDEVTYPE_HAL, GetDesktopWindow(),
                                     D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED | D3DCREATE_FPU_PRESERVE, &pp,
                                     nullptr, &g_d9.dev);
    SafeRelease(d3d);
    if (FAILED(hr)) { g_d9.why = Fmt("its Direct3D 9Ex device could not be made (0x%08lX)", hr); return false; }
    // dxwrapper (D3d9to9Ex) wraps every Direct3D 9 device of the process, System32's too, and DXVA2 cannot use its
    // wrapper (GetDecoderDeviceGuids answered E_FAIL): the real device under it, by dxwrapper's own IID
    // (IClassFactory.h IID_GetRealInterface; given without a reference: the wrapper is kept)
    static const GUID kRealInterface = {0x00000000, 0x1c77, 0x4d40, {0xb0, 0xcf, 0x98, 0xfe, 0xfd, 0xff, 0xff, 0xff}};
    IDirect3DDevice9Ex *real = nullptr;
    if (SUCCEEDED(g_d9.dev->QueryInterface(kRealInterface, reinterpret_cast<void **>(&real))) && real && real != g_d9.dev) {
        g_d9.wrapper = g_d9.dev;
        g_d9.dev = real;
    }
    UINT token = 0;
    if (FAILED(manager(&token, &g_d9.mgr)) || FAILED(g_d9.mgr->ResetDevice(g_d9.dev, token)) ||
        FAILED(g_d9.mgr->OpenDeviceHandle(&g_d9.devHandle)) ||
        FAILED(g_d9.mgr->GetVideoService(g_d9.devHandle, __uuidof(IDirectXVideoDecoderService), reinterpret_cast<void **>(&g_d9.svc)))) {
        g_d9.why = "no DXVA2 decoder service";
        return false;
    }
    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_DXVA2);
    auto *h = reinterpret_cast<AVDXVA2DeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(ref->data)->hwctx);
    h->devmgr = g_d9.mgr;
    g_d9.mgr->AddRef();
    if (av_hwdevice_ctx_init(ref) < 0) { av_buffer_unref(&ref); g_d9.why = "FFmpeg refused the device manager"; return false; }
    g_d9.ff = ref;
    Log(MML_LOG_INFO, "video", "DXVA2: a Direct3D 9Ex video device (adapter %u%s)", use,
        g_d9.wrapper ? "; the real device under dxwrapper's" : "");
    return true;
}

// ---- which decoder profile a stream needs
struct Need { GUID guid; bool tenBit; const char *name; };
bool NeedOf(const AVCodecParameters *par, Need *n) {
    const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format));
    int depth = pd ? pd->comp[0].depth : 8;
    n->tenBit = depth > 8;
    if (depth > 10 || (pd && (pd->log2_chroma_w != 1 || pd->log2_chroma_h != 1))) return false;   // 4:2:0 at 8 or 10 bits only
    switch (par->codec_id) {
    case AV_CODEC_ID_H264:
        if (n->tenBit) return false;
        n->guid = D3D11_DECODER_PROFILE_H264_VLD_NOFGT, n->name = "H.264";
        return true;
    case AV_CODEC_ID_HEVC:
        n->guid = n->tenBit ? D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10 : D3D11_DECODER_PROFILE_HEVC_VLD_MAIN;
        n->name = n->tenBit ? "HEVC Main 10" : "HEVC Main";
        return true;
    case AV_CODEC_ID_VP9:
        n->guid = n->tenBit ? D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2 : D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0;
        n->name = n->tenBit ? "VP9 profile 2" : "VP9 profile 0";
        return true;
    case AV_CODEC_ID_AV1:
        n->guid = D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, n->name = n->tenBit ? "AV1 Main (10-bit)" : "AV1 Main";
        return true;
    case AV_CODEC_ID_MPEG2VIDEO:
        n->guid = D3D11_DECODER_PROFILE_MPEG2and1_VLD, n->name = "MPEG-2";
        return !n->tenBit;
    case AV_CODEC_ID_VC1: case AV_CODEC_ID_WMV3:
        n->guid = D3D11_DECODER_PROFILE_VC1_D2010, n->name = "VC-1";
        return !n->tenBit;
    default:
        return false;
    }
}

bool D11Decodes(const Need &n, int w, int h, std::string *why) {
    D3D11_VIDEO_DECODER_DESC d{n.guid, static_cast<UINT>(w), static_cast<UINT>(h), n.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12};
    UINT count = 0;
    if (FAILED(g_d11.vdev->GetVideoDecoderConfigCount(&d, &count)) || !count) {
        *why = Fmt("the GPU has no %s decoder for %dx%d (D3D11VA)", n.name, w, h);
        return false;
    }
    return true;
}

bool D9Decodes(const Need &n, int w, int h, std::string *why) {
    UINT count = 0;
    GUID *guids = nullptr;
    bool has = false;
    HRESULT hr = g_d9.svc->GetDecoderDeviceGuids(&count, &guids);
    if (SUCCEEDED(hr)) {
        for (UINT i = 0; i < count; i++) has |= IsEqualGUID(guids[i], n.guid) != 0;
        CoTaskMemFree(guids);
    }
    if (!has) { *why = Fmt("the GPU has no %s decoder (DXVA2: %u decoders, 0x%08lX)", n.name, count, hr); return false; }
    D3DFORMAT want = n.tenBit ? static_cast<D3DFORMAT>(MAKEFOURCC('P', '0', '1', '0')) : static_cast<D3DFORMAT>(MAKEFOURCC('N', 'V', '1', '2'));
    D3DFORMAT *fmts = nullptr;
    bool fmt = false;
    if (SUCCEEDED(g_d9.svc->GetDecoderRenderTargets(n.guid, &count, &fmts))) {
        for (UINT i = 0; i < count; i++) fmt |= fmts[i] == want;
        CoTaskMemFree(fmts);
    }
    DXVA2_VideoDesc vd{};
    vd.SampleWidth = w;
    vd.SampleHeight = h;
    vd.Format = want;
    DXVA2_ConfigPictureDecode *cfg = nullptr;
    UINT ncfg = 0;
    if (fmt && SUCCEEDED(g_d9.svc->GetDecoderConfigurations(n.guid, &vd, nullptr, &ncfg, &cfg))) CoTaskMemFree(cfg);
    if (!fmt || !ncfg) { *why = Fmt("the GPU's %s decoder has no configuration for %dx%d (DXVA2)", n.name, w, h); return false; }
    return true;
}

} // namespace

const char *ApiName(Api api) { return api == kD3D11 ? "D3D11VA" : api == kDxva2 ? "DXVA2" : "software"; }
int PixelFormat(Api api) { return api == kD3D11 ? AV_PIX_FMT_D3D11 : api == kDxva2 ? AV_PIX_FMT_DXVA2_VLD : -1; }

Api Choose(const AVCodecParameters *par, const std::string &force, AVBufferRef **device, std::string *why) {
    *device = nullptr;
    if (force == "software") { *why = "software asked for"; return kSoftware; }
    Need n{};
    if (!NeedOf(par, &n)) {
        const AVCodecDescriptor *d = avcodec_descriptor_get(par->codec_id);
        const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format));
        bool gpuCodec = par->codec_id == AV_CODEC_ID_H264 || par->codec_id == AV_CODEC_ID_HEVC || par->codec_id == AV_CODEC_ID_VP9 ||
                        par->codec_id == AV_CODEC_ID_AV1 || par->codec_id == AV_CODEC_ID_MPEG2VIDEO || par->codec_id == AV_CODEC_ID_VC1 ||
                        par->codec_id == AV_CODEC_ID_WMV3;
        *why = gpuCodec ? Fmt("GPUs decode %s at 4:2:0 and %s bits only (this is %s)", d ? d->long_name : "it",
                              par->codec_id == AV_CODEC_ID_HEVC || par->codec_id == AV_CODEC_ID_VP9 || par->codec_id == AV_CODEC_ID_AV1 ? "8 or 10" : "8",
                              pd ? pd->name : "?")
                        : std::string("GPUs have no ") + (d ? d->long_name : "such") + " decoder";
        return kSoftware;
    }
    IDirect3DDevice9 *game9 = GameDevice9();
    std::string why11, why9;
    Api got = kSoftware;
    if (force != "dxva2") {
        if (!MakeD11(game9)) why11 = "D3D11VA: " + g_d11.why;
        else if (D11Decodes(n, par->width, par->height, &why11)) got = kD3D11;
    }
    if (got == kSoftware && force != "d3d11va") {
        if (!MakeD9(game9)) why9 = "DXVA2: " + g_d9.why;
        else if (D9Decodes(n, par->width, par->height, &why9)) got = kDxva2;
    }
    if (game9) game9->Release();
    if (got == kD3D11) *device = av_buffer_ref(g_d11.ff);
    if (got == kDxva2) *device = av_buffer_ref(g_d9.ff);
    *why = why11 + (!why11.empty() && !why9.empty() ? "; " : "") + why9;
    return got;
}

// ---- one video's slots and converters
struct Slot {
    IDirect3DTexture9 *tex9 = nullptr;      // the game's (shared, or the copy's upload target is video.cpp's own)
    HANDLE handle = nullptr;
    IDirect3DQuery9 *drawn = nullptr;       // issued on the game's device after its draw
    bool pending = false;
    // D3D11
    ID3D11Texture2D *tex11 = nullptr;
    ID3D11VideoProcessorOutputView *ov = nullptr;
    // DXVA2
    IDirect3DTexture9 *texOurs = nullptr;
    IDirect3DSurface9 *surfOurs = nullptr;
};

struct Session {
    Api api = kSoftware;
    bool shared = false;
    int w = 0, h = 0;
    IDirect3DDevice9 *game9 = nullptr;
    std::vector<Slot> slots;
    // D3D11
    ID3D11VideoContext *vctx = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11VideoProcessorEnumerator *vpe = nullptr;
    ID3D11VideoProcessor *vp = nullptr;
    int vpW = 0, vpH = 0, vpFmt = 0;
    ID3D11Query *done11 = nullptr;
    ID3D11Texture2D *copyRt = nullptr, *staging = nullptr;   // the copy path's
    ID3D11VideoProcessorOutputView *copyOv = nullptr;
    // DXVA2
    IDirect3DQuery9 *done9 = nullptr;
    IDirect3DSurface9 *copyRt9 = nullptr, *sys9 = nullptr;
    IDirectXVideoProcessor *proc9 = nullptr;   // DXVA2's video processor (made at the first picture; none: StretchRect)
    bool proc9Tried = false;
    int proc9Fmt = 0;
};

namespace {

// The shared slots: textures of the game's device with a shared handle, opened by the loader's. False: the game's
// device cannot share (the copy path then).
bool MakeShared(Session *s, std::string *why) {
    for (Slot &sl : s->slots) {
        HRESULT hr = s->game9->CreateTexture(s->w, s->h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &sl.tex9, &sl.handle);
        if (FAILED(hr) || !sl.handle) { *why = Fmt("the game's device made no shared texture (0x%08lX)", hr); return false; }
        if (s->api == kD3D11) {
            hr = g_d11.dev->OpenSharedResource(sl.handle, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&sl.tex11));
            if (FAILED(hr)) { *why = Fmt("Direct3D 11 could not open the game's shared texture (0x%08lX)", hr); return false; }
        } else {
            HANDLE h = sl.handle;
            hr = g_d9.dev->CreateTexture(s->w, s->h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &sl.texOurs, &h);
            if (FAILED(hr) || FAILED(sl.texOurs->GetSurfaceLevel(0, &sl.surfOurs))) {
                *why = Fmt("the DXVA2 device could not open the game's shared texture (0x%08lX)", hr);
                return false;
            }
        }
        if (FAILED(s->game9->CreateQuery(D3DQUERYTYPE_EVENT, &sl.drawn))) { *why = "no event query on the game's device"; return false; }
    }
    return true;
}

void FreeSlots(Session *s) {
    for (Slot &sl : s->slots) {
        SafeRelease(sl.ov);
        SafeRelease(sl.tex11);
        SafeRelease(sl.surfOurs);
        SafeRelease(sl.texOurs);
        SafeRelease(sl.drawn);
        SafeRelease(sl.tex9);
        sl = Slot{};
    }
}

bool MakeCopy(Session *s, std::string *why) {
    if (s->api == kD3D11) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = s->w;
        d.Height = s->h;
        d.MipLevels = d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_d11.dev->CreateTexture2D(&d, nullptr, &s->copyRt))) { *why = "no Direct3D 11 target for the copy"; return false; }
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_d11.dev->CreateTexture2D(&d, nullptr, &s->staging))) { *why = "no Direct3D 11 staging texture"; return false; }
        return true;
    }
    if (FAILED(g_d9.dev->CreateRenderTarget(s->w, s->h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s->copyRt9, nullptr)) ||
        FAILED(g_d9.dev->CreateOffscreenPlainSurface(s->w, s->h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &s->sys9, nullptr))) {
        *why = "no DXVA2 target for the copy";
        return false;
    }
    return true;
}

// The video processor for this input (made at the first picture, again if the input changes size or format)
bool EnsureVp(Session *s, int inW, int inH, DXGI_FORMAT fmt, std::string *why) {
    if (s->vp && s->vpW == inW && s->vpH == inH && s->vpFmt == fmt) return true;
    for (Slot &sl : s->slots) SafeRelease(sl.ov);
    SafeRelease(s->copyOv);
    SafeRelease(s->vp);
    SafeRelease(s->vpe);
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = inW;
    cd.InputHeight = inH;
    cd.OutputWidth = s->w;
    cd.OutputHeight = s->h;
    cd.InputFrameRate = {60, 1};
    cd.OutputFrameRate = {60, 1};
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(g_d11.vdev->CreateVideoProcessorEnumerator(&cd, &s->vpe))) { *why = "no video processor enumerator"; return false; }
    UINT support = 0;
    if (FAILED(s->vpe->CheckVideoProcessorFormat(fmt, &support)) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        *why = Fmt("the video processor does not take format %d", static_cast<int>(fmt));
        return false;
    }
    if (FAILED(s->vpe->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support)) || !(support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        *why = "the video processor does not give BGRA";
        return false;
    }
    if (FAILED(g_d11.vdev->CreateVideoProcessor(s->vpe, 0, &s->vp))) { *why = "no video processor"; return false; }
    s->vpW = inW, s->vpH = inH, s->vpFmt = fmt;
    s->vctx->VideoProcessorSetStreamAutoProcessingMode(s->vp, 0, FALSE);
    s->vctx->VideoProcessorSetStreamFrameFormat(s->vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
    out.RGB_Range = 0;   // full range RGB, as the game's back buffer
    s->vctx->VideoProcessorSetOutputColorSpace(s->vp, &out);
    return true;
}

ID3D11VideoProcessorOutputView *OutputView(Session *s, ID3D11Texture2D *tex, ID3D11VideoProcessorOutputView **cache) {
    if (*cache) return *cache;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
    d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(g_d11.vdev->CreateVideoProcessorOutputView(tex, s->vpe, &d, cache))) return nullptr;
    return *cache;
}

// Waits for the loader's device to have done its work (an event query; FFmpeg's lock taken only to ask)
void WaitD11(Session *s) {
    {
        D11Lock lk;
        s->ctx->End(s->done11);
        s->ctx->Flush();
    }
    for (int i = 0; i < 2000; i++) {
        HRESULT hr;
        {
            D11Lock lk;
            hr = s->ctx->GetData(s->done11, nullptr, 0, 0);
        }
        if (hr != S_FALSE) return;
        if (i > 20) Sleep(0); else YieldProcessor();
    }
}

bool ConvertD11(Session *s, const AVFrame *f, int slot, std::vector<uint8_t> *bgra, std::string *why) {
    auto *tex = reinterpret_cast<ID3D11Texture2D *>(f->data[0]);
    UINT index = static_cast<UINT>(reinterpret_cast<intptr_t>(f->data[1]));
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    {
        D11Lock lk;
        if (!EnsureVp(s, f->width, f->height, td.Format, why)) return false;
        ID3D11VideoProcessorOutputView *ov = s->shared ? OutputView(s, s->slots[slot].tex11, &s->slots[slot].ov)
                                                       : OutputView(s, s->copyRt, &s->copyOv);
        if (!ov) { *why = "no video processor output view"; return false; }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        iv.Texture2D.ArraySlice = index;
        ID3D11VideoProcessorInputView *in = nullptr;
        if (FAILED(g_d11.vdev->CreateVideoProcessorInputView(tex, s->vpe, &iv, &in))) { *why = "no video processor input view"; return false; }
        // the stream's colour: BT.709 for HD (or when it says so), BT.601 otherwise; its range
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
        cs.YCbCr_Matrix = (f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720) ||
                           f->colorspace == AVCOL_SPC_BT2020_NCL) ? 1 : 0;
        cs.Nominal_Range = f->color_range == AVCOL_RANGE_JPEG ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        s->vctx->VideoProcessorSetStreamColorSpace(s->vp, 0, &cs);
        RECT src{static_cast<LONG>(f->crop_left), static_cast<LONG>(f->crop_top), static_cast<LONG>(f->crop_left + f->width),
                 static_cast<LONG>(f->crop_top + f->height)};
        s->vctx->VideoProcessorSetStreamSourceRect(s->vp, 0, TRUE, &src);
        RECT dst{0, 0, s->w, s->h};
        s->vctx->VideoProcessorSetStreamDestRect(s->vp, 0, TRUE, &dst);
        s->vctx->VideoProcessorSetOutputTargetRect(s->vp, TRUE, &dst);
        D3D11_VIDEO_PROCESSOR_STREAM st{};
        st.Enable = TRUE;
        st.pInputSurface = in;
        HRESULT hr = s->vctx->VideoProcessorBlt(s->vp, ov, 0, 1, &st);
        in->Release();
        if (FAILED(hr)) { *why = Fmt("VideoProcessorBlt failed (0x%08lX)", hr); return false; }
        if (!s->shared) s->ctx->CopyResource(s->staging, s->copyRt);
    }
    if (s->shared) { WaitD11(s); return true; }
    D11Lock lk;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(s->ctx->Map(s->staging, 0, D3D11_MAP_READ, 0, &m))) { *why = "the staging texture could not be read"; return false; }
    bgra->resize(static_cast<size_t>(s->w) * s->h * 4);
    for (int y = 0; y < s->h; y++)
        memcpy(bgra->data() + static_cast<size_t>(s->w) * 4 * y, static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(m.RowPitch) * y,
               static_cast<size_t>(s->w) * 4);
    s->ctx->Unmap(s->staging, 0);
    return true;
}

// DXVA2's video processor for the decoder's surface format (the colour matrix and range said, as D3D11's): made once;
// where the driver has none, StretchRect (the driver's own YUV to RGB: seen with a studio range left unexpanded).
IDirectXVideoProcessor *Proc9(Session *s, D3DFORMAT fmt, int w, int h) {
    if (s->proc9Tried && s->proc9Fmt == static_cast<int>(fmt)) return s->proc9;
    SafeRelease(s->proc9);
    s->proc9Tried = true;
    s->proc9Fmt = static_cast<int>(fmt);
    IDirectXVideoProcessorService *svc = nullptr;
    if (FAILED(g_d9.mgr->GetVideoService(g_d9.devHandle, __uuidof(IDirectXVideoProcessorService), reinterpret_cast<void **>(&svc)))) return nullptr;
    DXVA2_VideoDesc vd{};
    vd.SampleWidth = w;
    vd.SampleHeight = h;
    vd.Format = fmt;
    vd.SampleFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
    vd.InputSampleFreq = {60, 1};
    vd.OutputFrameFreq = {60, 1};
    if (FAILED(svc->CreateVideoProcessor(DXVA2_VideoProcProgressiveDevice, &vd, D3DFMT_X8R8G8B8, 0, &s->proc9))) s->proc9 = nullptr;
    svc->Release();
    return s->proc9;
}

bool ConvertD9(Session *s, const AVFrame *f, int slot, std::vector<uint8_t> *bgra, std::string *why) {
    auto *surf = reinterpret_cast<IDirect3DSurface9 *>(f->data[3]);
    RECT src{static_cast<LONG>(f->crop_left), static_cast<LONG>(f->crop_top), static_cast<LONG>(f->crop_left + f->width),
             static_cast<LONG>(f->crop_top + f->height)};
    IDirect3DSurface9 *dst = s->shared ? s->slots[slot].surfOurs : s->copyRt9;
    D3DSURFACE_DESC sd{};
    surf->GetDesc(&sd);
    HRESULT hr;
    if (IDirectXVideoProcessor *proc = Proc9(s, sd.Format, f->width, f->height)) {
        DXVA2_VideoSample sample{};
        sample.SrcSurface = surf;
        sample.SrcRect = src;
        sample.DstRect = {0, 0, s->w, s->h};
        sample.SampleFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
        sample.SampleFormat.VideoTransferMatrix = (f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720) ||
                                                   f->colorspace == AVCOL_SPC_BT2020_NCL) ? DXVA2_VideoTransferMatrix_BT709 : DXVA2_VideoTransferMatrix_BT601;
        sample.SampleFormat.NominalRange = f->color_range == AVCOL_RANGE_JPEG ? DXVA2_NominalRange_0_255 : DXVA2_NominalRange_16_235;
        sample.PlanarAlpha = DXVA2_Fixed32OpaqueAlpha();
        DXVA2_VideoProcessBltParams bp{};
        bp.TargetRect = sample.DstRect;
        bp.DestFormat.SampleFormat = DXVA2_SampleProgressiveFrame;
        bp.DestFormat.NominalRange = DXVA2_NominalRange_0_255;
        bp.DestFormat.VideoTransferMatrix = sample.SampleFormat.VideoTransferMatrix;
        bp.Alpha = DXVA2_Fixed32OpaqueAlpha();
        bp.ProcAmpValues.Contrast.Value = 1;     // (the processor's defaults: contrast and saturation 1, brightness and hue 0;
        bp.ProcAmpValues.Saturation.Value = 1;   // zeros are refused, E_INVALIDARG)
        DXVA2_AYUVSample16 black{0x8000, 0x8000, 0x1000, 0xFFFF};
        bp.BackgroundColor = black;
        hr = proc->VideoProcessBlt(dst, &bp, &sample, 1, nullptr);
        if (FAILED(hr)) { *why = Fmt("DXVA2's VideoProcessBlt failed (0x%08lX)", hr); return false; }
    } else {
        hr = g_d9.dev->StretchRect(surf, &src, dst, nullptr, D3DTEXF_LINEAR);
        if (FAILED(hr)) { *why = Fmt("StretchRect from the decoder's surface failed (0x%08lX)", hr); return false; }
    }
    if (s->shared) {
        s->done9->Issue(D3DISSUE_END);
        for (int i = 0; i < 2000 && s->done9->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE; i++)
            if (i > 20) Sleep(0); else YieldProcessor();
        return true;
    }
    if (FAILED(g_d9.dev->GetRenderTargetData(s->copyRt9, s->sys9))) { *why = "GetRenderTargetData failed"; return false; }
    D3DLOCKED_RECT lr{};
    if (FAILED(s->sys9->LockRect(&lr, nullptr, D3DLOCK_READONLY))) { *why = "the copy could not be read"; return false; }
    bgra->resize(static_cast<size_t>(s->w) * s->h * 4);
    for (int y = 0; y < s->h; y++)
        memcpy(bgra->data() + static_cast<size_t>(s->w) * 4 * y, static_cast<const uint8_t *>(lr.pBits) + static_cast<size_t>(lr.Pitch) * y,
               static_cast<size_t>(s->w) * 4);
    s->sys9->UnlockRect();
    return true;
}

} // namespace

Session *Begin(Api api, IDirect3DDevice9 *game9, int w, int h, int slots, std::string *why) {
    if (api == kSoftware || !game9) return nullptr;
    Session *s = new Session;
    s->api = api;
    s->game9 = game9;
    game9->AddRef();
    s->w = w;
    s->h = h;
    s->slots.resize(slots);
    std::string whyShared;
    if (api == kD3D11) {
        D11Ctx()->device_context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void **>(&s->vctx));
        s->ctx = D11Ctx()->device_context;
        s->ctx->AddRef();
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
        if (!s->vctx || FAILED(g_d11.dev->CreateQuery(&qd, &s->done11))) { *why = "no video context"; End(s); return nullptr; }
    } else if (FAILED(g_d9.dev->CreateQuery(D3DQUERYTYPE_EVENT, &s->done9))) {
        *why = "no event query on the DXVA2 device";
        End(s);
        return nullptr;
    }
    s->shared = MakeShared(s, &whyShared);
    if (!s->shared) {
        FreeSlots(s);
        if (!MakeCopy(s, why)) { End(s); return nullptr; }
        Log(MML_LOG_INFO, "video", "%s: the pictures are copied through the CPU (%s)", ApiName(api), whyShared.c_str());
    }
    return s;
}

bool Shared(const Session *s) { return s && s->shared; }
std::string Path(const Session *s) {
    return s ? std::string(ApiName(s->api)) + (s->shared ? ", a shared texture" : ", copied through the CPU") : "software";
}

bool Convert(Session *s, const AVFrame *frame, int slot, std::vector<uint8_t> *bgra, std::string *why) {
    if (!s) { *why = "no GPU session"; return false; }
    if (frame->format == AV_PIX_FMT_D3D11 && s->api == kD3D11) return ConvertD11(s, frame, slot, bgra, why);
    if (frame->format == AV_PIX_FMT_DXVA2_VLD && s->api == kDxva2) return ConvertD9(s, frame, slot, bgra, why);
    *why = "not a GPU picture of this session";
    return false;
}

IDirect3DTexture9 *SlotTexture(Session *s, int slot) { return s && s->shared ? s->slots[slot].tex9 : nullptr; }

void Drawn(Session *s, int slot) {
    if (!s || !s->shared) return;
    s->slots[slot].drawn->Issue(D3DISSUE_END);
    s->slots[slot].pending = true;
}

bool Retired(Session *s, int slot) {
    if (!s || !s->shared) return true;
    Slot &sl = s->slots[slot];
    if (!sl.pending) return true;
    if (sl.drawn->GetData(nullptr, 0, 0) == S_FALSE) return false;
    sl.pending = false;
    return true;
}

void End(Session *s) {
    if (!s) return;
    if (s->api == kD3D11 && g_d11.ff) {
        D11Lock lk;
        FreeSlots(s);
        SafeRelease(s->copyOv);
        SafeRelease(s->vp);
        SafeRelease(s->vpe);
        SafeRelease(s->done11);
        SafeRelease(s->copyRt);
        SafeRelease(s->staging);
        SafeRelease(s->vctx);
        SafeRelease(s->ctx);
    } else {
        FreeSlots(s);
        SafeRelease(s->proc9);
        SafeRelease(s->done9);
        SafeRelease(s->copyRt9);
        SafeRelease(s->sys9);
    }
    SafeRelease(s->game9);
    delete s;
}

} // namespace videohw
} // namespace host
