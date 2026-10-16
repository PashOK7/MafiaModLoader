// A mod's own files and settings (mml.h mod_data_dir, config_*).
//
// The data folder: <game>\mml\data\<id>\, made when first asked for. A mod writes there (records, logs, saved
// state): its own folder under mml\mods is replaced by every deploy.
//
// Settings: two JSON files (comments allowed), read as one. <mod's folder>\config.json: the defaults, shipped with
// the mod. <data folder>\config.json: what was changed, written by config_set and the bridge's config.set, or by
// hand. The mod reads the defaults with the changes merged over them (objects merge key by key; anything else is
// replaced; null removes). Both files' times are looked at twice a second: a file that changed is read again and
// its mod gets MML_EVENT_CONFIG_CHANGED (so does a set). A file that does not parse is reported in the log and the
// values in use stay.
//
// Bridge methods (game thread): config.get {mod, key} (the merged value at the key, or the whole config, and which
// keys were changed from the defaults), config.set {mod, key, value} (any JSON value; null: back to the default).
// A key is a path through objects: "overlay.x".
#include "host.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

struct Config {
    std::string dir;                   // the mod's folder (the defaults' place)
    json defaults = json::object(), changes = json::object(), merged = json::object();
    uint64_t timeDefaults = 0, timeChanges = 0;
    bool read = false, changed = false;   // changed: its mod is told at the next frame
    bool unwritten = false;               // changes made live, not in the file yet
    DWORD editedAt = 0;
    std::map<std::string, std::string> handed;   // strings handed to the mod (config_string, config_json), by key
};
std::map<std::string, Config> g_configs;   // by mod id
std::map<std::string, std::string> g_dataDirs;
DWORD g_lastLook;

std::string DataDirOf(const std::string &id) {
    auto it = g_dataDirs.find(id);
    if (it != g_dataDirs.end()) return it->second;
    std::string root = MmlDir() + "data\\", dir = root + id + "\\";
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    return g_dataDirs[id] = dir;
}

// A file's last write time (0: no such file).
uint64_t FileTime(const std::string &path) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
}

// An object from a JSON file; false (and why) when it is there and cannot be used. No file: an empty object.
bool ReadObject(const std::string &path, json *out, std::string *err) {
    std::ifstream f(path);
    if (!f) { *out = json::object(); return true; }
    try {
        json j = json::parse(f, nullptr, true, true);
        if (!j.is_object()) { *err = "not a JSON object"; return false; }
        *out = j;
        return true;
    } catch (const std::exception &e) { *err = e.what(); return false; }
}

// Both files read (again); true when the merged config is not what it was.
bool Read(const std::string &id, Config *c) {
    std::string defaultsPath = c->dir + "config.json", changesPath = DataDirOf(id) + "config.json", err;
    c->timeDefaults = FileTime(defaultsPath);
    c->timeChanges = FileTime(changesPath);
    json d, u;
    if (!ReadObject(defaultsPath, &d, &err)) { Log(MML_LOG_WARN, id.c_str(), "%s: %s (the values in use stay)", defaultsPath.c_str(), err.c_str()); d = c->defaults; }
    if (!ReadObject(changesPath, &u, &err)) { Log(MML_LOG_WARN, id.c_str(), "%s: %s (the values in use stay)", changesPath.c_str(), err.c_str()); u = c->changes; }
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
    std::string dir;
    if (!ModDirById(id, &dir)) return nullptr;
    Config &c = g_configs[id];
    c.dir = dir;
    Read(id, &c);
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
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) { *err = "cannot write " + tmp; return false; }
        f << changes.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { *err = "cannot write " + path; return false; }
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
    c->unwritten = false;
    c->timeChanges = FileTime(DataDirOf(id) + "config.json");   // (our own write: not a change on disk)
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

bool ConfigSetLive(MmlMod *mod, const char *key, const std::string &valueJson) {
    Config *c = Of(mod);
    if (!c || !key || !*key) return false;
    json value = json::parse(valueJson, nullptr, false);
    if (value.is_discarded()) return false;
    SetLive(c, key, value);
    return true;
}

void ConfigFlush(MmlMod *mod) {
    if (Config *c = Of(mod)) Flush(ModIdOf(mod), c);
}

std::string ConfigDefaultJson(MmlMod *mod, const char *key) {
    Config *c = Of(mod);
    const json *j = c ? At(c->defaults, key) : nullptr;
    return j ? j->dump() : std::string();
}

std::string ConfigValueJson(MmlMod *mod, const char *key) {
    Config *c = Of(mod);
    const json *j = c ? At(c->merged, key) : nullptr;
    return j ? j->dump() : std::string();
}

void ConfigFrame() {
    DWORD now = GetTickCount();
    bool look = now - g_lastLook >= 500;
    if (look) g_lastLook = now;
    for (MmlMod *mod : ModsLoaded()) {
        std::string id = ModIdOf(mod);
        auto it = g_configs.find(id);
        if (it == g_configs.end() || !it->second.read) continue;   // a mod that never asked has no config to watch
        Config &c = it->second;
        if (c.unwritten && now - c.editedAt >= 400) Flush(id, &c);   // (live changes: written once they stop)
        if (look && !c.unwritten && (FileTime(c.dir + "config.json") != c.timeDefaults || FileTime(DataDirOf(id) + "config.json") != c.timeChanges)) {
            if (Read(id, &c)) {
                c.changed = true;
                Log(MML_LOG_INFO, "host", "config: %s's settings changed on disk", id.c_str());
            }
        }
        if (!c.changed) continue;
        c.changed = false;
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
                {"files", {{"defaults", c->dir + "config.json"}, {"changes", DataDirOf(id) + "config.json"}}}};
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
