// mml-sidecar64.exe: the loader's 64-bit process beside the game (sdk/include/mml/side.h; docs/MODDING.md "64-bit
// halves").
//
// The loader starts it with `--game <pid>` when a mod has a 64-bit half (its manifest's "entry64"), in a job object
// that ends it with the game. It opens the shared memory proto.h describes, says it is ready, and then does what the
// loader asks: load and unload halves, hand them the game's events, answer the bridge's calls to their commands. A
// half's calls to the bridge go back the same way and wait for the answer.
//
// Threads: the reader takes every message from the game's ring; answers to calls go to the calls waiting for them,
// the rest to the mod thread (this process's main thread), which runs every callback of the halves, one at a time,
// each behind a fault guard: a half that faults is unloaded and the loader told. When the game's process ends, so
// does this one (also without the job: it waits on the game's handle).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/side.h"
#include "channel.h"
#include "ring.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using json = nlohmann::json;

struct MmlSideReply { std::string text; bool error = false; };
struct MmlSideChannel { side::Channel ch; };

struct Half;
struct MmlSideMod { Half *half; };

namespace {

// ---- the link to the game
uint32_t g_gamePid;
HANDLE g_game;
side::Section *g_sec;
side::RingEnd g_in, g_out;   // g_in: the game's messages to us; g_out: ours to it
std::mutex g_outMx;
std::string g_gameDir, g_mmlDir, g_build = "unknown";
FILE *g_logFile;

std::wstring Wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Utf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::string Dump(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

bool Send(uint32_t type, uint32_t aux, const json &j, uint32_t waitMs = 2000) {
    std::string s = j.is_null() ? std::string() : Dump(j);
    std::lock_guard<std::mutex> lk(g_outMx);
    return side::RingWrite(g_out, type, aux, s.data(), static_cast<uint32_t>(s.size()), waitMs);
}

// Logged to the game's log (by the loader) and to <mml>\sidecar.log (when the game cannot be told).
void Log(const std::string &mod, int level, const std::string &text) {
    if (g_logFile) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(g_logFile, "%02d:%02d:%02d.%03d %d [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, level, mod.c_str(), text.c_str());
        fflush(g_logFile);
    }
    if (g_sec) Send(side::kLog, 0, {{"mod", mod}, {"level", level}, {"text", text}}, 200);
}
void Logf(int level, const char *fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    Log("sidecar", level, b);
}

// ---- calls to the bridge, waiting for their answers
struct Pending { bool done = false; bool ok = false; std::string text; };
std::mutex g_callMx;
std::condition_variable g_callCv;
std::map<uint32_t, Pending *> g_pending;
std::atomic<uint32_t> g_seq{1};

// ---- what the reader hands the mod thread
struct Msg { uint32_t type, aux; std::string payload; };
std::mutex g_qMx;
std::deque<Msg> g_queue;
HANDLE g_qEvent;
std::atomic<bool> g_quit{false};

DWORD WINAPI Reader(void *) {
    for (;;) {
        uint32_t type = 0, aux = 0;
        std::string payload;
        bool any = false;
        while (side::RingRead(g_in, &type, &aux, &payload)) {
            any = true;
            if (type == side::kCallReply) {
                json j = json::parse(payload, nullptr, false);
                std::lock_guard<std::mutex> lk(g_callMx);
                auto it = g_pending.find(aux);
                if (it != g_pending.end()) {
                    it->second->ok = j.is_object() && j.value("ok", false);
                    it->second->text = !j.is_object() ? std::string("a broken answer")
                                       : it->second->ok ? (j.contains("result") ? (j["result"].is_string() ? j["result"].get<std::string>() : Dump(j["result"])) : std::string("null"))
                                                        : j.value("error", std::string("error"));
                    it->second->done = true;
                    g_callCv.notify_all();
                }
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(g_qMx);
                g_queue.push_back(Msg{type, aux, std::move(payload)});
            }
            SetEvent(g_qEvent);
            if (type == side::kQuit) return 0;
        }
        if (g_quit) return 0;
        if (!any) WaitForSingleObject(g_in.dataEvent, 100);
    }
}

// ---- the adapter and the shared textures
uint32_t g_luid[2];
bool g_haveLuid;
ID3D12Device *g_d3d12;
std::string g_d3d12Error;

ID3D12Device *D3d12() {
    if (g_d3d12 || !g_haveLuid) return g_d3d12;
    IDXGIFactory4 *fac = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) { g_d3d12Error = "no DXGI factory"; return nullptr; }
    LUID luid;
    luid.LowPart = g_luid[0];
    luid.HighPart = static_cast<LONG>(g_luid[1]);
    IDXGIAdapter1 *ad = nullptr;
    if (FAILED(fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad)))) {
        fac->Release();
        g_d3d12Error = "no adapter of the game's LUID";
        return nullptr;
    }
    HRESULT hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_d3d12));
    ad->Release();
    fac->Release();
    if (FAILED(hr)) {
        char b[64];
        snprintf(b, sizeof b, "D3D12CreateDevice failed (%08lX)", static_cast<unsigned long>(hr));
        g_d3d12Error = b;
        g_d3d12 = nullptr;
    }
    return g_d3d12;
}

