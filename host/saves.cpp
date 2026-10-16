// A mod's state in the game's saves.
//
// The game writes a save with G_LoadSaveClass::SaveGameSave and reads one with SaveGameLoad (game_build.h
// saveGameSave, saveGameLoad): savegame\mafia<profile>.<slot>, the profile and the slot being the profile manager's
// +0x28 and +0x7C. Beside each save the loader writes the mods' state of that game, savegame\mafia<p>.<s>.mml, and
// reads it back when the game loads that save; a new game or a freeride (SaveGameNewGame) starts empty. The game's own
// files are never touched: an old save without a .mml simply has no mod state, and a .mml without its save is never
// read. The profile's state (savegame\mafia<p>.sav.mml) is written as soon as a mod changes it; ProfileDelete takes
// the profile's .mml files with its own.
//
// Mods: save_put / save_get by scope (MML_SAVE_SLOT, MML_SAVE_PROFILE) and key, kept under the mod's id (its
// reloads keep it). MML_EVENT_GAME_SAVING is sent before the game writes (a mod puts its state then), and
// MML_EVENT_GAME_LOADED after a save is loaded or a new game began (slot -1).
//
// File: "MMLS", u32 version 1, u32 count, then count records {u16 mod's id length, the id, u16 key length, the key,
// u32 size, the bytes}. Written to a .tmp, the old file deleted, the new one renamed into place.
//
// Bridge methods: save.state, save.get {mod, key, scope}, save.put {key, value, scope} (the bridge's own keys).
#include "host.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

using Blobs = std::map<std::string, std::map<std::string, std::string>>;   // mod id -> key -> bytes
constexpr uint32_t kMagic = 0x534C4D4D;   // "MMLS"
constexpr size_t kMaxKey = 128, kMaxBlob = 16u << 20;

std::recursive_mutex g_lock;
Blobs g_slot;                            // the game being played
std::map<int, Blobs> g_profiles;         // read on first use
int g_slotNow = -1, g_profileLoaded = -1;
unsigned g_saves, g_loads, g_news;
std::string g_lastFile, g_lastError;

using SaveFn = int(__fastcall *)(void *mgr, void *edx, int quick);
using LoadFn = int(__fastcall *)(void *mgr, void *edx);
using NewFn = void(__fastcall *)(void *mgr, void *edx);
using DeleteFn = int(__fastcall *)(void *mgr, void *edx, unsigned index);
SaveFn oSave;
LoadFn oLoad;
NewFn oNew;
DeleteFn oDelete;
bool g_hooked;

const game::ExeBuild &E() { return game::Exe(); }

template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

int ManagerInt(int off) {
    int v = -1;
    auto f = [&] { v = game::at<int>(reinterpret_cast<void *>(E().loadSave), off); };
    if (!game::g_exe || !E().loadSave || Guarded(f)) return -1;
    return v;
}
int ProfileNow() { return game::g_exe ? ManagerInt(E().saveProfile) : -1; }
int SlotNow() { return game::g_exe ? ManagerInt(E().saveSlot) : -1; }

std::string SlotFile(int profile, int slot) {
    char b[64];
    snprintf(b, sizeof b, "savegame\\mafia%03d.%03d.mml", profile, slot);
    return GameDir() + b;
}
std::string ProfileFile(int profile) {
    char b[64];
    snprintf(b, sizeof b, "savegame\\mafia%03d.sav.mml", profile);
    return GameDir() + b;
}
std::string GameSaveFile(int profile, int slot) {
    char b[64];
    snprintf(b, sizeof b, "savegame\\mafia%03d.%03d", profile, slot);
    return GameDir() + b;
}

