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
//
// Restore points (restore.save, restore.load, restore.list, restore.delete): an experiment repeated from the same
// moment, by the game's own save and load. restore.save calls SaveGameSave whole (quick = 1: every actor of the
// mission and its programs; 0, the scripts' AUTOSAVEGAME, keeps only the temporary actors and is what made a bridge's
// save load into a Game Over once) into slot 599, which no save of the game uses (1.2 names each save by its text
// 5000 + slot: 5590 "Freeride Extreme - Subquest" is the last, 5591..5599 are none; 1.0's texts name the chapters
// only, its missions are the same; a save found in slot 599 all the same stops the restore points, never
// overwritten), then moves the file and its mods' state to <game>\mml\restore\<name>.*; the
// slot the game had is put back, so the profile's own saves and its list are untouched. restore.load copies them back
// into slot 599 and sets C_game's state to "load" (game.h kGameState): the game loop loads it at its own place, the
// mission opened again only when the point is another mission's; then the slot is put back, the one-shot flag that
// skips the next script save cleared (the point was not made by a script), the files removed, RESTORE_LOADED told.
#include "host.h"
#include <cstdio>
#include <cctype>
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

// restore points
constexpr int kRestoreSlot = 599;
struct Restoring { bool active = false; std::string name, mission; int prevSlot = -1, prevSlotNow = -1; DWORD started = 0; } g_restoring;

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
    FILE *f = FileOpen(path, "rb");
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
        PathDelete(path);
        return true;
    }
    std::string tmp = path + ".tmp";
    FILE *f = FileOpen(tmp, "wb");
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
    if (!ok) { PathDelete(tmp); g_lastError = "writing " + tmp + " failed"; return false; }
    PathDelete(path);   // (a rename over an open file is refused; a reader keeps the old one)
    if (!PathMove(tmp, path)) { g_lastError = "cannot rename " + tmp; return false; }
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
    if (profile >= 0 && slot >= 0 && PathExists(GameSaveFile(profile, slot))) {
        g_lastFile = SlotFile(profile, slot);
        if (WriteBlobs(g_lastFile, g_slot)) g_saves++;
        else Log(MML_LOG_WARN, "host", "saves: %s", g_lastError.c_str());
        g_slotNow = slot;
    }
    return r;
}

void SetSlot(int slot) {
    auto f = [&] { game::at<int>(reinterpret_cast<void *>(E().loadSave), E().saveSlot) = slot; };
    Guarded(f);
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
    if (g_restoring.active && slot == kRestoreSlot) {
        // a restore point loaded: the game's slot, its next script save and the files as they were
        Restoring done = g_restoring;
        g_restoring.active = false;
        SetSlot(done.prevSlot);
        auto f = [&] { game::at<uint8_t>(mgr, game::kSaveSuppress) = 0; };
        Guarded(f);
        PathDelete(GameSaveFile(profile, kRestoreSlot));
        PathDelete(SlotFile(profile, kRestoreSlot));
        {
            std::lock_guard<std::recursive_mutex> g(g_lock);
            g_slotNow = done.prevSlotNow;
        }
        HLOG("restore point '%s' loaded (%s) in %u ms", done.name.c_str(), MissionName(), static_cast<unsigned>(GetTickCount() - done.started));
        EventsRecord("RESTORE_LOADED", json({{"name", done.name}, {"mission", MissionName()},
                                             {"ms", GetTickCount() - done.started}}).dump(-1, ' ', false, json::error_handler_t::replace));
    }
    HappenGameLoaded();   // (the money and the items as loaded are no change the player made)
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
    HappenGameLoaded();
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
        PathDelete(ProfileFile(profile));
        char pat[64];
        snprintf(pat, sizeof pat, "savegame\\mafia%03d.*.mml", profile);
        for (const FoundFile &fd : ListDir(GameDir() + pat)) PathDelete(GameDir() + "savegame\\" + fd.name);
        g_profiles.erase(profile);
    }
    return r;
}

std::string Who(MmlMod *mod) { return mod ? ModIdOf(mod) : "bridge"; }

// ---- restore points
std::string RestoreDir() { return MmlDir() + "restore\\"; }

bool RestoreName(const std::string &n) {
    if (n.empty() || n.size() > 48) return false;
    for (char c : n) if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    return n[0] != '.';
}