// The formats Direct3D 11 opens from a Direct3D 12 shared texture (a render target with simultaneous access: measured
// on an RTX 3070; the rest are refused there).
bool FormatShareable(uint32_t f) {
    switch (f) {
    case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_R10G10B10A2_UNORM:
        return true;
    default:
        return false;
    }
}

struct Texture {
    std::string name;
    ID3D12Resource *res = nullptr;
    HANDLE handle = nullptr;   // ours
    uint32_t w = 0, h = 0, format = 0, generation = 0;
};

} // namespace

// ---- one load of a half
struct Half {
    std::string id, dir, entry, dataDir, liveDir;
    unsigned generation = 0;
    HMODULE dll = nullptr;
    MmlSideUnloadFn unload = nullptr;
    MmlSideMod mod{};
    bool alive = false, faulted = false;
    struct Sub { std::string type; MmlSideEventFn fn; void *user; };
    std::vector<Sub> subs;
    struct Cmd { MmlSideCommandFn fn; void *user; std::string help; };
    std::map<std::string, Cmd> commands;
    std::vector<Texture> textures;
    std::vector<MmlSideChannel *> channels;
    uint32_t textureGen = 0;
};

namespace {

std::map<std::string, std::unique_ptr<Half>> g_halves;   // by mod id: the live load of each

Half *Of(const MmlSideMod *m) { return m && m->half && m->half->alive ? m->half : nullptr; }

std::vector<std::string> SubTypes(const Half &h) {
    std::vector<std::string> t;
    for (const Half::Sub &s : h.subs) if (std::find(t.begin(), t.end(), s.type) == t.end()) t.push_back(s.type);
    return t;
}

// ---- the host table
const char *H_GameDir() { return g_gameDir.c_str(); }
const char *H_GameBuild() { return g_build.c_str(); }
const char *H_ModId(const MmlSideMod *m) { return m && m->half ? m->half->id.c_str() : ""; }
const char *H_ModDir(const MmlSideMod *m) { return m && m->half ? m->half->dir.c_str() : ""; }
const char *H_DataDir(const MmlSideMod *m) { return m && m->half ? m->half->dataDir.c_str() : ""; }
void H_Log(const MmlSideMod *m, int level, const char *fmt, ...) {
    char b[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    Log(m && m->half ? m->half->id : "?", level, b);
}

int H_Call(MmlSideMod *m, const char *method, const char *params, MmlSideReplyFn fn, void *user, uint32_t timeoutMs) {
    if (!method || !*method) { if (fn) fn(0, "no method", 8, user); return 0; }
    json p = params && *params ? json::parse(params, nullptr, false) : json::object();
    if (p.is_discarded()) { const char *e = "params: not JSON"; if (fn) fn(0, e, strlen(e), user); return 0; }
    if (!timeoutMs) timeoutMs = 5000;
    Pending pend;
    uint32_t seq = g_seq++;
    {
        std::lock_guard<std::mutex> lk(g_callMx);
        g_pending[seq] = &pend;
    }
    json req = {{"method", method}, {"params", p}, {"timeout_ms", timeoutMs}, {"mod", m && m->half ? m->half->id : ""}};
    bool sent = Send(side::kCall, seq, req, timeoutMs);
    std::unique_lock<std::mutex> lk(g_callMx);
    if (sent) g_callCv.wait_for(lk, std::chrono::milliseconds(timeoutMs + 1000), [&] { return pend.done; });
    g_pending.erase(seq);
    if (!pend.done) {
        const char *e = sent ? "no answer from the game in time" : "the link to the game is full";
        lk.unlock();
        if (fn) fn(0, e, strlen(e), user);
        return -1;
    }
    std::string text = pend.text;
    bool ok = pend.ok;
    lk.unlock();
    if (fn) fn(ok ? 1 : 0, text.c_str(), text.size(), user);
    return ok ? 1 : 0;
}

int H_Subscribe(MmlSideMod *m, const char *type, MmlSideEventFn fn, void *user) {
    Half *h = Of(m);
    if (!h || !type || !fn) return 0;
    h->subs.push_back({type, fn, user});
    Send(side::kSubscribe, 0, {{"mod", h->id}, {"types", SubTypes(*h)}});
    return 1;
}

int H_Command(MmlSideMod *m, const char *name, const char *help, MmlSideCommandFn fn, void *user) {
    Half *h = Of(m);
    if (!h || !name || !*name || !fn) return 0;
    h->commands[name] = {fn, user, help ? help : ""};
    Send(side::kCommandAdd, 0, {{"mod", h->id}, {"name", name}, {"help", help ? help : ""}});
    return 1;
}
void H_ReplyAppend(MmlSideReply *r, const char *text, size_t len) { if (r && text) r->text.append(text, len); }
void H_ReplyError(MmlSideReply *r, const char *msg) {
    if (!r) return;
    r->error = true;
    r->text = msg ? msg : "error";
}

int H_AdapterLuid(uint32_t luid[2]) {
    if (!g_haveLuid) return 0;
    luid[0] = g_luid[0];
    luid[1] = g_luid[1];
    return 1;
}
void *H_D3d12Device() { return D3d12(); }

void DropTexture(Half *h, Texture &t, bool tell) {
    if (tell) Send(side::kTextureGone, 0, {{"mod", h->id}, {"name", t.name}});
    if (t.handle) CloseHandle(t.handle);
    if (t.res) t.res->Release();
    t = Texture{};
}

int H_TextureCreate(MmlSideMod *m, const char *name, uint32_t w, uint32_t hgt, uint32_t format, MmlSideTexture *out) {
    Half *h = Of(m);
    if (!h || !name || !*name || !w || !hgt || w > 16384 || hgt > 16384) return 0;
    if (!FormatShareable(format)) { Log(h->id, 2, std::string("texture_create ") + name + ": a format Direct3D 11 does not open from Direct3D 12"); return 0; }
    ID3D12Device *dev = D3d12();
    if (!dev) { Log(h->id, 2, std::string("texture_create ") + name + ": " + (g_haveLuid ? g_d3d12Error : "the game's adapter is not known yet")); return 0; }
    for (Texture &t : h->textures) if (t.name == name) DropTexture(h, t, true);
    h->textures.erase(std::remove_if(h->textures.begin(), h->textures.end(), [](const Texture &t) { return !t.res; }), h->textures.end());
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = hgt;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = static_cast<DXGI_FORMAT>(format);
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // (what Direct3D 11 opens: a render target with simultaneous access; the halves' writers sync by their own
    // means, the readers read only versions the writer has finished)
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    Texture t;
    t.name = name;
    t.w = w;
    t.h = hgt;
    t.format = format;
    t.generation = ++h->textureGen;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t.res));
    if (FAILED(hr) || FAILED(dev->CreateSharedHandle(t.res, nullptr, GENERIC_ALL, nullptr, &t.handle))) {
        Log(h->id, 2, std::string("texture_create ") + name + ": the shared texture was not made");
        DropTexture(h, t, false);
        return 0;
    }
    HANDLE theirs = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), t.handle, g_game, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        Log(h->id, 2, std::string("texture_create ") + name + ": its handle was not given to the game (" + std::to_string(GetLastError()) + ")");
        DropTexture(h, t, false);
        return 0;
    }
    Send(side::kTexture, 0, {{"mod", h->id}, {"name", name}, {"handle", static_cast<uint64_t>(reinterpret_cast<uintptr_t>(theirs))},
                             {"width", w}, {"height", hgt}, {"format", format}, {"generation", t.generation}});
    h->textures.push_back(t);
    if (out && out->size >= sizeof(MmlSideTexture)) {
        out->width = w;
        out->height = hgt;
        out->format = format;
        out->d3d12_resource = t.res;
        out->shared_handle = t.handle;
        out->generation = t.generation;
    }
    return 1;
}
void H_TextureRemove(MmlSideMod *m, const char *name) {
    Half *h = Of(m);
    if (!h || !name) return;
    for (Texture &t : h->textures) if (t.name == name) DropTexture(h, t, true);
    h->textures.erase(std::remove_if(h->textures.begin(), h->textures.end(), [](const Texture &t) { return !t.res; }), h->textures.end());
}

