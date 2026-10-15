// The bridge: what the MCP server (and any tool) talks to (see docs/PROTOCOL.md).
//
// TCP on 127.0.0.1 only (port 47100, or <game>\mml\config.json "bridge_port"; the next free one when that is taken),
// one JSON object per line both ways:
//   -> {"id": 1, "method": "hello", "params": {"token": "..."}}       first, on every connection
//   -> {"id": 2, "method": "mem.read", "params": {"addr": "game+0x2479C0", "size": 16}}
//   <- {"id": 2, "result": {...}}   or   {"id": 2, "error": {"message": "..."}}
// The port and the token are written to <game>\mml\bridge.json at start (the token changes every run). Methods that
// only read memory or the log run on the bridge's own thread (they answer even while the game thread hangs or loads);
// the rest (mods, their commands) wait for the game thread's next frame, `timeout_ms` (default 5000).
#include "host.h"
#include <ws2tcpip.h>
#include <psapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include "json.hpp"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")

using json = nlohmann::json;

namespace host {
json EventsSince(const json &p, std::string *err);   // (events.cpp: the bridge thread answers it)
namespace {
struct Job {
    std::string method, params, reply;
    bool ok = false, done = false;
};
std::mutex g_mx;
std::condition_variable g_cv;
std::deque<Job *> g_jobs;
std::atomic<uint64_t> g_frames{0};
std::string g_token;
int g_port = 47100;
LARGE_INTEGER g_start, g_freq;

std::string Hex(const uint8_t *p, size_t n) {
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}
bool Unhex(const std::string &text, std::vector<uint8_t> *out) {
    std::string h;
    for (char c : text) if (!isspace(static_cast<unsigned char>(c))) h += c;
    if (h.size() % 2) return false;
    for (size_t i = 0; i < h.size(); i += 2) {
        char *end;
        std::string b = h.substr(i, 2);
        unsigned long v = strtoul(b.c_str(), &end, 16);
        if (*end) return false;
        out->push_back(static_cast<uint8_t>(v));
    }
    return true;
}

uintptr_t AddrParam(const json &p, std::string *err) {
    if (!p.contains("addr")) { *err = "addr missing"; return 0; }
    const json &a = p["addr"];
    uintptr_t v = a.is_number_unsigned() ? static_cast<uintptr_t>(a.get<uint64_t>()) : a.is_string() ? ParseAddress(a.get<std::string>()) : 0;
    if (!v) *err = "addr: not an address (\"0x5762A0\", \"game+0x1762A0\", \"ls3d+1AAE0\", \"<module>+<hex>\")";
    return v;
}

// mem.read {addr, size (<= 1 MiB), as: "u8"|"i8"|"u16"|"i16"|"u32"|"i32"|"f32"|"f64"|"ptr"|"str"}
json MemReadMethod(const json &p, std::string *err) {
    uintptr_t addr = AddrParam(p, err);
    if (!addr) return nullptr;
    size_t size = p.value("size", 64u);
    if (size == 0 || size > (1u << 20)) { *err = "size: 1..1048576"; return nullptr; }
    std::vector<uint8_t> buf(size);
    if (!MemRead(addr, buf.data(), size)) { *err = "cannot read " + DescribeAddress(addr) + " (" + std::to_string(size) + " bytes)"; return nullptr; }
    json r = {{"addr", DescribeAddress(addr)}, {"abs", addr}, {"size", size}, {"hex", Hex(buf.data(), size)}};
    std::string as = p.value("as", "");
    if (!as.empty()) {
        json v = json::array();
        auto each = [&](size_t w, auto fn) { for (size_t i = 0; i + w <= size; i += w) v.push_back(fn(buf.data() + i)); };
        if (as == "u8") each(1, [](const uint8_t *q) { return *q; });
        else if (as == "i8") each(1, [](const uint8_t *q) { return *reinterpret_cast<const int8_t *>(q); });
        else if (as == "u16") each(2, [](const uint8_t *q) { return *reinterpret_cast<const uint16_t *>(q); });
        else if (as == "i16") each(2, [](const uint8_t *q) { return *reinterpret_cast<const int16_t *>(q); });
        else if (as == "u32") each(4, [](const uint8_t *q) { return *reinterpret_cast<const uint32_t *>(q); });
        else if (as == "i32") each(4, [](const uint8_t *q) { return *reinterpret_cast<const int32_t *>(q); });
        else if (as == "f32") each(4, [](const uint8_t *q) { return *reinterpret_cast<const float *>(q); });
        else if (as == "f64") each(8, [](const uint8_t *q) { return *reinterpret_cast<const double *>(q); });
        else if (as == "ptr") each(4, [](const uint8_t *q) { return DescribeAddress(*reinterpret_cast<const uint32_t *>(q)); });
        else if (as == "str") v = std::string(reinterpret_cast<const char *>(buf.data()), strnlen(reinterpret_cast<const char *>(buf.data()), size));
        else { *err = "as: u8 i8 u16 i16 u32 i32 f32 f64 ptr str"; return nullptr; }
        r["value"] = v;
    }
    return r;
}

// mem.write {addr, hex: "90 90"} or {addr, as: "u32"|"i32"|"f32"|..., values: [..]}
json MemWriteMethod(const json &p, std::string *err) {
    uintptr_t addr = AddrParam(p, err);
    if (!addr) return nullptr;
    std::vector<uint8_t> bytes;
    if (p.contains("hex")) {
        if (!Unhex(p["hex"].get<std::string>(), &bytes)) { *err = "hex: pairs of hex digits"; return nullptr; }
    } else if (p.contains("values") && p["values"].is_array()) {
        std::string as = p.value("as", "u32");
        for (auto &v : p["values"]) {
            uint8_t b[8];
            size_t w;
            if (as == "u8" || as == "i8") { b[0] = static_cast<uint8_t>(v.get<int64_t>()); w = 1; }
            else if (as == "u16" || as == "i16") { uint16_t x = static_cast<uint16_t>(v.get<int64_t>()); memcpy(b, &x, 2); w = 2; }
            else if (as == "u32" || as == "i32" || as == "ptr") { uint32_t x = static_cast<uint32_t>(v.get<int64_t>()); memcpy(b, &x, 4); w = 4; }
            else if (as == "f32") { float x = v.get<float>(); memcpy(b, &x, 4); w = 4; }
            else if (as == "f64") { double x = v.get<double>(); memcpy(b, &x, 8); w = 8; }
            else { *err = "as: u8 i8 u16 i16 u32 i32 ptr f32 f64"; return nullptr; }
            bytes.insert(bytes.end(), b, b + w);
        }
    } else { *err = "hex or values needed"; return nullptr; }
    if (bytes.empty() || !MemWrite(addr, bytes.data(), bytes.size())) { *err = "cannot write " + DescribeAddress(addr); return nullptr; }
    return {{"addr", DescribeAddress(addr)}, {"written", bytes.size()}};
}

json Modules() {
    HMODULE mods[1024];
    DWORD need = 0;
    json out = json::array();
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return out;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++) {
        char path[MAX_PATH] = "";
        MODULEINFO mi{};
        GetModuleFileNameA(mods[i], path, MAX_PATH);
        GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
        out.push_back({{"name", strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path}, {"base", reinterpret_cast<uintptr_t>(mi.lpBaseOfDll)},
                       {"size", mi.SizeOfImage}, {"path", path}});
    }
    return out;
}

// What the bridge thread answers itself; false: the method is for the game thread.
bool Immediate(const std::string &m, const json &p, json *result, std::string *err) {
    if (m == "ping") {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        *result = {{"pid", GetCurrentProcessId()}, {"build", GameBuild()}, {"frames", Frames()}, {"api", MML_API_VERSION},
                   {"uptime_s", static_cast<double>(now.QuadPart - g_start.QuadPart) / static_cast<double>(g_freq.QuadPart)},
                   {"game_dir", GameDir()}};
        return true;
    }
    if (m == "log.tail") {
        *result = LogTail(p.is_object() ? p.value("lines", 100u) : 100u);
        return true;
    }
    if (m == "mem.read") { *result = MemReadMethod(p, err); return true; }
    if (m == "mem.write") { *result = MemWriteMethod(p, err); return true; }
    if (m == "addr.describe") {
        uintptr_t a = AddrParam(p, err);
        if (a) *result = {{"abs", a}, {"where", DescribeAddress(a)}};
        return true;
    }
    if (m == "modules") { *result = Modules(); return true; }
    if (m == "where") { *result = Where(); return true; }
    if (m == "tap") { *result = tap::Command(p.is_object() ? p.value("cmd", "") : ""); return true; }
    if (m == "events.since") { *result = EventsSince(p, err); return true; }   // (events.cpp)
    return false;
}

json Handle(const json &req, bool *authed) {
    json id = req.contains("id") ? req["id"] : json();
    std::string method = req.value("method", "");
    json params = req.contains("params") ? req["params"] : json::object();
    auto fail = [&](const std::string &msg) { return json{{"id", id}, {"error", {{"message", msg}}}}; };
    if (method == "hello") {
        if (!params.is_object() || params.value("token", "") != g_token) return fail("bad token (read mml\\bridge.json)");
        *authed = true;
        return {{"id", id}, {"result", {{"pid", GetCurrentProcessId()}, {"build", GameBuild()}, {"api", MML_API_VERSION}}}};
    }
    if (!*authed) return fail("say hello with the token first (mml\\bridge.json)");
    json result;
    std::string err;
    if (Immediate(method, params, &result, &err)) return err.empty() ? json{{"id", id}, {"result", result}} : fail(err);
    // the game thread's
    Job job;
    job.method = method;
    job.params = params.dump();
    int timeout = params.is_object() ? params.value("timeout_ms", 5000) : 5000;
    {
        std::unique_lock<std::mutex> lk(g_mx);
        g_jobs.push_back(&job);
        if (!g_cv.wait_for(lk, std::chrono::milliseconds(timeout), [&] { return job.done; })) {
            g_jobs.erase(std::remove(g_jobs.begin(), g_jobs.end(), &job), g_jobs.end());
            return fail("the game thread did not answer in " + std::to_string(timeout) + " ms (paused, minimized, loading or hung? `where` shows what it does)");
        }
    }
    if (!job.ok) return fail(job.reply);
    try { return {{"id", id}, {"result", json::parse(job.reply)}}; }
    catch (...) { return {{"id", id}, {"result", job.reply}}; }   // a mod answered plain text
}

void Serve(SOCKET s) {
    bool authed = false;
    std::string buf;
    char tmp[4096];
    int n;
    while ((n = recv(s, tmp, sizeof tmp, 0)) > 0) {
        buf.append(tmp, n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty() || line == "\r") continue;
            json reply;
            try { reply = Handle(json::parse(line), &authed); }
            catch (const std::exception &ex) { reply = {{"id", nullptr}, {"error", {{"message", std::string("bad request: ") + ex.what()}}}}; }
            std::string out = reply.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
            send(s, out.data(), static_cast<int>(out.size()), 0);
        }
    }
    closesocket(s);
}