DWORD CallSave() {   // SaveGameSave through its hooked entry (hkSave: the mods put their state, the .mml written)
    auto save = reinterpret_cast<SaveFn>(E().saveGameSave);
    __try { save(reinterpret_cast<void *>(E().loadSave), nullptr, 1); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

bool FileSize(const std::string &path, uint64_t *size) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!PathAttributesEx(path, &a)) return false;
    *size = (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}

// Slot 599 must be free: a save there is not ours (ours never stays) and is never overwritten.
bool SlotFree(int profile, std::string *err) {
    if (!PathExists(GameSaveFile(profile, kRestoreSlot))) return true;
    *err = GameSaveFile(profile, kRestoreSlot) + " holds a save the loader did not make: no restore points while it is there (move it away)";
    return false;
}

bool Move(const std::string &from, const std::string &to) {
    PathDelete(to);
    return PathMove(from, to);
}

json ReadMeta(const std::string &name) {
    FILE *f = FileOpen(RestoreDir() + name + ".json", "rb");
    if (!f) return nullptr;
    std::string s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return json::parse(s, nullptr, false);
}

// restore.save {name}: the game as it is now, whole, kept under a name.
json RestoreSave(const json &p, std::string *err) {
    std::string name = p.value("name", "");
    if (!RestoreName(name)) { *err = "name: 1..48 letters, digits, '_', '-' or '.'"; return nullptr; }
    if (!game::g_exe || !g_hooked) { *err = "not a known build of the game (or its saves are not hooked)"; return nullptr; }
    if (g_restoring.active) { *err = "a restore point is being loaded"; return nullptr; }
    std::string mission = MissionName();
    if (MissionState() != MML_MISSION_RUNNING || mission.empty() || mission == "00menu") {
        *err = "no mission being played (the main menu, a load or the pause menu): a restore point is a game in play"; return nullptr;
    }
    int profile = ProfileNow(), prev = SlotNow(), prevNow = g_slotNow;
    if (profile < 0) { *err = "no profile"; return nullptr; }
    if (!SlotFree(profile, err)) return nullptr;
    DWORD t0 = GetTickCount();
    SetSlot(kRestoreSlot);
    DWORD code = CallSave();
    SetSlot(prev);
    {
        std::lock_guard<std::recursive_mutex> g(g_lock);
        g_slotNow = prevNow;
    }
    std::string sav = GameSaveFile(profile, kRestoreSlot), mml = SlotFile(profile, kRestoreSlot);
    uint64_t bytes = 0;
    if (code || !FileSize(sav, &bytes)) {
        PathDelete(sav);
        PathDelete(mml);
        char b[96];
        snprintf(b, sizeof b, "SaveGameSave %s", code ? "faulted" : "wrote no file");
        *err = b;
        return nullptr;
    }
    MakeDir(RestoreDir());
    std::string base = RestoreDir() + name;
    if (!Move(sav, base + ".sav")) { PathDelete(sav); PathDelete(mml); *err = "cannot move the save to " + base + ".sav"; return nullptr; }
    bool mods = PathExists(mml);
    PathDelete(base + ".mml");
    if (mods && !Move(mml, base + ".mml")) mods = false;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char made[32];
    snprintf(made, sizeof made, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    json meta = {{"name", name}, {"mission", mission}, {"build", GameBuild()}, {"profile", profile}, {"made", made},
                 {"bytes", bytes}, {"mods_state", mods}};
    FILE *f = FileOpen(base + ".json", "wb");
    if (f) { std::string s = meta.dump(1, ' ', false, json::error_handler_t::replace); fwrite(s.data(), 1, s.size(), f); fclose(f); }
    meta["ms"] = GetTickCount() - t0;
    HLOG("restore point '%s' saved (%s, %u bytes)", name.c_str(), mission.c_str(), static_cast<unsigned>(bytes));
    return meta;
}

// restore.load {name}: the game loop loads it at its next frame (RESTORE_LOADED tells when).
json RestoreLoad(const json &p, std::string *err) {
    std::string name = p.value("name", "");
    if (!RestoreName(name)) { *err = "name: 1..48 letters, digits, '_', '-' or '.'"; return nullptr; }
    if (!game::g_exe || !g_hooked) { *err = "not a known build of the game (or its saves are not hooked)"; return nullptr; }
    if (g_restoring.active) { *err = "a restore point is being loaded already: '" + g_restoring.name + "'"; return nullptr; }
    std::string base = RestoreDir() + name;
    json meta = ReadMeta(name);
    if (!PathExists(base + ".sav") || !meta.is_object()) {
        *err = "no restore point '" + name + "' (restore.list)"; return nullptr;
    }
    if (meta.value("build", "") != GameBuild()) { *err = "the point was made in the game " + meta.value("build", "?") + ", this is " + GameBuild(); return nullptr; }
    void *g = game::Game();
    int profile = ProfileNow();
    if (!g || profile < 0 || MissionName() == std::string("00menu") || MissionState() == MML_MISSION_LOADING) {
        *err = "no game being played to load it into (a mission, not the main menu or a load)"; return nullptr;
    }
    int state = -1;
    auto rs = [&] { state = game::at<int>(g, game::kGameState); };
    if (Guarded(rs) || state != 0) { *err = "the game is leaving or loading already (its state " + std::to_string(state) + ")"; return nullptr; }
    if (!SlotFree(profile, err)) return nullptr;
    std::string sav = GameSaveFile(profile, kRestoreSlot), mml = SlotFile(profile, kRestoreSlot);
    if (!PathCopy(base + ".sav", sav, true)) { *err = "cannot copy the point into " + sav; return nullptr; }
    PathDelete(mml);
    if (PathExists(base + ".mml")) PathCopy(base + ".mml", mml, false);
    g_restoring.active = true;
    g_restoring.name = name;
    g_restoring.mission = meta.value("mission", "");
    g_restoring.prevSlot = SlotNow();
    {
        std::lock_guard<std::recursive_mutex> lk(g_lock);
        g_restoring.prevSlotNow = g_slotNow;
    }
    g_restoring.started = GetTickCount();
    SetSlot(kRestoreSlot);
    auto ws = [&] { game::at<int>(g, game::kGameState) = game::kGameStateLoad; };
    if (Guarded(ws)) {
        g_restoring.active = false;
        SetSlot(g_restoring.prevSlot);
        PathDelete(sav);
        PathDelete(mml);
        *err = "cannot set the game's state";
        return nullptr;
    }
    return {{"loading", name}, {"mission", meta.value("mission", "")}, {"same_mission", meta.value("mission", "") == MissionName()},
            {"note", "the game loop loads it at its next frame (not while the pause menu is open): RESTORE_LOADED tells when"}};
}

json RestoreList() {
    json out = json::array();
    for (const FoundFile &fd : ListDir(RestoreDir() + "*.sav")) {
        std::string n = fd.name;
        n.resize(n.size() - 4);
        json m = ReadMeta(n);
        out.push_back(m.is_object() ? m : json({{"name", n}, {"note", "no .json beside it"}}));
    }
    return out;
}

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
    if (method.compare(0, 8, "restore.") == 0) {
        if (method != "restore.save" && method != "restore.load" && method != "restore.list" && method != "restore.delete") return false;
        *ok = false;
        json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
        if (!p.is_object()) p = json::object();
        std::string err;
        json r;
        if (method == "restore.save") r = RestoreSave(p, &err);
        else if (method == "restore.load") r = RestoreLoad(p, &err);
        else if (method == "restore.list") r = {{"dir", RestoreDir()}, {"points", RestoreList()}};
        else {
            std::string name = p.value("name", "");
            if (!RestoreName(name) || !PathExists(RestoreDir() + name + ".sav")) err = "no restore point '" + name + "'";
            else {
                for (const char *ext : {".sav", ".mml", ".json"}) PathDelete(RestoreDir() + name + ext);
                r = {{"deleted", name}};
            }
        }
        if (!err.empty()) { *reply = method + ": " + err; return true; }
        *reply = r.dump(-1, ' ', false, json::error_handler_t::replace);
        *ok = true;
        return true;
    }
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
            std::string value = p.contains("value") ? (p["value"].is_string() ? p["value"].get<std::string>() : p["value"].dump(-1, ' ', false, json::error_handler_t::replace)) : "";
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