bool ReadBlobs(const std::string &path, Blobs *out) {
    out->clear();
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<char> d;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    fclose(f);
    size_t at = 0;
    auto take = [&](void *dst, size_t len) {
        if (at + len > d.size()) return false;
        memcpy(dst, d.data() + at, len);
        at += len;
        return true;
    };
    uint32_t magic = 0, version = 0, count = 0;
    if (!take(&magic, 4) || !take(&version, 4) || !take(&count, 4) || magic != kMagic || version != 1) {
        g_lastError = path + ": not a mod state file of version 1";
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint16_t ml = 0, kl = 0;
        uint32_t size = 0;
        std::string mod, key, data;
        if (!take(&ml, 2)) return false;
        mod.resize(ml);
        if (!take(mod.data(), ml) || !take(&kl, 2)) return false;
        key.resize(kl);
        if (!take(key.data(), kl) || !take(&size, 4) || size > kMaxBlob) return false;
        data.resize(size);
        if (!take(data.data(), size)) return false;
        (*out)[mod][key] = std::move(data);
    }
    return true;
}

bool WriteBlobs(const std::string &path, const Blobs &b) {
    uint32_t count = 0;
    for (const auto &m : b) count += static_cast<uint32_t>(m.second.size());
    if (!count) {   // nothing to keep: no file
        DeleteFileA(path.c_str());
        return true;
    }
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) { g_lastError = "cannot write " + tmp; return false; }
    const uint32_t head[3] = {kMagic, 1, count};
    bool ok = fwrite(head, 4, 3, f) == 3;
    for (const auto &m : b)
        for (const auto &k : m.second) {
            uint16_t ml = static_cast<uint16_t>(m.first.size()), kl = static_cast<uint16_t>(k.first.size());
            uint32_t size = static_cast<uint32_t>(k.second.size());
            ok = ok && fwrite(&ml, 2, 1, f) == 1 && fwrite(m.first.data(), 1, ml, f) == ml && fwrite(&kl, 2, 1, f) == 1 &&
                 fwrite(k.first.data(), 1, kl, f) == kl && fwrite(&size, 4, 1, f) == 1 && fwrite(k.second.data(), 1, size, f) == size;
        }
    ok = fclose(f) == 0 && ok;
    if (!ok) { DeleteFileA(tmp.c_str()); g_lastError = "writing " + tmp + " failed"; return false; }
    DeleteFileA(path.c_str());   // (a rename over an open file is refused; a reader keeps the old one)
    if (!MoveFileA(tmp.c_str(), path.c_str())) { g_lastError = "cannot rename " + tmp; return false; }
    return true;
}

Blobs &Profile(int profile) {
    auto it = g_profiles.find(profile);
    if (it != g_profiles.end()) return it->second;
    Blobs &b = g_profiles[profile];
    if (profile >= 0) ReadBlobs(ProfileFile(profile), &b);
    return b;
}

void Tell(MmlEventType type, int slot, int profile) {
    MmlEvent ev{};
    ev.size = sizeof ev;
    ev.type = type;
    ev.slot = slot;
    ev.profile = profile;
    Dispatch(ev);
}