// The first free port from the configured one on (another game with the loader, or a test harness, may hold it).
SOCKET Listen() {
    for (int port = g_port; port < g_port + 32; port++) {
        SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) return ls;
        BOOL excl = TRUE;
        setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&excl), sizeof excl);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (!bind(ls, reinterpret_cast<sockaddr *>(&addr), sizeof addr) && !listen(ls, 4)) {
            if (port != g_port) HWARN("bridge: port %d is taken, using %d", g_port, port);
            g_port = port;
            return ls;
        }
        closesocket(ls);
    }
    return INVALID_SOCKET;
}

DWORD WINAPI ServerThread(void *) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = Listen();
    if (ls == INVALID_SOCKET) {
        HWARN("bridge: cannot listen on 127.0.0.1:%d..%d (%d)", g_port, g_port + 31, WSAGetLastError());
        return 0;
    }
    // written once the port is known: clients read the port and the token from it
    json info = {{"port", g_port}, {"token", g_token}, {"pid", GetCurrentProcessId()}, {"game_dir", GameDir()}, {"api", MML_API_VERSION}};
    std::ofstream(MmlDir() + "bridge.json") << info.dump(2);
    HLOG("bridge: listening on 127.0.0.1:%d", g_port);
    for (;;) {
        SOCKET s = accept(ls, nullptr, nullptr);
        if (s == INVALID_SOCKET) continue;
        HANDLE t = CreateThread(nullptr, 0, [](void *p) -> DWORD { Serve(reinterpret_cast<SOCKET>(p)); return 0; },
                                reinterpret_cast<void *>(s), 0, nullptr);
        if (t) CloseHandle(t); else closesocket(s);
    }
}
} // namespace

