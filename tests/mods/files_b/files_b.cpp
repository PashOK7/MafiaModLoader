// Test mod: a mod's own files in the game (tests/run_tests.py files_tests). It ships files\ of its own (copied by
// the test: tables\test.txt over files_a's, archived.txt over the stand-in archive's) and opens files as the game
// does: rw_data.dll's dtaOpen export, called directly. Commands:
//   files_b.open {name}             the file the game gets for a name: {"ok", "text"}
//   files_b.hold {name}, .release   a file held open (the game streaming a sound); release reads it and closes it
//   files_b.add {name, path}        file_add (path "" takes it back): {"rc"}
//   files_b.write {file, text}      a file written into the mod's data folder: {"path"}
//   files_b.source {name}           file_source: {"rc", "path"}
//   files_b.read {name, game}       file_read (game: MML_FILE_GAME): {"size", "text"}
//   files_b.status                  the MML_EVENT_FILE_CHANGED this load got: {"events", "names"}
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
using DtaOpenFn = uint32_t(__stdcall *)(const char *, int);
using DtaReadFn = uint32_t(__stdcall *)(uint32_t, void *, uint32_t);
using DtaCloseFn = uint32_t(__stdcall *)(uint32_t);
DtaOpenFn dtaOpen;
DtaReadFn dtaRead;
DtaCloseFn dtaClose;
uint32_t g_held = 0xFFFFFFFFu;
unsigned g_events;
std::vector<std::string> g_names;

// A string member of the command's JSON arguments (the tests send plain ones: only \\ and \" are escaped).
std::string Arg(const char *args, const char *key) {
    std::string a = args ? args : "", k = std::string("\"") + key + "\"";
    size_t p = a.find(k);
    if (p == std::string::npos) return "";
    p = a.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p = a.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos) return "";
    if (a[p] != '"') return a.substr(p, a.find_first_of(",}", p) - p);   // a number or true/false
    std::string out;
    for (size_t i = p + 1; i < a.size() && a[i] != '"'; i++) {
        if (a[i] == '\\' && i + 1 < a.size()) i++;
        out += a[i];
    }
    return out;
}
std::string Esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n') { o += "\\n"; continue; }
        if (c == '\r') { o += "\\r"; continue; }
        o += c;
    }
    return o;
}
int Reply(MmlReply *r, const std::string &json) {
    H->reply_append(r, json.c_str(), json.size());
    return 0;
}
std::string ReadAll(uint32_t h) {
    std::string text;
    char buf[256];
    for (;;) {
        uint32_t n = dtaRead(h, buf, sizeof buf);
        if (n == 0xFFFFFFFFu || n == 0) break;
        text.append(buf, n);
    }
    return text;
}

int Open(const char *args, MmlReply *r, void *) {
    uint32_t h = dtaOpen(Arg(args, "name").c_str(), 0);
    if (h == 0xFFFFFFFFu) return Reply(r, "{\"ok\":false}");
    std::string text = ReadAll(h);
    dtaClose(h);
    return Reply(r, "{\"ok\":true,\"text\":\"" + Esc(text) + "\"}");
}
int Hold(const char *args, MmlReply *r, void *) {
    if (g_held != 0xFFFFFFFFu) dtaClose(g_held);
    g_held = dtaOpen(Arg(args, "name").c_str(), 0);
    return Reply(r, g_held == 0xFFFFFFFFu ? "{\"ok\":false}" : "{\"ok\":true}");
}
int Release(const char *, MmlReply *r, void *) {
    if (g_held == 0xFFFFFFFFu) return Reply(r, "{\"ok\":false}");
    std::string text = ReadAll(g_held);
    dtaClose(g_held);
    g_held = 0xFFFFFFFFu;
    return Reply(r, "{\"ok\":true,\"text\":\"" + Esc(text) + "\"}");
}
int Add(const char *args, MmlReply *r, void *) {
    int rc = H->file_add(M, Arg(args, "name").c_str(), Arg(args, "path").c_str());
    return Reply(r, "{\"rc\":" + std::to_string(rc) + "}");
}
int Write(const char *args, MmlReply *r, void *) {
    std::string path = std::string(H->mod_data_dir(M)) + Arg(args, "file"), text = Arg(args, "text");
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { H->reply_error(r, "cannot write"); return 1; }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return Reply(r, "{\"path\":\"" + Esc(path) + "\"}");
}
int Source(const char *args, MmlReply *r, void *) {
    char path[MAX_PATH] = "?";
    int rc = H->file_source(Arg(args, "name").c_str(), path, sizeof path);
    return Reply(r, "{\"rc\":" + std::to_string(rc) + ",\"path\":\"" + Esc(path) + "\"}");
}
int Read(const char *args, MmlReply *r, void *) {
    std::string name = Arg(args, "name");
    int flags = Arg(args, "game") == "true" ? MML_FILE_GAME : 0;
    int64_t size = H->file_read(M, name.c_str(), nullptr, 0, flags);
    std::string text;
    if (size > 0) {
        text.resize(static_cast<size_t>(size));
        int64_t again = H->file_read(M, name.c_str(), &text[0], text.size(), flags);
        if (again != size) text = "?";
    }
    return Reply(r, "{\"size\":" + std::to_string(size) + ",\"text\":\"" + Esc(text) + "\"}");
}
int Status(const char *, MmlReply *r, void *) {
    std::string names;
    for (const std::string &n : g_names) names += (names.empty() ? "\"" : ",\"") + Esc(n) + "\"";
    return Reply(r, "{\"events\":" + std::to_string(g_events) + ",\"names\":[" + names + "]}");
}

void OnEvent(const MmlEvent *ev, void *) {
    g_events++;
    g_names.push_back(ev->name ? ev->name : "");
}
} // namespace

extern "C" __declspec(dllexport) int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, file_read)) return 1;
    HMODULE rw = GetModuleHandleA("rw_data.dll");
    dtaOpen = rw ? reinterpret_cast<DtaOpenFn>(GetProcAddress(rw, "_dtaOpen@8")) : nullptr;
    dtaRead = rw ? reinterpret_cast<DtaReadFn>(GetProcAddress(rw, "_dtaRead@12")) : nullptr;
    dtaClose = rw ? reinterpret_cast<DtaCloseFn>(GetProcAddress(rw, "_dtaClose@4")) : nullptr;
    if (!dtaOpen || !dtaRead || !dtaClose) return 2;
    H->subscribe(M, MML_EVENT_FILE_CHANGED, OnEvent, nullptr);
    H->command(M, "open", "the file the game gets for {name}", Open, nullptr);
    H->command(M, "hold", "{name} held open", Hold, nullptr);
    H->command(M, "release", "the held file read and closed", Release, nullptr);
    H->command(M, "add", "file_add {name, path}", Add, nullptr);
    H->command(M, "write", "{file, text} into the data folder", Write, nullptr);
    H->command(M, "source", "file_source {name}", Source, nullptr);
    H->command(M, "read", "file_read {name, game}", Read, nullptr);
    H->command(M, "status", "the file events seen", Status, nullptr);
    return 0;
}

extern "C" __declspec(dllexport) void mml_mod_unload(MmlMod *) {
    if (g_held != 0xFFFFFFFFu) dtaClose(g_held);
    g_held = 0xFFFFFFFFu;
}
