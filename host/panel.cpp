// The mods' settings in the game's own menus (docs/MODDING.md "Settings in the game's Options"): Options > Mods lists
// every loaded mod whose manifest has a "settings" block; its page is made of the game's own controls (sliders,
// check boxes, selectors, as the freeride's densities and the Options' own) from that block, with no code of the
// mod's. A change is made live (config.cpp ConfigSetLive: the mod gets MML_EVENT_CONFIG_CHANGED at the next frame and
// the file is written once the changes stop), so what a slider does is seen while it is dragged.
//
// mod.json:
//   "settings": {
//     "title": "God rays",                     (the row in Mods and the page's title; else the mod's name)
//     "controls": [
//       {"key": "on", "label": "On"},          (a boolean default: a check box)
//       {"key": "density", "label": "Haze", "min": 0, "max": 0.03},              (a number: a slider, its value in %)
//       {"key": "distance", "label": "Reach", "min": 30, "max": 200, "step": 10, "unit": "m"},   (shown as 100 m)
//       {"key": "mode", "label": "Mode", "options": [{"value": 0, "label": "Soft"}, {"value": 1, "label": "Hard"}]}
//     ]
//   }
// "kind" ("slider", "check", "choice") may be given; else it follows the default's type and "options".
//
// The pages are the loader's own (menus.cpp's owners kLoaderOwner: the list, kLoaderPage: a mod's page over it), so
// Esc on a mod's page returns to the list, and on the list to Options, as the game's own pages do. Both builds.
//
// Bridge methods (game thread): panel.state (the mods with settings, their controls and values, the open page),
// panel.open {mod} (the list, or a mod's page, opened over the open menu: the pause menu first if none is).
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

enum Kind { kSlider, kCheck, kChoice };
struct Control {
    std::string key, label, unit;
    Kind kind = kSlider;
    double min = 0, max = 1, step = 0;
    bool percent = true;              // a slider's value shown as % of its range (else the value, with its unit)
    std::vector<json> values;         // a choice's values
    std::vector<std::string> labels;  // and what is shown for them
};
struct Settings {
    std::string mod, title;
    std::vector<Control> controls;
};

// the page's ids: the keyboard walks a page by rising id
constexpr int kListRow = 100, kListBack = 400;
constexpr int kLabel = 200, kInput = 300, kValue = 500, kDefaults = 900, kBack = 901;
// A window's controls start under its title: 44 units below its top (measured on the game's own optionsb); its paper
// ends `h` below the top, the game's pages leave about 25 under their last row (optionsa: Back at 462..487 of 540)
constexpr float kHeader = 44, kFooter = 25;

std::string g_pageMod;   // the mod whose page is open (kLoaderPage)
int g_item;              // the Mods row's handle in Options