uint64_t Frames() { return g_frames.load(); }

void BridgeStart() {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    tap::Init();
    {
        // <game>\mml\config.json: {"bridge_port": 47100, "taps": ["5409D0 n=4 ptr=1:32 log", ...]}
        std::ifstream f(MmlDir() + "config.json");
        if (f) {
            try {
                json cfg = json::parse(f, nullptr, true, true);
                g_port = cfg.value("bridge_port", g_port);
                if (cfg.contains("taps") && cfg["taps"].is_array())
                    for (auto &t : cfg["taps"])
                        if (t.is_string()) HLOG("config tap %s: %s", t.get<std::string>().c_str(), tap::Command(t.get<std::string>()).c_str());
            } catch (const std::exception &ex) { HWARN("mml\\config.json: %s", ex.what()); }
        }
    }
    std::random_device rd;
    char tok[33];
    snprintf(tok, sizeof tok, "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
    g_token = tok;
    DeleteFileA((MmlDir() + "bridge.json").c_str());   // a stale one would send clients to the last run's port
    CreateThread(nullptr, 0, ServerThread, nullptr, 0, nullptr);
}

void BridgeFrame() {
    g_frames++;
    std::unique_lock<std::mutex> lk(g_mx);
    while (!g_jobs.empty()) {
        Job *j = g_jobs.front();
        g_jobs.pop_front();
        lk.unlock();
        try { j->ok = ExecCommand(j->method, j->params, &j->reply); }
        catch (const std::exception &ex) { j->ok = false; j->reply = std::string("exception: ") + ex.what(); }
        lk.lock();
        j->done = true;
        g_cv.notify_all();
    }
}

} // namespace host
