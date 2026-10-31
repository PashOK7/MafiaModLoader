// A mod's own files and settings (mml.h mod_data_dir, config_*).
//
// The data folder: <game>\mml\data\<id>\, made when first asked for. A mod writes there (records, logs, saved
// state): its own folder under mml\mods is replaced by every deploy.
//
// Settings: two JSON files (comments allowed), read as one. <mod's folder>\config.json: the defaults, shipped with
// the mod. <data folder>\config.json: what was changed, written by config_set and the bridge's config.set, or by
// hand. The mod reads the defaults with the changes merged over them (objects merge key by key; anything else is
// replaced; null removes). Both folders are watched (ReadDirectoryChangesW, overlapped: the game thread looks at
// each watch's completion every frame, a read of memory: no thread, no call while nothing changes); a change to
// either config.json is read again once its writes have stopped for a moment (kQuietMs: an editor's save, a burst of
// writes, is one change) and the mod gets MML_EVENT_CONFIG_CHANGED (so does a set). A file that does not parse is
// reported in the log and the values in use stay; one that cannot be opened yet (a writer holds it) is tried again.
//
// Bridge methods (game thread): config.get {mod, key} (the merged value at the key, or the whole config, and which
// keys were changed from the defaults), config.set {mod, key, value} (any JSON value; null: back to the default).
// A key is a path through objects: "overlay.x".
//
// The loader's own settings (settings.cpp: Options > Mods' Rendering) are a config of the same kind under the id
// "loader" (no mod may take it): their defaults given by the loader's code (ConfigOwn), their changes in
// <game>\mml\data\loader\config.json, watched as a mod's; a change calls the loader back at the next frame.
#include "host.h"
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

struct Config {
    std::string dir;                   // the mod's folder (the defaults' place)
    json defaults = json::object(), changes = json::object(), merged = json::object();
    bool read = false, changed = false;   // changed: its mod is told at the next frame
    bool stale = false;                   // a file of it changed on disk: read again once quiet
    LONGLONG staleAt = 0;                 // when the last change was seen (NowMs)
    LONGLONG heldSince = 0;               // a file of it could not be opened since then (0: it could)
    bool unwritten = false;               // changes made live, not in the file yet
    DWORD editedAt = 0;
    std::map<std::string, std::string> handed;   // strings handed to the mod (config_string, config_json), by key
    bool own = false;                     // the loader's own (ConfigOwn): defaults from its code, no folder of a mod
    std::function<void()> told;           // and what is told of a change
};
std::map<std::string, Config> g_configs;   // by mod id
std::map<std::string, std::string> g_dataDirs;

// A file's writes have stopped once no change came for this long: then it is read (a frame or two after the change).
constexpr LONGLONG kQuietMs = 10;

LONGLONG NowMs() {
    static LARGE_INTEGER freq;   // (zero-initialised: no dynamic initializer, no TLS)
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart * 1000 / freq.QuadPart;
}

std::string LowerAscii(std::string s) {
    for (char &ch : s) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return s;
}

std::string DataDirOf(const std::string &id);

// A folder that holds settings (a mod's folder, a data folder), watched for changes to its files. The read stays
// pending in the kernel; ConfigFrame sees it complete (the OVERLAPPED leaves STATUS_PENDING) and issues the next one.
struct DirWatch {
    std::string dir;                  // UTF-8, trailing backslash
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED ov{};
    alignas(DWORD) BYTE buffer[4096];
    bool pending = false;
    LONGLONG retryAt = 0;             // a folder that could not be watched (not there): tried again from then
};
std::map<std::string, DirWatch *> g_watches;   // by the folder's path in lower case; never freed (the kernel writes the buffer)

bool Issue(DirWatch *w) {
    w->ov = OVERLAPPED{};
    w->pending = ReadDirectoryChangesW(w->handle, w->buffer, sizeof w->buffer, FALSE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                       FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION, nullptr, &w->ov, nullptr) != FALSE;
    return w->pending;
}

void Close(DirWatch *w, LONGLONG now) {
    CloseHandle(w->handle);
    w->handle = INVALID_HANDLE_VALUE;
    w->pending = false;
    w->retryAt = now + 500;
}