MmlSideChannel *H_ChannelOpen(MmlSideMod *m, const char *name, uint32_t size) {
    Half *h = Of(m);
    if (!h || !name || !*name) return nullptr;
    auto *c = new MmlSideChannel;
    std::string err;
    if (!side::ChannelOpen(&c->ch, g_gamePid, h->id, name, size, &err)) {
        Log(h->id, 2, std::string("channel_open ") + name + ": " + err);
        delete c;
        return nullptr;
    }
    h->channels.push_back(c);
    return c;
}
int H_ChannelWrite(MmlSideChannel *c, const void *data, uint32_t bytes) { return c && side::ChannelWrite(&c->ch, data, bytes) ? 1 : 0; }
uint32_t H_ChannelRead(MmlSideChannel *c, void *data, uint32_t cap, uint64_t *version) { return c ? side::ChannelRead(&c->ch, data, cap, version) : 0; }

const MmlSideHost g_host = {
    sizeof(MmlSideHost), MML_SIDE_API_VERSION,
    H_GameDir, H_GameBuild, H_ModId, H_ModDir, H_DataDir, H_Log,
    H_Call, H_Subscribe, H_Command, H_ReplyAppend, H_ReplyError,
    H_AdapterLuid, H_D3d12Device, H_TextureCreate, H_TextureRemove,
    H_ChannelOpen, H_ChannelWrite, H_ChannelRead,
};

