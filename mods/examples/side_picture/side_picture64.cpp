// side_picture's 64-bit half (sdk/include/mml/side.h; the 32-bit half: side_picture.cpp; docs/MODDING.md "64-bit
// halves"). It draws a picture with Direct3D 12 into two shared textures, in turn, one a frame, and tells the 32-bit
// half which one is whole: how a 64-bit simulation hands the game a frame's result.
//
// Each FRAME event wakes its worker thread (the sidecar's mod thread makes the textures and passes the word on: a
// half's heavy work is on threads of its own; the host's functions but `call` and the channels' are the mod thread's). The worker reads the channel "ask" (the frame number and the time the 32-bit half wrote at
// that frame's start), clears the next texture to a colour of that frame with a bar at a place of it, waits for the
// GPU, and writes the channel "done" (the frame, the ask's time, the texture, the time it finished). The 32-bit half
// measures from them how many frames late the picture it draws is and how long the round trip took.
//
// The textures can be made once the game's adapter is known (the graphics bridge on: a mod with "gfx": true on at the
// start, this mod itself); until then each frame tries again.
//   call side_picture.side -> {"drawn", "made", "gpu_ms", "error"}: what this half has done
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include "mml/side.h"

namespace {
const MmlSideHost *H;
MmlSideMod *M;

// the channels' records, the same in both halves (side_picture.cpp)
struct Ask { uint64_t frame; int64_t qpc; };
struct Done { uint64_t frame; int64_t askQpc; uint32_t texture; uint32_t pad; int64_t doneQpc; };
MmlSideChannel *g_ask, *g_done;

const uint32_t kW = 256, kH = 144;
const char *kNames[2] = {"pic0", "pic1"};
ID3D12Resource *g_tex[2];      // the sidecar's (texture_remove or the unload releases them)
ID3D12CommandQueue *g_queue;
ID3D12CommandAllocator *g_alloc;
ID3D12GraphicsCommandList *g_list;
ID3D12DescriptorHeap *g_rtvHeap;
UINT g_rtvStep;
ID3D12Fence *g_fence;
HANDLE g_fenceEvent, g_wake;
uint64_t g_fenceValue;

std::thread g_worker;
std::atomic<bool> g_stop;
std::atomic<uint64_t> g_drawn;
std::atomic<double> g_gpuMs;
std::atomic<bool> g_made;
std::mutex g_errMx;
std::string g_error;

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }
void SetError(const std::string &e) { std::lock_guard<std::mutex> lk(g_errMx); g_error = e; }

// The textures and what draws into them, on the sidecar's Direct3D 12 device (the game's adapter). false: not yet.
bool Make() {
    auto *dev = static_cast<ID3D12Device *>(H->texture_d3d12_device());
    if (!dev) { SetError("the game's adapter is not known yet (no graphics bridge)"); return false; }
    for (int i = 0; i < 2; i++) {
        MmlSideTexture t{};
        t.size = sizeof t;
        if (!H->texture_create(M, kNames[i], kW, kH, 28 /* DXGI_FORMAT_R8G8B8A8_UNORM */, &t)) { SetError("texture_create failed"); return false; }
        g_tex[i] = static_cast<ID3D12Resource *>(t.d3d12_resource);
    }
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2};
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_queue))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list))) ||
        FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap))) || FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        SetError("the Direct3D 12 objects were not made");
        return false;
    }
    g_list->Close();
    g_rtvStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (int i = 0; i < 2; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += i * g_rtvStep;
        dev->CreateRenderTargetView(g_tex[i], nullptr, h);
    }
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SetError("");
    return true;
}

void Release() {
    SafeRelease(g_list);
    SafeRelease(g_alloc);
    SafeRelease(g_queue);
    SafeRelease(g_rtvHeap);
    SafeRelease(g_fence);
    if (g_fenceEvent) CloseHandle(g_fenceEvent);
    g_fenceEvent = nullptr;
    for (int i = 0; i < 2; i++) {
        if (g_tex[i]) H->texture_remove(M, kNames[i]);
        g_tex[i] = nullptr;
    }
    g_made = false;
}