bool Open(DirWatch *w, LONGLONG now) {
    w->handle = PathCreateFile(w->dir, FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED);
    if (w->handle == INVALID_HANDLE_VALUE) { w->retryAt = now + 500; return false; }
    if (!Issue(w)) { Close(w, now); return false; }
    return true;
}

// The folder of a settings file watched (once; again when it was gone).
void WatchDir(const std::string &dir) {
    DirWatch *&w = g_watches[LowerAscii(dir)];
    if (w) return;
    w = new DirWatch;
    w->dir = dir;
    Open(w, NowMs());
}

// Every config whose file is `path` (lower case), or under it (a folder renamed or removed), made stale; every
// config with a file in the folder when `path` is empty (more changes than the buffer held, or the watch broke).
void MarkStale(const std::string &dirLower, const std::string &path, LONGLONG now) {
    for (auto &kv : g_configs) {
        Config &c = kv.second;
        if (!c.read) continue;
        for (const std::string &file : {c.own ? std::string() : LowerAscii(c.dir + "config.json"), LowerAscii(DataDirOf(kv.first) + "config.json")}) {
            if (file.empty()) continue;
            if (file.compare(0, dirLower.size(), dirLower) != 0) continue;
            if (!path.empty() && file != path && file.compare(0, path.size() + 1, path + "\\") != 0) continue;
            c.stale = true;
            c.staleAt = now;
        }
    }
}

// What the watches saw since the last frame.
void LookAtWatches(LONGLONG now) {
    for (auto &kv : g_watches) {
        DirWatch *w = kv.second;
        if (w->handle == INVALID_HANDLE_VALUE) {
            if (now < w->retryAt || !Open(w, now)) continue;
            MarkStale(kv.first, "", now);   // (watched again: what changed meanwhile is not known)
            continue;
        }
        if (!w->pending || !HasOverlappedIoCompleted(&w->ov)) continue;
        DWORD bytes = 0;
        if (!GetOverlappedResult(w->handle, &w->ov, &bytes, FALSE)) {   // the folder removed (its delete pending), or worse
            Close(w, now);
            MarkStale(kv.first, "", now);
            continue;
        }
        if (bytes == 0) MarkStale(kv.first, "", now);   // more than the buffer held: anything may have changed
        for (DWORD at = 0; bytes != 0;) {
            const auto *n = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(w->buffer + at);
            MarkStale(kv.first, LowerAscii(w->dir + Utf8Of(n->FileName, static_cast<int>(n->FileNameLength / sizeof(WCHAR)))), now);
            if (!n->NextEntryOffset) break;
            at += n->NextEntryOffset;
        }
        if (!Issue(w)) Close(w, now);
    }
}

std::string DataDirOf(const std::string &id) {
    auto it = g_dataDirs.find(id);
    if (it != g_dataDirs.end()) return it->second;
    std::string root = MmlDir() + "data\\", dir = root + id + "\\";
    MakeDir(root);
    MakeDir(dir);
    return g_dataDirs[id] = dir;
}

// An object from a JSON file; false (and why) when it is there and cannot be used (*busy: it cannot be opened
// now: a writer holds it with no sharing, or its delete is pending). No file: an empty object.
bool ReadObject(const std::string &path, json *out, std::string *err, bool *busy) {
    HANDLE h = PathCreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) { *out = json::object(); return true; }
        *busy = e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED;
        *err = "cannot be opened (error " + std::to_string(e) + ")";
        return false;
    }
    std::string text;
    char chunk[4096];
    DWORD n = 0;
    while (ReadFile(h, chunk, sizeof chunk, &n, nullptr) && n) text.append(chunk, n);
    CloseHandle(h);
    try {
        json j = json::parse(text, nullptr, true, true);
        if (!j.is_object()) { *err = "not a JSON object"; return false; }
        *out = j;
        return true;
    } catch (const std::exception &e) { *err = e.what(); return false; }
}

