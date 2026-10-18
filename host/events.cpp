// What happens, kept for agents (docs/PROTOCOL.md "events"): the bridge keeps the last 2000 events with a sequence
// number, so a client waits for one ("until the mission is loaded", "until he dies") instead of polling the game's
// state or sleeping. Every event the host sends mods is kept but those of every frame (FRAME, MISSION_TICK, RENDER_*,
// PRESENT, VIEW_WORLD), with the host's own: a mod loaded, unloaded or faulted, a script ended.
//
// Written on the game thread, read on the bridge's (events.since answers while the game thread loads or hangs).
// MCP's `wait` polls events.since until an event of the asked type (and fields) comes.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

struct Rec { uint64_t seq; double t; std::string type; json data; };
std::mutex g_mx;
std::deque<Rec> g_ring;
uint64_t g_seq;
constexpr size_t kKeep = 2000;
LARGE_INTEGER g_t0, g_freq;

double Now() {
    if (!g_freq.QuadPart) { QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0); }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

std::string Hex(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}

void Push(const char *type, json data) {
    if (SidecarWants(type)) SidecarEvent(type, data.dump(-1, ' ', false, json::error_handler_t::replace));
    std::lock_guard<std::mutex> lk(g_mx);
    g_ring.push_back({++g_seq, Now(), type, std::move(data)});
    if (g_ring.size() > kKeep) g_ring.pop_front();
}

// A filter's value matches a field: equal, or for strings, case-insensitively equal.
bool Matches(const json &field, const json &want) {
    if (field == want) return true;
    if (field.is_string() && want.is_string()) {
        std::string a = field.get<std::string>(), b = want.get<std::string>();
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    }
    if (field.is_number() && want.is_number()) return field.get<double>() == want.get<double>();
    return false;
}

} // namespace

void EventsRecord(const char *type, const std::string &dataJson) {
    json d = json::parse(dataJson.empty() ? "{}" : dataJson, nullptr, false);
    Push(type, d.is_discarded() ? json::object() : d);
}

void EventsFromDispatch(const MmlEvent &ev) {
    switch (ev.type) {
    case MML_EVENT_FRAME: case MML_EVENT_MISSION_TICK: case MML_EVENT_RENDER_BEGIN: case MML_EVENT_RENDER_WORLD:
    case MML_EVENT_RENDER_END: case MML_EVENT_PRESENT: case MML_EVENT_VIEW_WORLD:
        return;   // (every frame's)
    default: break;
    }
    json d = json::object();
    if (ev.actor) d["actor"] = Hex(ev.actor);
    if (ev.name) d["name"] = ev.name;
    if ((ev.type == MML_EVENT_HUMAN_HIT || ev.type == MML_EVENT_PEDESTRIAN_HIT) && ev.hit) {
        const MmlHumanHit &h = *ev.hit;
        d["hit_type"] = h.type;
        d["damage"] = h.damage;
        d["pos"] = {h.pos.x, h.pos.y, h.pos.z};
        d["dir"] = {h.dir.x, h.dir.y, h.dir.z};
        if (h.attacker) d["attacker"] = Hex(h.attacker);
        d["was_alive"] = h.was_alive != 0;
        d["alive"] = h.alive != 0;
        d["killed"] = h.was_alive && !h.alive;
    }
    if (ev.type == MML_EVENT_SHOT && ev.shot) {
        const MmlShot &s = *ev.shot;
        d["pos"] = {s.pos.x, s.pos.y, s.pos.z};
        d["dir"] = {s.dir.x, s.dir.y, s.dir.z};
        d["damage"] = s.damage;
        d["shots"] = s.shots;
    }
    if (ev.type == MML_EVENT_EXPLOSION && ev.explosion) {
        const MmlExplosion &x = *ev.explosion;
        d["pos"] = {x.pos.x, x.pos.y, x.pos.z};
        d["radius"] = x.radius;
        d["damage"] = x.damage;
    }
    if (ev.type == MML_EVENT_BULLET_HIT && ev.bullet) {
        const MmlBulletHit &b = *ev.bullet;
        d["pos"] = {b.pos.x, b.pos.y, b.pos.z};
        d["normal"] = {b.normal.x, b.normal.y, b.normal.z};
        d["dir"] = {b.dir.x, b.dir.y, b.dir.z};
        d["damage"] = b.damage;
        if (b.actor) d["struck"] = Hex(b.actor);
        d["kind"] = b.flags & 0xFF;
        if (!b.actor) d["surface"] = b.flags >> 24;
    }
    if (ev.type == MML_EVENT_CAR_CRASH && ev.crash) {
        const MmlCarCrash &c = *ev.crash;
        d["pos"] = {c.pos.x, c.pos.y, c.pos.z};
        d["impulse"] = {c.impulse.x, c.impulse.y, c.impulse.z};
        d["speed"] = c.speed;
        if (c.flags) d["surface"] = c.flags >> 24;
    }
    if (ev.type == MML_EVENT_HUMAN_FELL && ev.fall) { d["speed"] = ev.fall->speed; d["height"] = ev.fall->height; }
    if ((ev.type == MML_EVENT_ITEM_GAINED || ev.type == MML_EVENT_ITEM_LOST) && ev.item) {
        d["item"] = ev.item->item;
        d["name"] = WorldItemName(ev.item->item);
        d["count"] = ev.item->count;
        d["rounds"] = ev.item->rounds;
    }
    if (ev.type == MML_EVENT_MONEY_CHANGED) { d["money"] = ev.value; d["change"] = ev.change; }
    if (ev.other)
        d[ev.type == MML_EVENT_HUMAN_DIED ? "killer" : ev.type == MML_EVENT_PEDESTRIAN_HIT ? "model" : ev.type == MML_EVENT_CAR_CRASH ? "struck" : "vehicle"] =
            Hex(ev.other);
    if (ev.type == MML_EVENT_GAME_SAVING || ev.type == MML_EVENT_GAME_LOADED) { d["slot"] = ev.slot; d["profile"] = ev.profile; }
    Push(ProfEventSite(ev.type), std::move(d));
}

