// side_test: the offline tests' 64-bit half (tests/run_tests.py sidecar_tests): its commands answer from the sidecar,
// it calls the bridge, counts the frames it is told about, writes a channel, faults when asked.
#include <cstdio>
#include <cstring>
#include <string>
#include "mml/side.h"

namespace {
const MmlSideHost *H;
unsigned long long g_frames;
MmlSideChannel *g_ch;
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
    H->command(mod, "fault", "faults (its half is unloaded)", Fault, nullptr);
    g_ch = H->channel_open(mod, "box", 4096);
    H->log(mod, 1, "side_test loaded (pointer bits %u)", static_cast<unsigned>(sizeof(void *) * 8));
    return 0;
}

MML_EXPORT void mml_side_unload(MmlSideMod *) { g_ch = nullptr; }