// Both files read (again); true when the merged config is not what it was. *busy: a file could not be opened (a
// writer holds it): its values in use stay, to be read again.
bool Read(const std::string &id, Config *c, bool *busy = nullptr) {
    std::string defaultsPath = c->dir + "config.json", changesPath = DataDirOf(id) + "config.json", err;
    json d, u;
    bool held = false;
    if (c->own) {
        d = c->defaults;
    } else if (!ReadObject(defaultsPath, &d, &err, &held)) {
        if (!held) Log(MML_LOG_WARN, id.c_str(), "%s: %s (the values in use stay)", defaultsPath.c_str(), err.c_str());
        d = c->defaults;
    }
    bool held2 = false;
    if (!ReadObject(changesPath, &u, &err, &held2)) {
        if (!held2) Log(MML_LOG_WARN, id.c_str(), "%s: %s (the values in use stay)", changesPath.c_str(), err.c_str());
        u = c->changes;
    }
    if (busy) *busy = held || held2;
    json merged = d;
    merged.merge_patch(u);
    bool differs = !c->read || merged != c->merged;
    c->defaults = d;
    c->changes = u;
    c->merged = merged;
    c->read = true;
    if (differs) c->handed.clear();
    return differs;
}

Config *Of(const std::string &id) {
    auto it = g_configs.find(id);
    if (it != g_configs.end() && it->second.read) return &it->second;
    const bool own = it != g_configs.end() && it->second.own;
    std::string dir;
    if (!own && !ModDirById(id, &dir)) return nullptr;
    Config &c = g_configs[id];
    if (!own) c.dir = dir;
    bool busy = false;
    Read(id, &c, &busy);
    c.stale = busy;   // (a file held by its writer: read again at a frame)
    c.staleAt = NowMs();
    c.heldSince = 0;
    if (!own) WatchDir(c.dir);
    WatchDir(DataDirOf(id));
    return &c;
}
Config *Of(MmlMod *mod) { return ModAlive(mod) ? Of(ModIdOf(mod)) : nullptr; }

// The value at a path ("a.b.c") of an object; null when there is none.
const json *At(const json &root, const char *key) {
    const json *j = &root;
    if (!key || !*key) return j;
    std::stringstream parts(key);
    std::string part;
    while (std::getline(parts, part, '.')) {
        if (j->is_array()) {   // "points.2": an array's element
            char *end;
            unsigned long i = strtoul(part.c_str(), &end, 10);
            if (part.empty() || *end || i >= j->size()) return nullptr;
            j = &(*j)[i];
            continue;
        }
        if (!j->is_object()) return nullptr;
        auto it = j->find(part);
        if (it == j->end()) return nullptr;
        j = &*it;
    }
    return j;
}

// A patch that sets `value` at the path: {"a": {"b": value}}.
json PatchFor(const char *key, const json &value) {
    std::vector<std::string> parts;
    std::stringstream ss(key);
    std::string part;
    while (std::getline(ss, part, '.')) parts.push_back(part);
    json patch = value;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) patch = json{{*it, patch}};
    return patch;
}

// Empty objects left by removed keys are dropped (the file stays what was changed, no more).
void Prune(json *j) {
    for (auto it = j->begin(); it != j->end();) {
        if (it->is_object()) {
            Prune(&*it);
            if (it->empty()) { it = j->erase(it); continue; }
        }
        ++it;
    }
}

// The changes with `value` at the key (null: the key back to its default).
json Patched(const json &from, const char *key, const json &value) {
    json changes = from;
    // back to the default: the key leaves the changes (a null in a merge patch would remove the default too)
    if (value.is_null()) {
        std::string k = key;
        size_t dot = k.rfind('.');
        json *parent = dot == std::string::npos ? &changes : const_cast<json *>(At(changes, k.substr(0, dot).c_str()));
        if (parent && parent->is_object()) parent->erase(dot == std::string::npos ? k : k.substr(dot + 1));
        Prune(&changes);
    } else {
        changes.merge_patch(PatchFor(key, value));
    }
    return changes;
}