int __fastcall hkSave(void *mgr, void *edx, int quick) {
    int profile = ProfileNow(), slot = SlotNow();
    Tell(MML_EVENT_GAME_SAVING, slot, profile);   // (the mods put their state now)
    int r = oSave(mgr, edx, quick);
    std::lock_guard<std::recursive_mutex> g(g_lock);
    if (profile >= 0 && slot >= 0 && GetFileAttributesA(GameSaveFile(profile, slot).c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_lastFile = SlotFile(profile, slot);
        if (WriteBlobs(g_lastFile, g_slot)) g_saves++;
        else Log(MML_LOG_WARN, "host", "saves: %s", g_lastError.c_str());
        g_slotNow = slot;
    }
    return r;
}

int __fastcall hkLoad(void *mgr, void *edx) {
    int profile = ProfileNow(), slot = SlotNow();
    {
        std::lock_guard<std::recursive_mutex> g(g_lock);
        g_lastFile = SlotFile(profile, slot);
        Blobs b;
        ReadBlobs(g_lastFile, &b);   // (none: this save has no mod state)
        g_slot.swap(b);
        g_slotNow = slot;
        g_loads++;
    }
    int r = oLoad(mgr, edx);
    Tell(MML_EVENT_GAME_LOADED, slot, profile);
    return r;
}

void __fastcall hkNew(void *mgr, void *edx) {
    oNew(mgr, edx);
    {
        std::lock_guard<std::recursive_mutex> g(g_lock);
        g_slot.clear();
        g_slotNow = -1;
        g_news++;
    }
    Tell(MML_EVENT_GAME_LOADED, -1, ProfileNow());
}

int __fastcall hkDelete(void *mgr, void *edx, unsigned index) {
    int profile = -1;
    auto f = [&] {
        uint8_t *list = game::at<uint8_t *>(mgr, 4);
        if (list) profile = *reinterpret_cast<int *>(list + 0x54 * index + 8);
    };
    Guarded(f);
    int r = oDelete(mgr, edx, index);
    if (profile >= 0) {
        std::lock_guard<std::recursive_mutex> g(g_lock);
        DeleteFileA(ProfileFile(profile).c_str());
        char pat[64];
        snprintf(pat, sizeof pat, "savegame\\mafia%03d.*.mml", profile);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((GameDir() + pat).c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do DeleteFileA((GameDir() + "savegame\\" + fd.cFileName).c_str());
            while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        g_profiles.erase(profile);
    }
    return r;
}

std::string Who(MmlMod *mod) { return mod ? ModIdOf(mod) : "bridge"; }

} // namespace

int SaveProfileNumber() { return ProfileNow(); }

void SavesInit() {
    if (!game::g_exe || !E().saveGameSave || !E().saveGameLoad || !E().saveGameNew || !E().profileDelete || !E().loadSave) return;
    std::string err;
    g_hooked = HookAdd(nullptr, E().saveGameSave, reinterpret_cast<void *>(&hkSave), reinterpret_cast<void **>(&oSave), &err) &&
               HookAdd(nullptr, E().saveGameLoad, reinterpret_cast<void *>(&hkLoad), reinterpret_cast<void **>(&oLoad), &err) &&
               HookAdd(nullptr, E().saveGameNew, reinterpret_cast<void *>(&hkNew), reinterpret_cast<void **>(&oNew), &err) &&
               HookAdd(nullptr, E().profileDelete, reinterpret_cast<void *>(&hkDelete), reinterpret_cast<void **>(&oDelete), &err);
    if (!g_hooked) HWARN("saves: the game's saves are not hooked (no mod state in them): %s", err.c_str());
}

int SavePut(MmlMod *mod, int scope, const char *key, const void *data, size_t n, std::string *err) {
    return SavePutAs(Who(mod), scope, key, data, n, err);
}
int SavePutAs(const std::string &who, int scope, const char *key, const void *data, size_t n, std::string *err) {
    if (scope != MML_SAVE_SLOT && scope != MML_SAVE_PROFILE) { *err = "scope: MML_SAVE_SLOT or MML_SAVE_PROFILE"; return 0; }
    if (!key || !*key || strlen(key) > kMaxKey) { *err = "key: 1..128 characters"; return 0; }
    if (n > kMaxBlob) { *err = "at most 16 MB a key"; return 0; }
    std::lock_guard<std::recursive_mutex> g(g_lock);
    int profile = ProfileNow();
    if (scope == MML_SAVE_PROFILE && profile < 0) { *err = "no profile selected yet"; return 0; }
    Blobs &b = scope == MML_SAVE_SLOT ? g_slot : Profile(profile);
    if (!data || !n) {
        auto it = b.find(who);
        if (it != b.end()) { it->second.erase(key); if (it->second.empty()) b.erase(it); }
    } else {
        b[who][key].assign(static_cast<const char *>(data), n);
    }
    if (scope == MML_SAVE_PROFILE && !WriteBlobs(ProfileFile(profile), b)) { *err = g_lastError; return 0; }
    return 1;
}

size_t SaveGet(MmlMod *mod, int scope, const char *key, void *data, size_t cap) { return SaveGetAs(Who(mod), scope, key, data, cap); }
size_t SaveGetAs(const std::string &who, int scope, const char *key, void *data, size_t cap) {
    if (!key || (scope != MML_SAVE_SLOT && scope != MML_SAVE_PROFILE)) return 0;
    std::lock_guard<std::recursive_mutex> g(g_lock);
    int profile = ProfileNow();
    if (scope == MML_SAVE_PROFILE && profile < 0) return 0;
    const Blobs &b = scope == MML_SAVE_SLOT ? g_slot : Profile(profile);
    auto m = b.find(who);
    if (m == b.end()) return 0;
    auto k = m->second.find(key);
    if (k == m->second.end()) return 0;
    if (data && cap) memcpy(data, k->second.data(), k->second.size() < cap ? k->second.size() : cap);
    return k->second.size();
}

bool SavesMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "save.state" && method != "save.get" && method != "save.put") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    auto scopeOf = [&](int *scope) {
        std::string s = p.value("scope", "slot");
        *scope = s == "profile" ? MML_SAVE_PROFILE : s == "slot" ? MML_SAVE_SLOT : -1;
        return *scope >= 0;
    };
    // a blob shown as text when it is printable UTF-8-ish, else as hex
    auto show = [](const std::string &v) {
        bool text = true;
        for (unsigned char c : v) if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') { text = false; break; }
        if (text) return json(v);
        std::string h;
        char b[4];
        for (unsigned char c : v) { snprintf(b, sizeof b, "%02X", c); h += b; }
        return json({{"hex", h}});
    };
    try {
        std::lock_guard<std::recursive_mutex> g(g_lock);
        if (method == "save.state") {
            auto list = [](const Blobs &b) {
                json l = json::array();
                for (const auto &m : b)
                    for (const auto &k : m.second) l.push_back({{"mod", m.first}, {"key", k.first}, {"size", k.second.size()}});
                return l;
            };
            int profile = ProfileNow();
            json out = {{"hooked", g_hooked}, {"profile", profile}, {"slot", SlotNow()}, {"slot_loaded", g_slotNow},
                        {"slot_keys", list(g_slot)}, {"saves_written", g_saves}, {"saves_loaded", g_loads}, {"new_games", g_news},
                        {"last_file", g_lastFile}, {"last_error", g_lastError}};
            if (profile >= 0) { out["profile_keys"] = list(Profile(profile)); out["profile_file"] = ProfileFile(profile); }
            *reply = dump(out);
        } else if (method == "save.get") {
            int scope;
            if (!scopeOf(&scope)) { *reply = "scope: \"slot\" (default) or \"profile\""; return true; }
            std::string mod = p.value("mod", "bridge"), key = p.value("key", "");
            int profile = ProfileNow();
            if (scope == MML_SAVE_PROFILE && profile < 0) { *reply = "save.get: no profile selected yet"; return true; }
            const Blobs &b = scope == MML_SAVE_SLOT ? g_slot : Profile(profile);
            auto m = b.find(mod);
            if (m == b.end() || !m->second.count(key)) { *reply = "save.get: no such key for " + mod + " (save.state lists them)"; return true; }
            *reply = dump({{"mod", mod}, {"key", key}, {"size", m->second.at(key).size()}, {"value", show(m->second.at(key))}});
        } else {   // save.put: the bridge's own keys
            int scope;
            if (!scopeOf(&scope)) { *reply = "scope: \"slot\" (default) or \"profile\""; return true; }
            std::string key = p.value("key", ""), err;
            std::string value = p.contains("value") ? (p["value"].is_string() ? p["value"].get<std::string>() : p["value"].dump()) : "";
            if (!SavePut(nullptr, scope, key.c_str(), value.data(), value.size(), &err)) { *reply = "save.put: " + err; return true; }
            *reply = dump({{"key", key}, {"size", value.size()}, {"scope", scope == MML_SAVE_SLOT ? "slot" : "profile"}});
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