// ---- calls into halves, each behind __try (no C++ objects with destructors here)
thread_local uintptr_t t_faultAt;
int NoteFault(EXCEPTION_POINTERS *x) {
    t_faultAt = reinterpret_cast<uintptr_t>(x->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
DWORD SafeLoad(MmlSideLoadFn fn, MmlSideMod *m, int *rc) {
    __try { *rc = fn(&g_host, m); return 0; } __except (NoteFault(GetExceptionInformation())) { return GetExceptionCode(); }
}
DWORD SafeUnload(MmlSideUnloadFn fn, MmlSideMod *m) {
    __try { fn(m); return 0; } __except (NoteFault(GetExceptionInformation())) { return GetExceptionCode(); }
}
DWORD SafeEvent(MmlSideEventFn fn, MmlSideMod *m, const char *type, const char *js, void *user) {
    __try { fn(m, type, js, user); return 0; } __except (NoteFault(GetExceptionInformation())) { return GetExceptionCode(); }
}
DWORD SafeCommand(MmlSideCommandFn fn, MmlSideMod *m, const char *args, MmlSideReply *r, void *user, int *rc) {
    __try { *rc = fn(m, args, r, user); return 0; } __except (NoteFault(GetExceptionInformation())) { return GetExceptionCode(); }
}

std::string Where(uintptr_t at) {
    HMODULE mod = nullptr;
    char b[512];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<const char *>(at), &mod) && mod) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(mod, path, MAX_PATH);
        std::string p = Utf8(path);
        snprintf(b, sizeof b, "%s+0x%llX", p.substr(p.find_last_of('\\') + 1).c_str(),
                 static_cast<unsigned long long>(at - reinterpret_cast<uintptr_t>(mod)));
    } else {
        snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(at));
    }
    return b;
}