bool WriteChanges(const std::string &id, const json &changes, std::string *err) {
    std::string path = DataDirOf(id) + "config.json", tmp = path + ".tmp";
    MakeDir(MmlDir() + "data\\");   // (removed by hand since: made again)
    MakeDir(DataDirOf(id));
    {
        std::ofstream f(Wide(tmp), std::ios::trunc);
        if (!f) { *err = "cannot write " + tmp; return false; }
        f << changes.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    }
    if (!PathMove(tmp, path, MOVEFILE_REPLACE_EXISTING)) { *err = "cannot write " + path; return false; }
    return true;
}

bool Set(const std::string &id, Config *c, const char *key, const json &value, std::string *err) {
    if (!key || !*key) { *err = "key: a setting's name (a path: \"overlay.x\")"; return false; }
    if (!WriteChanges(id, Patched(c->changes, key, value), err)) return false;
    c->unwritten = false;
    if (Read(id, c)) c->changed = true;
    return true;
}

// A change made live (panel.cpp: a slider dragged): the mod sees it at the next frame, the file is written once the
// changes have stopped for a moment (ConfigFrame) or at once with ConfigFlush.
void SetLive(Config *c, const char *key, const json &value) {
    json changes = Patched(c->changes, key, value);
    json merged = c->defaults;
    merged.merge_patch(changes);
    if (merged == c->merged && changes == c->changes) return;
    c->changes = changes;
    c->merged = merged;
    c->handed.clear();
    c->changed = true;
    c->unwritten = true;
    c->editedAt = GetTickCount();
}

void Flush(const std::string &id, Config *c) {
    if (!c->unwritten) return;
    std::string err;
    if (!WriteChanges(id, c->changes, &err)) { Log(MML_LOG_WARN, id.c_str(), "config: %s", err.c_str()); return; }
    c->unwritten = false;   // (our own write comes back as a change on disk: read, the same, not told)
}

} // namespace

const char *ModDataDir(MmlMod *mod) {
    if (!mod) return "";
    DataDirOf(ModIdOf(mod));
    return g_dataDirs[ModIdOf(mod)].c_str();
}

double ConfigNumber(MmlMod *mod, const char *key, double def) {
    Config *c = Of(mod);
    const json *j = c ? At(c->merged, key) : nullptr;
    if (!j) return def;
    if (j->is_boolean()) return j->get<bool>() ? 1.0 : 0.0;
    return j->is_number() ? j->get<double>() : def;
}

const char *ConfigString(MmlMod *mod, const char *key, const char *def) {
    Config *c = Of(mod);
    const json *j = c ? At(c->merged, key) : nullptr;
    if (!j || !j->is_string()) return def;
    return (c->handed[std::string("s:") + key] = j->get<std::string>()).c_str();
}

const char *ConfigJson(MmlMod *mod, const char *key) {
    Config *c = Of(mod);
    const json *j = c ? At(c->merged, key) : nullptr;
    if (!j) return nullptr;
    return (c->handed[std::string("j:") + (key ? key : "")] = j->dump(-1, ' ', false, json::error_handler_t::replace)).c_str();
}

bool ConfigSet(MmlMod *mod, const char *key, const char *valueJson) {
    Config *c = Of(mod);
    if (!c || !valueJson) return false;
    json value;
    try { value = json::parse(valueJson); } catch (...) { return false; }
    std::string err;
    if (Set(ModIdOf(mod), c, key, value, &err)) return true;
    Log(MML_LOG_WARN, ModIdOf(mod), "config_set %s: %s", key ? key : "", err.c_str());
    return false;
}

bool ConfigSetLive(const std::string &id, const char *key, const std::string &valueJson) {
    Config *c = Of(id);
    if (!c || !key || !*key) return false;
    json value = json::parse(valueJson, nullptr, false);
    if (value.is_discarded()) return false;
    SetLive(c, key, value);
    return true;
}

void ConfigFlush(const std::string &id) {
    if (Config *c = Of(id)) Flush(id, c);
}

std::string ConfigDefaultJson(const std::string &id, const char *key) {
    Config *c = Of(id);
    const json *j = c ? At(c->defaults, key) : nullptr;
    return j ? j->dump() : std::string();
}

std::string ConfigValueJson(const std::string &id, const char *key) {
    Config *c = Of(id);
    const json *j = c ? At(c->merged, key) : nullptr;
    return j ? j->dump() : std::string();
}