// One picture for the frame asked: a colour that turns with the frames, a bar that crosses the picture.
void Draw(const Ask &a, uint32_t which) {
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    g_alloc->Reset();
    g_list->Reset(g_alloc, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += which * g_rtvStep;
    float hue = static_cast<float>(a.frame % 360) / 360.0f * 6.2831853f;
    const float back[4] = {0.5f + 0.4f * sinf(hue), 0.5f + 0.4f * sinf(hue + 2.094f), 0.5f + 0.4f * sinf(hue + 4.189f), 1.0f};
    const float bar[4] = {1, 1, 1, 1};
    g_list->ClearRenderTargetView(rtv, back, 0, nullptr);   // (simultaneous access: promoted from COMMON, no barrier)
    LONG x = static_cast<LONG>((a.frame * 3) % kW);
    D3D12_RECT r{x, 0, x + 6 > static_cast<LONG>(kW) ? static_cast<LONG>(kW) : x + 6, static_cast<LONG>(kH)};
    g_list->ClearRenderTargetView(rtv, bar, 1, &r);
    g_list->Close();
    ID3D12CommandList *lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, ++g_fenceValue);
    if (g_fence->GetCompletedValue() < g_fenceValue) {
        g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 1000);
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    g_gpuMs = 1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart;
    Done d{a.frame, a.qpc, which, 0, t1.QuadPart};
    H->channel_write(g_done, &d, sizeof d);   // (the texture is whole: its GPU work has ended)
    g_drawn++;
}

void Worker() {
    uint64_t seen = 0;
    uint32_t which = 0;
    while (!g_stop) {
        WaitForSingleObject(g_wake, 100);
        if (g_stop) break;
        if (!g_made) continue;
        Ask a{};
        uint64_t v = 0;
        if (H->channel_read(g_ask, &a, sizeof a, &v) != sizeof a || v == seen) continue;
        seen = v;
        which ^= 1;   // (the other one is the one the 32-bit half may be reading)
        Draw(a, which);
    }
}

void OnFrame(MmlSideMod *, const char *, const char *, void *) {
    static unsigned waited;
    if (!g_made && waited++ % 60 == 0) {   // (once a second at most: a failure is not retried every frame)
        g_made = Make();
        if (!g_made) Release();
    }
    if (g_made) SetEvent(g_wake);
}

int Side(MmlSideMod *, const char *, MmlSideReply *r, void *) {
    std::string e;
    {
        std::lock_guard<std::mutex> lk(g_errMx);
        e = g_error;
    }
    for (char &c : e) if (c == '"' || c == '\\') c = '\'';
    char b[512];
    snprintf(b, sizeof b, "{\"drawn\": %llu, \"made\": %s, \"gpu_ms\": %.3f, \"error\": \"%s\"}", static_cast<unsigned long long>(g_drawn.load()),
             g_made.load() ? "true" : "false", g_gpuMs.load(), e.c_str());
    H->reply_append(r, b, strlen(b));
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_side_api(void) { return MML_SIDE_API_VERSION; }

MML_EXPORT int mml_side_load(const MmlSideHost *host, MmlSideMod *mod) {
    H = host;
    M = mod;
    g_ask = H->channel_open(mod, "ask", sizeof(Ask));
    g_done = H->channel_open(mod, "done", sizeof(Done));
    if (!g_ask || !g_done) return 1;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stop = false;
    g_worker = std::thread(Worker);
    H->subscribe(mod, "FRAME", OnFrame, nullptr);
    H->command(mod, "side", "{} -> {drawn, made, gpu_ms, error}: what the 64-bit half has drawn", Side, nullptr);
    return 0;
}

MML_EXPORT void mml_side_unload(MmlSideMod *) {
    g_stop = true;
    SetEvent(g_wake);
    if (g_worker.joinable()) g_worker.join();
    Release();
    CloseHandle(g_wake);
    g_wake = nullptr;
}
