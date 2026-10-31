// The loader's own settings, switched in the game's Options > Mods (its page "Rendering (loader)", the first row) and
// kept across runs: what the loader does to the game's drawing.
//
// Their page is made by panel.cpp from the block below as a mod's is from its manifest's "settings"; their values are
// config.cpp's under the id "loader" (ConfigOwn: the defaults below, the changes in
// <game>\mml\data\loader\config.json; config.get / config.set {"mod": "loader"} as for a mod). The defaults are what
// the loader does with none: the buffers renamed (vbrename.cpp), the batches off (batch.cpp: opt-in until they have
// been driven through the missions), the whole city off (render.cpp: the game's own streaming and distances).
//
// A change is applied at the next frame (SettingsFrame) through the methods agents use, render.set and vb.set, and
// only what changed: a render.set or vb.set an agent sends meanwhile stays until the setting itself changes. Before
// LS3D's driver is made (the intro videos), what is to be applied waits.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <string>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

constexpr const char *kBlock = R"json({
  "title": "Rendering (loader)",
  "sections": [
    {"title": "Distance", "controls": [
      {"key": "render.distance_mode", "label": "Draw distance", "options": [
        {"value": 0, "label": "Game"}, {"value": 1, "label": "Custom"}, {"value": 2, "label": "Whole city"}]},
      {"key": "render.distance", "label": "Custom distance", "min": 100, "max": 10000, "step": 50, "unit": "m"},
      {"key": "render.occlusion", "label": "Occluders"}
    ]},
    {"title": "Fog", "controls": [
      {"key": "render.fog_mode", "label": "Native fog", "options": [
        {"value": 0, "label": "Game"}, {"value": 1, "label": "Off"}, {"value": 2, "label": "Linear"}]},
      {"key": "render.fog_start", "label": "Fog start", "min": 0, "max": 10000, "step": 50, "unit": "m"},
      {"key": "render.fog_span", "label": "Fog fade distance", "min": 50, "max": 10000, "step": 50, "unit": "m"}
    ]},
    {"title": "Advanced", "controls": [
      {"key": "render.batch", "label": "Batched drawing"},
      {"key": "render.batch_objects", "label": "Objects bypass LS3D"},
      {"key": "render.buffer_renaming", "label": "Buffer renaming"}
    ]}
  ]
})json";
// (render.batch_objects matters only while render.batch is on: batch.cpp's objects whole in batches not given to
// LS3D's m_AddFGroups)
constexpr const char *kDefaults = R"({"render": {"batch": false, "batch_objects": true, "buffer_renaming": true, "whole_city": false,
  "distance_mode": 0, "distance": 1000, "occlusion": true, "fog_mode": 0, "fog_start": 100, "fog_span": 900}})";

json g_applied;        // the values in effect (at the start: the loader's own, kDefaults)
bool g_pending;        // a change to apply
bool g_waitLogged;

json Wanted() {
    std::string j = ConfigValueJson(kLoaderSettings, "render");
    json v = j.empty() ? json() : json::parse(j, nullptr, false);
    return v.is_object() ? v : json::object();
}

std::string Text(const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

bool Flag(const json &o, const char *key, bool def) {
    auto it = o.find(key);
    return it != o.end() && it->is_boolean() ? it->get<bool>() : def;
}
double Number(const json &o, const char *key, double lo, double hi) {
    auto it = o.find(key);
    const double v = it != o.end() && it->is_number() ? it->get<double>() : g_applied[key].get<double>();
    return std::isfinite(v) ? std::clamp(v, lo, hi) : g_applied[key].get<double>();
}

} // namespace

std::string SettingsBlockJson() { return kBlock; }