bool ParseSettings(MmlMod *mod, Settings *out, std::string *why) {
    std::string text = ModSettingsJson(mod);
    if (text.empty()) return false;
    json s = json::parse(text, nullptr, false);
    if (!s.is_object() || !s.contains("controls") || !s["controls"].is_array()) { *why = "\"settings\" has no \"controls\""; return false; }
    out->mod = ModIdOf(mod);
    out->title = s.value("title", "");
    if (out->title.empty()) {
        out->title = ModNameOf(mod);
        size_t colon = out->title.find(':');   // ("Sun shafts: the sun's light..." -> "Sun shafts")
        if (colon != std::string::npos && colon > 0) out->title.resize(colon);
    }
    for (const json &c : s["controls"]) {
        if (!c.is_object() || !c.contains("key") || !c["key"].is_string()) continue;
        Control k;
        k.key = c["key"].get<std::string>();
        k.label = c.value("label", k.key);
        k.unit = c.value("unit", "");
        std::string def = ConfigDefaultJson(mod, k.key.c_str());
        json d = def.empty() ? json() : json::parse(def, nullptr, false);
        std::string kind = c.value("kind", "");
        if (kind.empty()) kind = c.contains("options") ? "choice" : d.is_boolean() ? "check" : "slider";
        if (kind == "check") {
            k.kind = kCheck;
        } else if (kind == "choice") {
            k.kind = kChoice;
            if (!c.contains("options") || !c["options"].is_array() || c["options"].empty()) { *why = k.key + ": a choice with no \"options\""; continue; }
            for (const json &o : c["options"]) {
                json v = o.is_object() ? o.value("value", json()) : o;
                k.values.push_back(v);
                k.labels.push_back(o.is_object() ? o.value("label", v.dump()) : v.is_string() ? v.get<std::string>() : v.dump());
            }
        } else {
            k.kind = kSlider;
            if (!c.contains("min") || !c.contains("max") || !c["min"].is_number() || !c["max"].is_number() ||
                !(c["max"].get<double>() > c["min"].get<double>())) {
                *why = k.key + ": a slider needs \"min\" < \"max\"";
                continue;
            }
            k.min = c["min"].get<double>();
            k.max = c["max"].get<double>();
            k.step = c.value("step", 0.0);
            k.percent = c.value("show", k.unit.empty() ? std::string("percent") : std::string("value")) == "percent";
        }
        out->controls.push_back(std::move(k));
    }
    return !out->controls.empty();
}

std::vector<Settings> AllSettings() {
    std::vector<Settings> all;
    for (MmlMod *mod : ModsLoaded()) {
        Settings s;
        std::string why;
        if (ParseSettings(mod, &s, &why)) all.push_back(std::move(s));
        else if (!why.empty()) Log(MML_LOG_WARN, ModIdOf(mod), "settings: %s", why.c_str());
    }
    return all;
}

bool Find(const std::string &id, Settings *out) {
    MmlMod *mod = ModById(id);
    std::string why;
    return mod && ParseSettings(mod, out, &why);
}

double Number(const std::string &j, double def) {
    json v = j.empty() ? json() : json::parse(j, nullptr, false);
    if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    return v.is_number() ? v.get<double>() : def;
}

// A slider's 0..1 for a value, and back (snapped to its step; a whole step and whole ends: a whole number).
float ToSlider(const Control &k, double v) { return static_cast<float>(std::clamp((v - k.min) / (k.max - k.min), 0.0, 1.0)); }
json FromSlider(const Control &k, float s) {
    double v = k.min + std::clamp(static_cast<double>(s), 0.0, 1.0) * (k.max - k.min);
    if (k.step > 0) v = std::clamp(k.min + std::round((v - k.min) / k.step) * k.step, k.min, k.max);
    bool whole = k.step >= 1 && k.step == std::floor(k.step) && k.min == std::floor(k.min);
    if (whole) return static_cast<long long>(std::llround(v));
    return std::round(v * 1e6) / 1e6;
}

std::string Shown(const Control &k, MmlMod *mod) {
    std::string j = ConfigValueJson(mod, k.key.c_str());
    char b[64];
    if (k.kind == kCheck) return "";
    if (k.kind == kChoice) {
        json v = j.empty() ? json() : json::parse(j, nullptr, false);
        for (size_t i = 0; i < k.values.size(); i++) if (k.values[i] == v) return k.labels[i];
        return v.dump();
    }
    double v = Number(j, k.min);
    if (k.percent) snprintf(b, sizeof b, "%d%%", static_cast<int>(std::lround(ToSlider(k, v) * 100)));
    else snprintf(b, sizeof b, "%g%s%s", std::round(v * 1000) / 1000, k.unit.empty() ? "" : " ", k.unit.c_str());
    return b;
}