// events.since {seq, types, filter, limit} (the bridge's thread): the events after `seq`, of `types` (an array or a
// comma-separated list; none: all), whose data has each of `filter`'s fields equal; `latest` the last sequence number.
json EventsSince(const json &p, std::string *err) {
    uint64_t after = 0;
    std::vector<std::string> types;
    json filter = json::object();
    size_t limit = 200;
    if (p.is_object()) {
        after = p.value("seq", static_cast<uint64_t>(0));
        if (p.contains("types")) {
            const json &t = p["types"];
            if (t.is_array()) { for (auto &x : t) if (x.is_string()) types.push_back(x.get<std::string>()); }
            else if (t.is_string()) {
                std::string s = t.get<std::string>(), cur;
                for (char c : s + ",") { if (c == ',') { if (!cur.empty()) types.push_back(cur); cur.clear(); } else if (c != ' ') cur += c; }
            }
        }
        if (p.contains("filter") && p["filter"].is_object()) filter = p["filter"];
        limit = std::min<size_t>(p.value("limit", static_cast<size_t>(200)), 2000);
    }
    for (std::string &t : types) for (char &c : t) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    json out = json::array();
    std::lock_guard<std::mutex> lk(g_mx);
    uint64_t oldest = g_ring.empty() ? g_seq + 1 : g_ring.front().seq;
    for (const Rec &r : g_ring) {
        if (r.seq <= after) continue;
        if (!types.empty() && std::find(types.begin(), types.end(), r.type) == types.end()) continue;
        bool ok = true;
        for (auto it = filter.begin(); ok && it != filter.end(); ++it) ok = r.data.contains(it.key()) && Matches(r.data[it.key()], it.value());
        if (!ok) continue;
        out.push_back({{"seq", r.seq}, {"t", std::round(r.t * 1000) / 1000}, {"type", r.type}, {"data", r.data}});
        if (out.size() >= limit) break;
    }
    (void)err;
    return {{"latest", g_seq}, {"now", std::round(Now() * 1000) / 1000}, {"events", out}, {"missed", after > 0 && after < g_seq && after + 1 < oldest}};
}

} // namespace host
