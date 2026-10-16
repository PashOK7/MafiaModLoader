// A mod's own items (mml.h item_define, item_find; docs/MODDING.md "A mod's own items").
//
// The game's items are tables\predmety.def's records as LoadMaterialsFromBin read them at the start (g_pItems,
// 188 bytes a record, its count after it; game_build.h items): an item's id is its index, and everything the game
// does with an item (the inventory, the hand, dropping it, the pickup menu, the scripts' human_addweapon) reads its
// record there, bounded by the count. A mod's item is one more record: the table is moved once into memory of the
// game's own heap (gameNew, as the loader allocated it) with room for kCap records, and the count raised to cover
// every id given out; the game's first array is left as it was (a pointer to one of its records may be kept). The
// game shows an item's name by text ids 3300 + id (its name: the pickup menu, the line when it comes into the
// hand), 3400 + id ("... collected") and 3500 + id (its name again): those are answered from the loader (menus.cpp
// GameTextSet), and an id is given only where all three are free in the game's own database (3438, 3439 are other
// lines). Ids are kept by "mod/name" in mml\items.json, so a save holding an item finds it again; one whose mod is
// not loaded this run is an inert placeholder (a key's record, its name the key).
//
// Bridge methods (game thread): items.state, items.define.
#include "host.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kRec = 188, kCap = 128, kName = 0x00, kFlags = 0x20, kModel = 0x24, kLikeDefault = 16;
constexpr uint32_t kTextName = 3300, kTextCollected = 3400, kTextName2 = 3500;

using NewFn = void *(__cdecl *)(size_t);

struct Def {
    std::string key, label, model, collected;
    int like = kLikeDefault, flags = -1;
    bool defined = false;   // this run (else a placeholder)
};
std::map<std::string, int> g_ids;    // "mod/name" -> id (mml\items.json)
std::map<int, Def> g_defs;           // id -> what it is
bool g_loaded;                       // items.json read
int g_gameCount = -1;                // the game's own records
std::vector<uint8_t> g_orig;         // their bytes (what a mod's item is made like)
uint8_t *g_table;                    // ours, kCap records
std::string g_lastError;

const game::ExeBuild &E() { return game::Exe(); }
bool Known() { return game::g_exe && E().items && E().gameNew; }
uint8_t *GameTable() { return *reinterpret_cast<uint8_t **>(E().items); }
int &GameCount() { return *reinterpret_cast<int *>(E().items + 4); }

std::string File() { return MmlDir() + "items.json"; }
void Load() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(File(), std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_object() || !j.contains("items") || !j["items"].is_object()) { HWARN("items: %s is not readable: its ids start anew", File().c_str()); return; }
    for (auto it = j["items"].begin(); it != j["items"].end(); ++it)
        if (it.value().is_number_integer()) {
            int id = it.value().get<int>();
            if (id > 0 && id < kCap) { g_ids[it.key()] = id; g_defs[id].key = it.key(); }
        }
}
void Save() {
    json items = json::object();
    for (const auto &kv : g_ids) items[kv.first] = kv.second;
    std::string tmp = File() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << json({{"note", "the ids of the mods' items (MafiaModLoader): kept so the saves holding them find them again"},
                   {"items", items}}).dump(2);
    }
    DeleteFileA(File().c_str());
    if (!MoveFileA(tmp.c_str(), File().c_str())) HWARN("items: cannot write %s", File().c_str());
}

// The game's own text of an id is free for us: none, or blank.
bool TextFree(uint32_t id) {
    const char *t = GameTextOwn(id);
    if (!t) return true;
    for (; *t; t++) if (*t != ' ') return false;
    return true;
}
bool IdFree(int id) { return TextFree(kTextName + id) && TextFree(kTextCollected + id) && TextFree(kTextName2 + id); }

void PutStr(uint8_t *at, const std::string &s, size_t n) {
    std::memset(at, 0, n);
    std::memcpy(at, s.data(), std::min(s.size(), n - 1));
}
// Record `id` written from its definition (or as a placeholder), and its texts.
void Write(int id) {
    static const Def gap{"(unused)"};
    auto it = g_defs.find(id);
    const Def &d = it != g_defs.end() ? it->second : gap;
    int like = d.defined ? d.like : kLikeDefault;
    uint8_t *r = g_table + static_cast<size_t>(id) * kRec;
    std::memcpy(r, g_orig.data() + static_cast<size_t>(like) * kRec, kRec);
    std::string label = d.defined ? d.label : d.key;
    PutStr(r + kName, GameAnsi(label), 32);
    if (d.defined && !d.model.empty()) PutStr(r + kModel, d.model, 32);
    if (d.defined && d.flags >= 0) std::memcpy(r + kFlags, &d.flags, 4);
    GameTextSet(kTextName + id, label);
    GameTextSet(kTextName2 + id, label);
    GameTextSet(kTextCollected + id, d.defined && !d.collected.empty() ? d.collected : label + " collected");
}
void *GameNew(size_t n) {
    __try { return reinterpret_cast<NewFn>(E().gameNew)(n); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
int Needed() {
    int n = g_gameCount;
    for (const auto &kv : g_defs) n = std::max(n, kv.first + 1);
    return n;
}

// The table ours, covering every id (the game's read again at a new start: ours again). False: not yet.
bool Install(std::string *err) {
    if (!Known()) { *err = "the items are not known for this build"; return false; }
    uint8_t *t = GameTable();
    if (!t) { *err = "the game's items are not loaded yet"; return false; }
    if (t != g_table) {
        int n = GameCount();
        if (n <= 0 || n >= kCap) { *err = "the game's items: a count of " + std::to_string(n); return false; }
        if (g_defs.empty()) return true;   // nothing to add: the game's table as it is
        g_gameCount = n;
        g_orig.assign(t, t + static_cast<size_t>(n) * kRec);
        if (!g_table) {
            g_table = static_cast<uint8_t *>(GameNew(static_cast<size_t>(kCap) * kRec));
            if (!g_table) { *err = "the game's operator new gave nothing"; return false; }
        }
        std::memset(g_table, 0, static_cast<size_t>(kCap) * kRec);
        std::memcpy(g_table, g_orig.data(), g_orig.size());
        for (int id = n; id < Needed(); id++) Write(id);
        *reinterpret_cast<uint8_t **>(E().items) = g_table;
        HLOG("items: the game's %d items and %d of the mods' (%s)", n, Needed() - n, File().c_str());
    }
    GameCount() = Needed();
    return true;
}

bool ValidName(const std::string &s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s) if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.') return false;
    return true;
}

} // namespace