// The controls' values as the mod's settings have them now, and the texts beside the sliders.
void ShowValues(const Settings &s, MmlMod *mod) {
    for (size_t i = 0; i < s.controls.size(); i++) {
        const Control &k = s.controls[i];
        int id = kInput + static_cast<int>(i);
        std::string j = ConfigValueJson(mod, k.key.c_str());
        if (k.kind == kCheck) {
            MenuPageValue(kLoaderPage, id, Number(j, 0) != 0 ? 1.0f : 0.0f);
        } else if (k.kind == kChoice) {
            json v = j.empty() ? json() : json::parse(j, nullptr, false);
            int at = 0;
            for (size_t o = 0; o < k.values.size(); o++) if (k.values[o] == v) at = static_cast<int>(o);
            MenuPageValue(kLoaderPage, id, static_cast<float>(at));
        } else {
            MenuPageValue(kLoaderPage, id, ToSlider(k, Number(j, k.min)));
            MenuPageText(kLoaderPage, kValue + static_cast<int>(i), Shown(k, mod).c_str());
        }
    }
}

void OnModPage(int id, void *) {
    Settings s;
    MmlMod *mod = ModById(g_pageMod);
    if (!mod || !Find(g_pageMod, &s)) { MenuPageClose(kLoaderPage); return; }   // (the mod is gone)
    if (id == kBack) { ConfigFlush(mod); MenuPageClose(kLoaderPage); return; }
    if (id == kDefaults) {
        for (const Control &k : s.controls) ConfigSetLive(mod, k.key.c_str(), "null");
        ShowValues(s, mod);
        return;
    }
    int i = id - kInput;
    if (i < 0 || i >= static_cast<int>(s.controls.size())) return;
    const Control &k = s.controls[static_cast<size_t>(i)];
    float v = 0;
    if (!MenuPageGetValue(kLoaderPage, id, &v)) return;
    json value;
    if (k.kind == kCheck) value = v > 0.5f;
    else if (k.kind == kChoice) value = k.values[std::min(k.values.size() - 1, static_cast<size_t>(std::max(0.0f, v)))];
    else value = FromSlider(k, v);
    ConfigSetLive(mod, k.key.c_str(), value.dump());
    if (k.kind == kSlider) MenuPageText(kLoaderPage, kValue + i, Shown(k, mod).c_str());
}

// A mod's page, over the list (or over the open menu).
bool OpenModPage(const std::string &modId, std::string *err) {
    Settings s;
    MmlMod *mod = ModById(modId);
    if (!mod || !Find(modId, &s)) { *err = "no loaded mod \"" + modId + "\" with settings"; return false; }
    std::vector<MmlMenuControl> c;
    const float row = 25, top = 20, w = 460;
    float bottom = top + row * static_cast<float>(s.controls.size()) + 15;
    float h = kHeader + bottom + 25 + kFooter;
    c.push_back({1, "wind", (800 - w) / 2, std::max(20.0f, (600 - h) / 2), w, h, s.title.c_str(), 0});
    std::vector<std::string> keep;   // (a selector's options, alive through the call: the menus intern what they keep)
    keep.reserve(s.controls.size());
    for (size_t i = 0; i < s.controls.size(); i++) {
        const Control &k = s.controls[i];
        float y = top + row * static_cast<float>(i);
        int n = static_cast<int>(i);
        c.push_back({kLabel + n, "stxt", 30, y, 160, 18, k.label.c_str(), 0});
        if (k.kind == kCheck) {
            c.push_back({kInput + n, "chck", 200, y, 20, 20, nullptr, 0});
        } else if (k.kind == kChoice) {
            std::string options;
            for (size_t o = 0; o < k.labels.size(); o++) options += (o ? "|" : "") + k.labels[o];
            keep.push_back(options);
            c.push_back({kInput + n, "bsel", 200, y - 2, 200, 22, keep.back().c_str(), 0});
        } else {
            c.push_back({kInput + n, "slid", 200, y + 1, 160, 16, nullptr, 0});
            c.push_back({kValue + n, "stxt", 375, y, 60, 18, " ", 0});
        }
    }
    c.push_back({kDefaults, "butt", 30, bottom, 170, 25, "Defaults", 0});
    c.push_back({kBack, "butt", w - 130, bottom, 100, 25, "Back", 0});
    if (!MenuPageOpen(kLoaderPage, "mod", c.data(), static_cast<int>(c.size()), OnModPage, nullptr)) {
        *err = "the game did not make the page (the log tells)";
        return false;
    }
    g_pageMod = modId;
    ShowValues(s, mod);   // (applied when the game makes the page)
    return true;
}

