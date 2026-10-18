// side_test's 32-bit half (tests/run_tests.py sidecar_tests): the channels both halves see. It reads "box" (the
// 64-bit half's `write`), writes "back" (the 64-bit half's `fetch` reads it), and tells what channel_open and
// gfx_shared_texture give where they must refuse.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
MmlChannel *g_box, *g_back, *g_flood;
std::thread g_later;

uint32_t Num(const char *args, const char *key, uint32_t def) {   // (the tests send flat objects of numbers)
    std::string a = args ? args : "", k = std::string("\"") + key + "\"";
    size_t at = a.find(k);
    return at == std::string::npos ? def : static_cast<uint32_t>(strtoul(a.c_str() + a.find(':', at) + 1, nullptr, 10));
}

void Reply(MmlReply *r, const std::string &s) { H->reply_append(r, s.data(), s.size()); }

std::string Quoted(const std::string &s) {   // (the tests write plain text: quotes and backslashes escaped)
    std::string q = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') q += '\\';
        q += c;
    }
    return q + "\"";
}

// {} -> {"text", "bytes", "version"}: box's latest whole version
int Peek(const char *, MmlReply *r, void *) {
    char buf[4096];
    uint64_t version = 0;
    uint32_t n = H->channel_read(g_box, buf, sizeof buf, &version);
    Reply(r, "{\"text\": " + Quoted(std::string(buf, n)) + ", \"bytes\": " + std::to_string(n) + ", \"version\": " + std::to_string(version) + "}");
    return 0;
}
// args -> written as back's next version
int Post(const char *args, MmlReply *r, void *) {
    std::string a = args ? args : "";
    int ok = H->channel_write(g_back, a.data(), static_cast<uint32_t>(a.size()));
    Reply(r, "{\"written\": " + std::to_string(ok ? a.size() : 0) + "}");
    return 0;
}
// {} -> {"reads", "versions", "torn", "backwards"}: the channel flood read for 1 s as fast as it can (its 64-bit
// half's `flood` writes it meanwhile): versions seen, reads whose bytes were not one version's, versions gone back
int Torn(const char *, MmlReply *r, void *) {
    static unsigned char buf[4096];
    unsigned long long reads = 0, versions = 0, torn = 0, backwards = 0, last = 0;
    auto end = GetTickCount64() + 1000;
    while (GetTickCount64() < end) {
        uint64_t version = 0;
        uint32_t n = H->channel_read(g_flood, buf, sizeof buf, &version);
        if (!version) continue;
        reads++;
        unsigned long long inside = 0;
        memcpy(&inside, buf, 8);
        bool whole = n == sizeof buf && inside == version;
        for (uint32_t i = 8; whole && i < n; i++) whole = buf[i] == static_cast<unsigned char>(inside & 0xFF);
        if (!whole) torn++;
        if (version < last) backwards++;
        if (version != last) versions++;
        last = version;
    }
    char out[200];
    snprintf(out, sizeof out, "{\"reads\": %llu, \"versions\": %llu, \"torn\": %llu, \"backwards\": %llu}", reads, versions, torn, backwards);
    Reply(r, out);
    return 0;
}
// {"wait_ms"} -> {"after", "version", "ms"}: channel_wait for a version of box after the latest one (0: none came)
int WaitBox(const char *args, MmlReply *r, void *) {
    char buf[16];
    uint64_t after = 0;
    H->channel_read(g_box, buf, sizeof buf, &after);
    ULONGLONG t0 = GetTickCount64();
    uint64_t v = H->channel_wait(g_box, after, Num(args, "wait_ms", 1000));
    Reply(r, "{\"after\": " + std::to_string(after) + ", \"version\": " + std::to_string(v) + ", \"ms\": " + std::to_string(GetTickCount64() - t0) + "}");
    return 0;
}
// {"ms"}: a thread of this half writes "later" to back after that long (the 64-bit half's wait_back waits for it)
int PostLater(const char *args, MmlReply *r, void *) {
    if (g_later.joinable()) g_later.join();
    uint32_t ms = Num(args, "ms", 300);
    g_later = std::thread([ms] {
        Sleep(ms);
        H->channel_write(g_back, "later", 5);
    });
    Reply(r, "{\"started\": true}");
    return 0;
}
// {} -> what must be refused: box opened with another size, a write longer than the channel, a texture outside a pass
int Refusals(const char *, MmlReply *r, void *) {
    MmlChannel *other = H->channel_open(M, "box", 100);
    std::string big(5000, 'x');
    int tooLong = H->channel_write(g_back, big.data(), static_cast<uint32_t>(big.size()));
    MmlSharedTexture t{};
    t.size = sizeof t;
    int tex = H->gfx_shared_texture(M, "picture", &t);
    Reply(r, "{\"other_size\": " + std::string(other ? "true" : "false") + ", \"too_long\": " + std::to_string(tooLong) +
             ", \"texture_outside_pass\": " + std::to_string(tex) + "}");
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, gfx_shared_texture)) return 1;
    g_box = H->channel_open(mod, "box", 4096);
    g_back = H->channel_open(mod, "back", 4096);
    g_flood = H->channel_open(mod, "flood", 4096);
    host->command(mod, "torn", "{} -> the channel flood read for 1 s: {reads, versions, torn, backwards}", Torn, nullptr);
    host->command(mod, "peek", "{} -> {text, bytes, version}: the channel box's latest version (the 64-bit half writes it)", Peek, nullptr);
    host->command(mod, "post", "args -> written to the channel back (the 64-bit half's fetch reads it)", Post, nullptr);
    host->command(mod, "refusals", "{} -> channel_open of another size, a write too long, gfx_shared_texture outside a pass", Refusals, nullptr);
    if (MML_HAS(H, channel_wait)) {
        host->command(mod, "wait_box", "{wait_ms} -> {after, version, ms}: channel_wait for box's next version", WaitBox, nullptr);
        host->command(mod, "post_later", "{ms} -> back written by a thread after that long", PostLater, nullptr);
    }
    return g_box && g_back && g_flood ? 0 : 1;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    if (g_later.joinable()) g_later.join();
    g_box = g_back = g_flood = nullptr;
}