void SettingsInit() {
    g_applied = json::parse(kDefaults)["render"];
    ConfigOwn(kLoaderSettings, kDefaults, [] { g_pending = true; });
    // Migrate old whole_city once. An explicit new mode wins on later starts, even if the old key remains.
    std::string reply;
    bool ok = true;
    ConfigMethod("config.get", R"({"mod":"loader"})", &reply, &ok);
    const json saved = ok ? json::parse(reply, nullptr, false) : json();
    if (saved.is_object() && saved.contains("changes") && saved["changes"].contains("render")) {
        const json &r = saved["changes"]["render"];
        if (r.value("fog_mode",0) == 3)
            Log(MML_LOG_WARN,"host","legacy atmosphere settings: run tools/migrate_atmosphere.py with the game closed");
        if (!r.contains("distance_mode") && Flag(r, "whole_city", false))
            ConfigSetLive(kLoaderSettings, "render.distance_mode", "2");
    }
    g_applied["whole_city"] = Flag(Wanted(), "whole_city", false);
}

void SettingsFrame() {
    if (!g_pending) return;
    const json want = Wanted();
    const bool rename = Flag(want, "buffer_renaming", g_applied["buffer_renaming"].get<bool>());
    if (rename != g_applied["buffer_renaming"].get<bool>()) {
        std::string reply;
        bool ok = true;
        const std::string p = Text(json{{"rename", rename}});
        VbRenameMethod("vb.set", p, &reply, &ok);
        if (!ok) Log(MML_LOG_WARN, "host", "settings: vb.set %s: %s", p.c_str(), reply.c_str());
        else HLOG("settings: vb.set %s", p.c_str());
        g_applied["buffer_renaming"] = rename;
    }
    json set = json::object();   // render.set's (vb.set's flag needs no driver: first)
    for (const char *key : {"batch_objects", "batch", "occlusion"}) {
        const bool v = Flag(want, key, g_applied[key].get<bool>());
        if (v != g_applied[key].get<bool>()) set[key] = v;
    }
    const int mode = static_cast<int>(Number(want, "distance_mode", 0, 2));
    const double distance = Number(want, "distance", 100, 10000);
    const bool legacy = Flag(want, "whole_city", false);
    const bool legacyChanged = legacy != g_applied["whole_city"].get<bool>();
    if (mode != g_applied["distance_mode"].get<int>() || distance != g_applied["distance"].get<double>() || legacyChanged) {
        set["distance_mode"] = legacyChanged ? (legacy ? 2 : 0) : mode;
        set["far"] = set["distance_mode"].get<int>() == 2 ? 5000.0 : distance;
    }
    const int fogMode = static_cast<int>(Number(want, "fog_mode", 0, 2));
    const double fogStart = Number(want, "fog_start", 0, 10000), fogSpan = Number(want, "fog_span", 50, 10000);
    if (fogMode != g_applied["fog_mode"].get<int>() || fogStart != g_applied["fog_start"].get<double>() ||
        fogSpan != g_applied["fog_span"].get<double>()) {
        set["fog_mode"] = fogMode;
        set["fog_start"] = fogStart;
        set["fog_end"] = fogStart + fogSpan;
    }
    if (!set.empty()) {
        std::string reply;
        bool ok = true;
        RenderMethod("render.set", Text(set), &reply, &ok);
        if (!ok && reply.find("no LS3D driver") != std::string::npos) {   // (not made yet: the next frame)
            if (!g_waitLogged) HLOG("settings: rendering's settings wait for LS3D's driver");
            g_waitLogged = true;
            return;
        }
        if (!ok) Log(MML_LOG_WARN, "host", "settings: render.set %s: %s", Text(set).c_str(), reply.c_str());
        else HLOG("settings: render.set %s", Text(set).c_str());
        for (auto &kv : set.items())
            if (g_applied.contains(kv.key())) g_applied[kv.key()] = kv.value();
        g_applied["distance_mode"] = mode;
        g_applied["distance"] = distance;
        g_applied["whole_city"] = legacy;
        g_applied["fog_mode"] = fogMode;
        g_applied["fog_start"] = fogStart;
        g_applied["fog_span"] = fogSpan;
    }
    g_pending = false;
}

} // namespace host