std::vector<std::string> g_listed;   // the list's rows: their mods

void OnList(int id, void *) {
    if (id == kListBack) { MenuPageClose(kLoaderOwner); return; }
    size_t i = static_cast<size_t>(id - kListRow);
    if (id < kListRow || i >= g_listed.size()) return;
    std::string err;
    if (!OpenModPage(g_listed[i], &err)) Log(MML_LOG_WARN, "host", "panel: %s", err.c_str());
}

bool OpenList(std::string *err) {
    std::vector<Settings> all = AllSettings();
    g_listed.clear();
    std::vector<MmlMenuControl> c;
    const float row = 30, w = 260;
    float rows = static_cast<float>(std::max<size_t>(all.size(), 1));
    float h = kHeader + 10 + row * rows + 10 + 20 + kFooter;
    c.push_back({1, "wind", 290, 180, w, h, "Mods", 0});
    if (all.empty()) c.push_back({2, "stxt", 15, 15, w - 30, 18, "No loaded mod has settings", 0});
    for (size_t i = 0; i < all.size(); i++) {
        g_listed.push_back(all[i].mod);
        c.push_back({kListRow + static_cast<int>(i), "item", 0, 10 + row * static_cast<float>(i), w, row, all[i].title.c_str(), 0});
    }
    c.push_back({kListBack, "item", 0, 10 + row * rows + 10, w, 20, "Back", 0});
    if (!MenuPageOpen(kLoaderOwner, "mods", c.data(), static_cast<int>(c.size()), OnList, nullptr)) {
        *err = "the game did not make the page (the log tells)";
        return false;
    }
    return true;
}

void OnOptionsItem(int, void *) {
    std::string err;
    if (!OpenList(&err)) Log(MML_LOG_WARN, "host", "panel: %s", err.c_str());
}

} // namespace

void PanelInit() {
    // Options (optionsb in both builds: 100 Controls, 101 Audio and Video, 400 Back): Mods under Audio and Video
    g_item = MenuItemAdd(kLoaderOwner, "optionsb", 101, "Mods", OnOptionsItem, nullptr);
    if (!g_item) HWARN("panel: the Mods row could not be asked for in Options");
}

bool PanelMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "panel.state" && method != "panel.open") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    if (method == "panel.open") {
        std::string mod = p.value("mod", ""), err;
        bool done = mod.empty() ? OpenList(&err) : OpenModPage(mod, &err);
        if (!done) { *reply = "panel.open: " + err; return true; }
        *reply = json{{"opened", mod.empty() ? "mods" : mod}}.dump();
        *ok = true;
        return true;
    }
    json mods = json::array();
    for (const Settings &s : AllSettings()) {
        MmlMod *mod = ModById(s.mod);
        json controls = json::array();
        for (const Control &k : s.controls) {
            std::string v = ConfigValueJson(mod, k.key.c_str());
            json c = {{"key", k.key}, {"label", k.label}, {"kind", k.kind == kCheck ? "check" : k.kind == kChoice ? "choice" : "slider"},
                      {"value", v.empty() ? json() : json::parse(v, nullptr, false)}, {"shown", Shown(k, mod)}};
            if (k.kind == kSlider) { c["min"] = k.min; c["max"] = k.max; if (k.step > 0) c["step"] = k.step; }
            controls.push_back(c);
        }
        mods.push_back({{"mod", s.mod}, {"title", s.title}, {"controls", controls}});
    }
    *reply = json{{"item", g_item}, {"mods", mods}, {"page", g_pageMod}}.dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