// (game_hooks.cpp) every frame: the table ours once the game has it, should ids be kept from earlier runs.
void ItemsFrame() {
    if (!Known()) return;
    Load();
    if (g_defs.empty()) return;
    uint8_t *t = GameTable();
    if (!t || (t == g_table && GameCount() == Needed())) return;
    std::string err;
    if (!Install(&err) && err != "the game's items are not loaded yet") {
        static std::string said;
        if (said != err) HWARN("items: %s", err.c_str());
        said = err;
    }
}

int ItemDefine(const std::string &mod, const MmlItemDef &def, std::string *err) {
    if (!Known()) { *err = "the items are not known for this build"; return 0; }
    std::string name = def.name ? def.name : "";
    if (!ValidName(name)) { *err = "name: 1 to 64 letters, digits, '_', '-', '.'"; return 0; }
    if (!GameTable()) { *err = "the game's items are not loaded yet"; return 0; }
    Load();
    if (GameTable() != g_table) g_gameCount = GameCount();
    int like = def.like ? def.like : kLikeDefault;
    if (like <= 0 || like >= g_gameCount) { *err = "like: one of the game's items, 1.." + std::to_string(g_gameCount - 1); return 0; }
    std::string model = def.model ? def.model : "";
    if (model.size() > 31) { *err = "model: at most 31 characters"; return 0; }
    if (!GameTextsReady()) { *err = "the game's text database is not hooked (no names for the items)"; return 0; }
    std::string key = mod + "/" + name;
    int id;
    auto it = g_ids.find(key);
    if (it != g_ids.end()) id = it->second;
    else {
        id = 0;
        for (int c = g_gameCount; c < kCap && !id; c++) if (!g_defs.count(c) && IdFree(c)) id = c;
        if (!id) { *err = "no free id left (the game's texts 3300 + id, 3400 + id, 3500 + id must be free)"; return 0; }
        g_ids[key] = id;
        Save();
    }
    Def &d = g_defs[id];
    d.key = key;
    d.label = def.label && *def.label ? def.label : name;
    d.model = model;
    d.collected = def.collected ? def.collected : "";
    d.like = like;
    d.flags = def.flags;
    d.defined = true;
    if (!Install(err)) return 0;
    Write(id);   // (the table was ours already: this record changed in place)
    return id;
}

int ItemFind(const std::string &mod, const std::string &name) {
    Load();
    std::string key = name.find('/') == std::string::npos ? mod + "/" + name : name;
    auto it = g_ids.find(key);
    return it != g_ids.end() && g_defs.count(it->second) && g_defs[it->second].defined ? it->second : 0;
}

std::string ItemOwner(int id) {
    auto it = g_defs.find(id);
    return it != g_defs.end() && g_table && GameTable() == g_table ? it->second.key : std::string();
}

bool ItemsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "items.state" && method != "items.define") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "items.state") {
            Load();
            json list = json::array();
            for (const auto &kv : g_defs) {
                const Def &d = kv.second;
                json j = {{"id", kv.first}, {"key", d.key}, {"defined", d.defined}};
                if (d.defined) { j["label"] = d.label; j["like"] = d.like; j["model"] = d.model; j["flags"] = d.flags; }
                list.push_back(j);
            }
            bool ours = Known() && g_table && GameTable() == g_table;
            *reply = dump({{"known", Known()}, {"game_items", g_gameCount}, {"count", Known() && GameTable() ? GameCount() : 0},
                           {"installed", ours}, {"room", kCap}, {"file", File()}, {"items", list}});
        } else {
            std::string name = p.value("name", ""), label = p.value("label", ""), model = p.value("model", ""), coll = p.value("collected", "");
            MmlItemDef d{sizeof d, name.c_str(), label.empty() ? nullptr : label.c_str(), model.empty() ? nullptr : model.c_str(),
                         p.value("like", 0), p.value("flags", -1), coll.empty() ? nullptr : coll.c_str()};
            std::string err;
            int id = ItemDefine("bridge", d, &err);
            if (!id) { *reply = "items.define: " + err; return true; }
            *reply = dump({{"id", id}, {"key", "bridge/" + name}, {"name", WorldItemName(id)}});
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