void State(const Half &h, const char *state, const std::string &error = std::string(), const std::string &where = std::string()) {
    json j = {{"id", h.id}, {"generation", h.generation}, {"state", state}};
    if (!error.empty()) j["error"] = error;
    if (!where.empty()) j["where"] = where;
    Send(side::kModState, 0, j);
}

void Faulted(Half *h, const char *in, DWORD code) {
    char b[160];
    snprintf(b, sizeof b, "%s faulted (%08lX)", in, static_cast<unsigned long>(code));
    std::string where = Where(t_faultAt);
    Log(h->id, 3, std::string(b) + " at " + where + ": its 64-bit half is unloaded");
    h->faulted = true;
    State(*h, "faulted", b, where);
}

// ---- loading: from a fresh copy of the half's folder, as the loader's lifecycle.cpp does for 32-bit DLLs
bool CopyTree(const std::wstring &from, const std::wstring &to, std::string *err) {
    CreateDirectoryW(to.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((from + L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = CopyTree(from + n + L"\\", to + n + L"\\", err) && ok;
        else if (!CopyFileW((from + n).c_str(), (to + n).c_str(), FALSE)) {
            *err = "copying " + Utf8(n) + " failed (" + std::to_string(GetLastError()) + ")";
            ok = false;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return ok;
}
void RemoveTree(const std::wstring &dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(dir + n + L"\\");
            else DeleteFileW((dir + n).c_str());
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

void Unload(const std::string &id, const char *why) {
    auto it = g_halves.find(id);
    if (it == g_halves.end()) return;
    Half *h = it->second.get();
    if (h->unload && !h->faulted) {
        DWORD ex = SafeUnload(h->unload, &h->mod);
        if (ex) Log(id, 3, "mml_side_unload faulted at " + Where(t_faultAt));
    }
    h->alive = false;
    for (Texture &t : h->textures) DropTexture(h, t, true);
    for (MmlSideChannel *c : h->channels) { side::ChannelClose(&c->ch); delete c; }
    for (auto &c : h->commands) Send(side::kCommandRemove, 0, {{"mod", id}, {"name", c.first}});
    Send(side::kSubscribe, 0, {{"mod", id}, {"types", json::array()}});
    if (h->dll) FreeLibrary(h->dll);
    if (!h->liveDir.empty()) RemoveTree(Wide(h->liveDir));
    if (!h->faulted) State(*h, "unloaded", why);
    Log(id, 1, std::string("64-bit half unloaded (") + why + ")");
    g_halves.erase(it);
}

void Load(const json &j) {
    std::string id = j.value("id", "");
    if (id.empty()) return;
    Unload(id, "reload");
    auto h = std::make_unique<Half>();
    h->id = id;
    h->dir = j.value("dir", "");
    h->entry = j.value("entry", "");
    h->dataDir = j.value("data_dir", "");
    h->generation = j.value("generation", 0u);
    h->mod.half = h.get();
    std::string entry = h->entry;
    for (char &c : entry) if (c == '/') c = '\\';
    size_t slash = entry.find_last_of('\\');
    std::string sub = slash == std::string::npos ? std::string() : entry.substr(0, slash + 1);
    std::string file = slash == std::string::npos ? entry : entry.substr(slash + 1);
    char tag[160];
    snprintf(tag, sizeof tag, "%s_%u_%u", id.c_str(), static_cast<unsigned>(GetCurrentProcessId()), h->generation);
    h->liveDir = g_mmlDir + "live64\\" + tag + "\\";
    CreateDirectoryW(Wide(g_mmlDir + "live64").c_str(), nullptr);
    std::string err;
    if (!CopyTree(Wide(h->dir + sub), Wide(h->liveDir), &err)) {
        State(*h, "failed", err);
        RemoveTree(Wide(h->liveDir));
        return;
    }
    std::wstring path = Wide(h->liveDir + file);
    SetDllDirectoryW(Wide(h->liveDir).c_str());
    h->dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    DWORD code = GetLastError();
    if (!h->dll) {
        SetDllDirectoryW(nullptr);
        State(*h, "failed", "LoadLibrary of " + h->entry + " failed (" + std::to_string(code) + ")" +
                                (code == ERROR_MOD_NOT_FOUND ? ": a DLL it imports is not beside it" : code == ERROR_BAD_EXE_FORMAT ? ": not a 64-bit DLL" : ""));
        RemoveTree(Wide(h->liveDir));
        return;
    }
    auto load = reinterpret_cast<MmlSideLoadFn>(GetProcAddress(h->dll, "mml_side_load"));
    auto api = reinterpret_cast<MmlSideApiFn>(GetProcAddress(h->dll, "mml_side_api"));
    h->unload = reinterpret_cast<MmlSideUnloadFn>(GetProcAddress(h->dll, "mml_side_unload"));
    uint32_t want = api ? api() : MML_SIDE_API_VERSION;
    if (!load || want > MML_SIDE_API_VERSION) {
        SetDllDirectoryW(nullptr);
        State(*h, "failed", !load ? "no mml_side_load export" : "made for a newer sidecar (API " + std::to_string(want) + ")");
        FreeLibrary(h->dll);
        RemoveTree(Wide(h->liveDir));
        return;
    }
    h->alive = true;
    Half *raw = h.get();
    g_halves[id] = std::move(h);
    int rc = 0;
    DWORD ex = SafeLoad(load, &raw->mod, &rc);
    SetDllDirectoryW(nullptr);
    if (ex || rc) {
        if (ex) Faulted(raw, "mml_side_load", ex);
        else State(*raw, "failed", "mml_side_load returned " + std::to_string(rc));
        raw->unload = nullptr;
        Unload(id, "its load failed");
        return;
    }
    Log(id, 1, "64-bit half loaded (generation " + std::to_string(raw->generation) + ")");
    State(*raw, "loaded");
}

void Event(const json &j) {
    std::string type = j.value("type", "");
    std::string text = Dump(j);
    std::vector<Half *> faulted;
    for (auto &kv : g_halves) {
        Half *h = kv.second.get();
        if (!h->alive || h->faulted) continue;
        std::vector<Half::Sub> subs = h->subs;   // (a callback may subscribe again)
        for (const Half::Sub &s : subs) {
            if (s.type != type && s.type != "*") continue;
            DWORD ex = SafeEvent(s.fn, &h->mod, type.c_str(), text.c_str(), s.user);
            if (ex) { Faulted(h, ("its " + type + " handler").c_str(), ex); faulted.push_back(h); break; }
        }
    }
    for (Half *h : faulted) Unload(h->id, "faulted");
}

void Command(uint32_t seq, const json &j) {
    std::string mod = j.value("mod", ""), name = j.value("name", "");
    json args = j.contains("args") ? j["args"] : json::object();
    auto it = g_halves.find(mod);
    Half *h = it == g_halves.end() ? nullptr : it->second.get();
    auto c = h && h->alive ? h->commands.find(name) : std::map<std::string, Half::Cmd>::iterator();
    if (!h || !h->alive || c == h->commands.end()) {
        Send(side::kCommandReply, seq, {{"ok", false}, {"text", mod + "." + name + ": no such command in its 64-bit half"}});
        return;
    }
    MmlSideReply r;
    int rc = 0;
    std::string a = Dump(args);
    DWORD ex = SafeCommand(c->second.fn, &h->mod, a.c_str(), &r, c->second.user, &rc);
    if (ex) {
        Faulted(h, ("its command " + name).c_str(), ex);
        Send(side::kCommandReply, seq, {{"ok", false}, {"text", mod + "." + name + " faulted: its 64-bit half is unloaded"}});
        Unload(mod, "faulted");
        return;
    }
    Send(side::kCommandReply, seq, {{"ok", rc == 0 && !r.error}, {"text", r.text}});
}

bool Open(uint32_t pid) {
    wchar_t sec[64], sd[64], ss[64], hd[64], hs[64];
    side::Names(pid, sec, sd, ss, hd, hs);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, sec);
    if (!map) return false;
    auto *base = static_cast<uint8_t *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, side::kSectionBytes));
    if (!base) return false;
    g_sec = reinterpret_cast<side::Section *>(base);
    if (g_sec->magic != side::kMagic || g_sec->version != side::kVersion) return false;
    g_in.ring = reinterpret_cast<side::Ring *>(base + side::kOffToSide);
    g_in.data = base + side::kOffToSideData;
    g_in.dataEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, sd);
    g_in.spaceEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, ss);
    g_out.ring = reinterpret_cast<side::Ring *>(base + side::kOffToHost);
    g_out.data = base + side::kOffToHostData;
    g_out.dataEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, hd);
    g_out.spaceEvent = OpenEventW(EVENT_ALL_ACCESS, FALSE, hs);
    return g_in.dataEvent && g_in.spaceEvent && g_out.dataEvent && g_out.spaceEvent;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    for (int i = 1; i + 1 < argc; i++)
        if (!wcscmp(argv[i], L"--game")) g_gamePid = static_cast<uint32_t>(_wtoi(argv[i + 1]));
    if (!g_gamePid) return 2;
    g_game = OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_gamePid);
    if (!g_game || !Open(g_gamePid)) return 3;
    g_sec->sidecarPid = GetCurrentProcessId();
    g_sec->sidecarAlive = 1;
    g_qEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE reader = CreateThread(nullptr, 0, Reader, nullptr, 0, nullptr);
    Send(side::kReady, 0, {{"pid", GetCurrentProcessId()}, {"version", side::kVersion}});
    bool quit = false;
    while (!quit) {
        HANDLE w[2] = {g_qEvent, g_game};
        DWORD r = WaitForMultipleObjects(2, w, FALSE, 200);
        if (r == WAIT_OBJECT_0 + 1) break;   // the game is gone
        for (;;) {
            Msg m;
            {
                std::lock_guard<std::mutex> lk(g_qMx);
                if (g_queue.empty()) break;
                m = std::move(g_queue.front());
                g_queue.pop_front();
            }
            json j = m.payload.empty() ? json() : json::parse(m.payload, nullptr, false);
            switch (m.type) {
            case side::kHello:
                g_gameDir = j.value("game_dir", "");
                g_mmlDir = j.value("mml_dir", "");
                g_build = j.value("build", "unknown");
                if (!g_logFile && !g_mmlDir.empty()) g_logFile = _wfopen(Wide(g_mmlDir + "sidecar.log").c_str(), L"w");
                if (j.contains("luid") && j["luid"].is_array() && j["luid"].size() == 2) {
                    g_luid[0] = j["luid"][0].get<uint32_t>();
                    g_luid[1] = j["luid"][1].get<uint32_t>();
                    g_haveLuid = true;
                }
                Logf(1, "mml-sidecar64 %u for the game %u (%s), %s", GetCurrentProcessId(), g_gamePid, g_build.c_str(), g_gameDir.c_str());
                break;
            case side::kAdapter:
                if (j.contains("luid") && j["luid"].is_array() && j["luid"].size() == 2) {
                    g_luid[0] = j["luid"][0].get<uint32_t>();
                    g_luid[1] = j["luid"][1].get<uint32_t>();
                    g_haveLuid = true;
                }
                break;
            case side::kModLoad: Load(j); break;
            case side::kModUnload: Unload(j.value("id", ""), j.value("why", "unload").c_str()); break;
            case side::kEvent: Event(j); break;
            case side::kCommand: Command(m.aux, j); break;
            case side::kQuit: quit = true; break;
            default: break;
            }
            if (quit) break;
        }
        // halves that faulted outside a callback the loop runs (none yet) would be unloaded here
    }
    std::vector<std::string> ids;
    for (auto &kv : g_halves) ids.push_back(kv.first);
    for (const std::string &id : ids) Unload(id, "the sidecar ends");
    g_quit = true;
    g_sec->sidecarAlive = 0;
    SetEvent(g_in.dataEvent);
    WaitForSingleObject(reader, 1000);
    if (g_d3d12) g_d3d12->Release();
    if (g_logFile) fclose(g_logFile);
    return 0;
}