void ConfigOwn(const std::string &id, const std::string &defaultsJson, std::function<void()> told) {
    Config &c = g_configs[id];
    c.own = true;
    c.defaults = json::parse(defaultsJson, nullptr, false, true);
    if (!c.defaults.is_object()) c.defaults = json::object();
    c.told = std::move(told);
    c.read = false;
    Of(id);
    c.changed = true;   // (told at the first frame: what the files say is applied)
}

namespace {
// A config's frame: live changes written once they stop, the files read again once quiet; true: its owner is told.
bool Tend(const std::string &id, Config &c, DWORD now, LONGLONG nowMs) {
    if (c.unwritten && now - c.editedAt >= 400) Flush(id, &c);   // (live changes: written once they stop)
    if (c.stale && !c.unwritten && nowMs - c.staleAt >= kQuietMs) {
        bool busy = false;
        c.stale = false;
        if (Read(id, &c, &busy)) {
            c.changed = true;
            Log(MML_LOG_INFO, "host", "config: %s's settings changed on disk", id.c_str());
        }
        if (!busy) {
            c.heldSince = 0;
        } else if (!c.heldSince || nowMs - c.heldSince < 5000) {   // a writer holds a file: read once it lets go
            if (!c.heldSince) c.heldSince = nowMs;
            c.stale = true;
            c.staleAt = nowMs;
        } else {
            c.heldSince = 0;
            Log(MML_LOG_WARN, id.c_str(), "config: a settings file could not be opened for 5 s (the values in use stay)");
        }
    }
    if (!c.changed) return false;
    c.changed = false;
    return true;
}
} // namespace

void ConfigFrame() {
    DWORD now = GetTickCount();
    LONGLONG nowMs = NowMs();
    LookAtWatches(nowMs);
    for (auto &kv : g_configs)   // (the loader's own: told by a call)
        if (kv.second.own && kv.second.read && Tend(kv.first, kv.second, now, nowMs) && kv.second.told) kv.second.told();
    for (MmlMod *mod : ModsLoaded()) {
        std::string id = ModIdOf(mod);
        auto it = g_configs.find(id);
        if (it == g_configs.end() || !it->second.read || it->second.own) continue;   // a mod that never asked has no config to watch
        if (!Tend(id, it->second, now, nowMs)) continue;
        MmlEvent ev{};
        ev.size = sizeof ev;
        ev.type = MML_EVENT_CONFIG_CHANGED;
        ev.name = id.c_str();
        DispatchTo(mod, ev);
    }
}

// The mod's next load reads its files anew (its folder may have been deployed over) and gets no stale event.
void ConfigForget(MmlMod *mod) {
    auto it = g_configs.find(ModIdOf(mod));
    if (it == g_configs.end()) return;
    Flush(it->first, &it->second);   // (what was changed live is kept)
    it->second.read = false;
    it->second.changed = false;
    it->second.stale = false;
    it->second.handed.clear();
}

bool ConfigMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "config.get" && method != "config.set") return false;
    *ok = false;
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    if (!p.is_object() || !p.contains("mod") || !p["mod"].is_string()) { *reply = "mod: a mod's id; key: a setting (a path: \"overlay.x\"); value (config.set): any JSON, null for the default"; return true; }
    std::string id = p["mod"].get<std::string>(), key = p.value("key", "");
    Config *c = Of(id);
    if (!c) { *reply = "no mod '" + id + "' (mods.list)"; return true; }
    if (method == "config.set") {
        if (!p.contains("value")) { *reply = "value: any JSON value (null: back to the default)"; return true; }
        std::string err;
        if (!Set(id, c, key.c_str(), p["value"], &err)) { *reply = err; return true; }
    }
    const json *j = At(c->merged, key.c_str());
    json out = {{"mod", id}, {"value", j ? *j : json()}, {"changes", c->changes},
                {"files", {{"defaults", c->own ? json() : json(c->dir + "config.json")}, {"changes", DataDirOf(id) + "config.json"}}}};
    if (!key.empty()) {
        out["key"] = key;
        out["known"] = j != nullptr;
        const json *d = At(c->defaults, key.c_str());
        out["default"] = d ? *d : json();
    }
    *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
