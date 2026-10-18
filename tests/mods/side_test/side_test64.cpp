// side_test: the offline tests' 64-bit half (tests/run_tests.py sidecar_tests): its commands answer from the sidecar,
// it calls the bridge, counts the frames it is told about, writes the channel box and reads back (its 32-bit half,
// side_test.cpp, reads box and writes back), faults when asked.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "mml/side.h"

namespace {
const MmlSideHost *H;
unsigned long long g_frames;
MmlSideChannel *g_ch, *g_back, *g_flood;
std::thread g_flooder, g_later;
std::atomic<bool> g_stop;
unsigned g_loads;

void OnFrame(MmlSideMod *, const char *, const char *, void *) { g_frames++; }

void Reply(MmlSideReply *r, const std::string &s) { H->reply_append(r, s.data(), s.size()); }

int Echo(MmlSideMod *, const char *args, MmlSideReply *r, void *) {
    Reply(r, std::string("{\"args\": ") + (args && *args ? args : "null") + ", \"pointer_bits\": " + std::to_string(sizeof(void *) * 8) + "}");
    return 0;
}
int Frames(MmlSideMod *, const char *, MmlSideReply *r, void *) {
    Reply(r, "{\"frames\": " + std::to_string(g_frames) + "}");
    return 0;
}
// {"method": "...", "params": {...}}: that bridge call made from the 64-bit side, its answer returned
int Call(MmlSideMod *m, const char *args, MmlSideReply *r, void *) {
    std::string a = args ? args : "";
    auto field = [&](const char *key) -> std::string {   // (a tiny reader: the tests send flat objects)
        std::string k = std::string("\"") + key + "\"";
        size_t at = a.find(k);
        if (at == std::string::npos) return std::string();
        at = a.find(':', at) + 1;
        while (at < a.size() && a[at] == ' ') at++;
        if (a[at] == '"') {
            size_t end = a.find('"', at + 1);
            return a.substr(at + 1, end - at - 1);
        }
        int depth = 0;
        size_t i = at;
        for (; i < a.size(); i++) {
            if (a[i] == '{' || a[i] == '[') depth++;
            if (a[i] == '}' || a[i] == ']') { if (!depth) break; depth--; }
            if (a[i] == ',' && !depth) break;
        }
        return a.substr(at, i - at);
    };
    std::string method = field("method"), params = field("params");
    struct Got { std::string text; int ok = 0; } got;
    int rc = H->call(m, method.c_str(), params.empty() ? nullptr : params.c_str(), [](int ok, const char *t, size_t n, void *u) {
        static_cast<Got *>(u)->ok = ok;
        static_cast<Got *>(u)->text.assign(t, n);
    }, &got, 5000);
    if (rc != 1) { H->reply_error(r, ("the call failed: " + got.text).c_str()); return 1; }
    Reply(r, got.text);
    return 0;
}
// {"text": "..."}: written as the channel's next version
int Write(MmlSideMod *, const char *args, MmlSideReply *r, void *) {
    std::string a = args ? args : "";
    if (!g_ch || !H->channel_write(g_ch, a.data(), static_cast<uint32_t>(a.size()))) { H->reply_error(r, "no channel"); return 1; }
    Reply(r, "{\"written\": " + std::to_string(a.size()) + "}");
    return 0;
}
// {} -> {"text", "version"}: back's latest whole version (as its 32-bit half wrote it)
int Fetch(MmlSideMod *, const char *, MmlSideReply *r, void *) {
    char buf[4096];
    uint64_t version = 0;
    uint32_t n = g_back ? H->channel_read(g_back, buf, sizeof buf, &version) : 0;
    std::string q = "\"";
    for (uint32_t i = 0; i < n; i++) {
        if (buf[i] == '"' || buf[i] == '\\') q += '\\';
        q += buf[i];
    }
    Reply(r, "{\"text\": " + q + "\", \"version\": " + std::to_string(version) + "}");
    return 0;
}
// {} -> a thread writes the channel flood for 1.5 s, as fast as it can: each version 4096 bytes, the version's
// number in the first 8 and its low byte in every other (the 32-bit half's `torn` reads meanwhile)
int Flood(MmlSideMod *, const char *, MmlSideReply *r, void *) {
    if (!g_flood) { H->reply_error(r, "no channel flood"); return 1; }
    if (g_flooder.joinable()) g_flooder.join();
    g_stop = false;
    g_flooder = std::thread([] {
        std::vector<unsigned char> v(4096);
        unsigned long long n = 0;
        auto end = GetTickCount64() + 1500;
        while (!g_stop && GetTickCount64() < end) {
            n++;
            memcpy(v.data(), &n, 8);
            memset(v.data() + 8, static_cast<int>(n & 0xFF), v.size() - 8);
            H->channel_write(g_flood, v.data(), static_cast<uint32_t>(v.size()));
        }
    });
    Reply(r, "{\"started\": true}");
    return 0;
}
uint32_t Num(const char *args, const char *key, uint32_t def) {   // (the tests send flat objects of numbers)
    std::string a = args ? args : "", k = std::string("\"") + key + "\"";
    size_t at = a.find(k);
    return at == std::string::npos ? def : static_cast<uint32_t>(strtoul(a.c_str() + a.find(':', at) + 1, nullptr, 10));
}
// {"ms"}: a thread of this half writes "later" to box after that long (the 32-bit half's wait_box waits for it)
int WriteLater(MmlSideMod *, const char *args, MmlSideReply *r, void *) {
    if (g_later.joinable()) g_later.join();
    uint32_t ms = Num(args, "ms", 300);
    g_later = std::thread([ms] {
        Sleep(ms);
        H->channel_write(g_ch, "later", 5);
    });
    Reply(r, "{\"started\": true}");
    return 0;
}
// {"wait_ms"} -> {"after", "version", "ms"}: channel_wait for a version of back after the latest one
int WaitBack(MmlSideMod *, const char *args, MmlSideReply *r, void *) {
    char buf[16];
    uint64_t after = 0;
    H->channel_read(g_back, buf, sizeof buf, &after);
    ULONGLONG t0 = GetTickCount64();
    uint64_t v = H->channel_wait(g_back, after, Num(args, "wait_ms", 1000));
    Reply(r, "{\"after\": " + std::to_string(after) + ", \"version\": " + std::to_string(v) + ", \"ms\": " + std::to_string(GetTickCount64() - t0) + "}");
    return 0;
}
int Fault(MmlSideMod *, const char *, MmlSideReply *, void *) {
    volatile int *p = nullptr;
    return *p;
}
} // namespace

MML_EXPORT uint32_t mml_side_api(void) { return MML_SIDE_API_VERSION; }

MML_EXPORT int mml_side_load(const MmlSideHost *host, MmlSideMod *mod) {
    H = host;
    g_frames = 0;
    g_loads++;
    H->subscribe(mod, "FRAME", OnFrame, nullptr);
    H->command(mod, "echo", "args -> {args, pointer_bits}", Echo, nullptr);
    H->command(mod, "frames", "{} -> the FRAME events seen", Frames, nullptr);
    H->command(mod, "call", "{method, params} -> that bridge call's answer, made from the 64-bit side", Call, nullptr);
    H->command(mod, "write", "args -> written to the channel side_test.box", Write, nullptr);
    H->command(mod, "fetch", "{} -> {text, version}: the channel back (its 32-bit half writes it)", Fetch, nullptr);
    H->command(mod, "flood", "{} -> the channel flood written by a thread for 1.5 s, as fast as it can", Flood, nullptr);
    H->command(mod, "fault", "faults (its half is unloaded)", Fault, nullptr);
    H->command(mod, "write_later", "{ms} -> box written by a thread after that long", WriteLater, nullptr);
    H->command(mod, "wait_back", "{wait_ms} -> {after, version, ms}: channel_wait for back's next version", WaitBack, nullptr);
    g_ch = H->channel_open(mod, "box", 4096);
    g_back = H->channel_open(mod, "back", 4096);
    g_flood = H->channel_open(mod, "flood", 4096);
    H->log(mod, 1, "side_test loaded (pointer bits %u)", static_cast<unsigned>(sizeof(void *) * 8));
    return 0;
}

MML_EXPORT void mml_side_unload(MmlSideMod *) {
    g_stop = true;
    if (g_flooder.joinable()) g_flooder.join();
    if (g_later.joinable()) g_later.join();
    g_ch = g_back = g_flood = nullptr;
}
