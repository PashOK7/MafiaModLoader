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
// or, for a mod of several parts, "sections": [{"title": "Routes", "controls": [...]}, ...] in place of "controls".
// "kind" ("slider", "check", "choice") may be given; else it follows the default's type and "options".
// A label of more than 20 characters takes a second line (40 at most: the label column is 160 px wide). A slider's
// value is shown as a percentage of its range, or as the value itself with "show": "value" (a number the mod reads).
//
// A page never runs off the screen: a section whose rows do not fit is cut into parts. When the page has more than one
// part or section, they are tabs as the game's Controls has them (optionsc1: Player, Car, Other): buttons along the
// bottom of a wider window, before Defaults and Back (in a row of their own above them when they do not fit there),
// the window titled with the tab shown ("Debug views: Routes"). A tab makes the page again with its part (this one
// closed, that one opened once the game has let the old one go: the game's own tabs are three menus too).
//
// The pages are the loader's own (menus.cpp's owners kLoaderOwner: the list, kLoaderPage: a mod's page over it), so
// Esc on a mod's page returns to the list, and on the list to Options, as the game's own pages do. Both builds.
//
// A page is laid out apart from the game (LayoutPage, LayoutList: every control's place and text, and what a value
// field may come to show), then made from that: panel.layout hands the same layout out for any manifest, with no game,
// so tests/run_tests.py checks every mod's pages for controls and texts laid over each other or cut.
//
// Bridge methods (game thread): panel.state (the mods with settings, their sections, controls and values, the parts
// their pages are cut into, the open page), panel.open {mod, part} (the list, or a mod's page, opened over the open
// menu: the pause menu first if none is), panel.layout {settings, defaults, name} (a manifest's pages laid out, every
// part, no game needed; with no settings: every loaded mod's pages and the list).
#include "host.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

enum Kind { kSlider, kCheck, kChoice, kKey };
struct Control {
    std::string key, label, unit;
    Kind kind = kSlider;
    double min = 0, max = 1, step = 0;
    bool percent = true;              // a slider's value shown as % of its range (else the value, with its unit)
    std::vector<json> values;         // a choice's values
    std::vector<std::string> labels;  // and what is shown for them
};
struct Section {
    std::string title;
    std::vector<Control> controls;
};
struct Settings {
    std::string mod, title;
    std::vector<Section> sections;   // one with no title when the manifest gives "controls"
};
// a page's part: some of one section's controls, and its tab's name
struct Part {
    size_t section = 0, first = 0, count = 0;
    std::string label;
};

// the page's ids: the keyboard walks a page by rising id
constexpr int kListRow = 100, kListBack = 400;
constexpr int kTab = 150, kLabel = 200, kInput = 300, kValue = 500, kLabel2 = 600, kDefaults = 900, kBack = 901;
// A label's column is 160 px: about 20 characters of the game's font. A longer label takes a second line.
constexpr size_t kLabelChars = 20;
// A window's controls start under its title: 44 units below its top (measured on the game's own optionsb); its paper
// ends `h` below the top, the game's pages leave about 25 under their last row (optionsa: Back at 462..487 of 540)
constexpr float kHeader = 44, kFooter = 25;
// The page's rows: one 25 units high, one whose label takes two lines 36. The rows start 20 below the controls' top
// and the buttons 15 under them, 25 high. The screen is 600 units: the page keeps 20 above and below, so its rows
// have 600 - 40 - 44 - 20 - 15 - 25 - 25 = 431 units, less 30 for each row of tabs above the buttons.
constexpr float kRow1 = 25, kRow2 = 36, kTabRow = 30, kTop = 20, kScreen = 600, kMargin = 20;
constexpr float kRowsMax = kScreen - 2 * kMargin - kHeader - kTop - 15 - 25 - kFooter;
// The window: 460 wide, 580 with tabs (the game's Controls is 550). The tabs as the game's, side by side from 20 (its
// are 100 wide and overlap by 10: ours touch, so a click is never between two), each 90 wide or as wide as its text
// needs; Defaults 90 and Back 70 at the row's end (395..485, 490..560). The tabs stand beside them while they fit (to
// 390), else in rows of their own above them (20..560), in their order.
constexpr float kWidth = 460, kTabbedWidth = 580, kTabW = 90, kDefaultsW = 90, kBackW = 70;
constexpr float kTabsLeft = 20, kTabsBesideRight = kTabbedWidth - 20 - kBackW - 5 - kDefaultsW - 5, kTabsRight = kTabbedWidth - 20;

// A text's width in the menus' fonts (800x600 units), estimated: the game's button font measured on its own pages
// (2026-10-18, the 1.2 lab, Options > Mods: "Triggers" 72, "Collision" 75, "Defaults" 69-70, "Web" 36, "Names" 55,
// "Burning" 67, "Heat and motion" 139), the static texts' font ("Read within" 85, "Seen through the" 125, "Motion
// damping" 117), a window's title ("Gas and fire: Heat and motion" 303). The table times its font's factor comes out
// a few percent above every one of them. Not the game's own measure (a page is laid out with no game: panel.layout):
// an estimate from above. The layout gives a tab the room its text needs; tests/run_tests.py checks every page by it.
constexpr float kButtonFont = 1.05f, kTextFont = 0.9f, kTitleFont = 1.12f;
// What a key's row shows while it waits for the key (Esc keeps the one it had): within its 200-wide button ("Press a
// key (Esc: keep)" was wider, found by tests/run_tests.py's check of the pages).
constexpr const char *kKeyPrompt = "Press a key (Esc)";
float TextWidth(const std::string &t, float font) {
    float w = 0;
    for (unsigned char ch : t) {
        if (strchr("il.,:;'|!", ch)) w += 5;
        else if (strchr("fjtrI()[]/-", ch)) w += 7;
        else if (ch == ' ') w += 5;
        else if (ch == 'M' || ch == 'W') w += 16;
        else if (ch == 'm' || ch == 'w' || ch == '%') w += 14;
        else if (isupper(ch)) w += 13;
        else w += 10;
    }
    return w * font;
}
// The tabs' places: each one's x, row (0: the buttons' row, beside Defaults and Back; 1..: rows of their own above
// them, the first the highest) and width, in the parts' order; the rows of their own they take.
struct TabSpot { float x, w; size_t row; };
std::vector<TabSpot> TabSpots(const std::vector<std::string> &labels, size_t *rowsOwn) {
    std::vector<TabSpot> spots;
    std::vector<float> widths;
    float all = 0;
    for (const std::string &l : labels) {
        widths.push_back(std::max(kTabW, std::ceil(TextWidth(l, kButtonFont) + 16)));
        all += widths.back();
    }
    *rowsOwn = 0;
    if (labels.size() <= 1) return spots;
    if (kTabsLeft + all <= kTabsBesideRight) {   // beside Defaults and Back
        float x = kTabsLeft;
        for (float w : widths) { spots.push_back({x, w, 0}); x += w; }
        return spots;
    }
    float x = kTabsLeft;
    size_t row = 1;
    for (float w : widths) {
        if (x > kTabsLeft && x + w > kTabsRight) { row++; x = kTabsLeft; }
        spots.push_back({x, std::min(w, kTabsRight - kTabsLeft), row});
        x += w;
    }
    *rowsOwn = row;
    return spots;
}
size_t TabRows(const std::vector<std::string> &labels) {
    size_t rows = 0;
    TabSpots(labels, &rows);
    return rows;
}
// The tab whose button is the k-th (its id kTab + k): the 0th is the tab shown, the others in their order after it.
size_t TabPart(size_t k, size_t shown) { return k == 0 ? shown : (k <= shown ? k - 1 : k); }

std::string g_pageMod;   // the mod whose page is open (kLoaderPage)
size_t g_pagePart = 0;   // its part shown
long g_reopen = -1;      // the part to show once the page's old menu is gone (its tab was chosen), -1: none
int g_grabbing = -1;     // the "key" row waiting for a key (input.cpp KeyGrab*), -1: none
int g_item;              // the Mods row's handle in Options

using DefaultOf = std::function<std::string(const std::string &key)>;   // a setting's default, as JSON text ("": none)

void ParseControls(const DefaultOf &defaultOf, const json &list, std::vector<Control> *out, std::string *why) {
    for (const json &c : list) {
        if (!c.is_object() || !c.contains("key") || !c["key"].is_string()) continue;
        Control k;
        k.key = c["key"].get<std::string>();
        k.label = c.value("label", k.key);
        k.unit = c.value("unit", "");
        std::string def = defaultOf(k.key);
        json d = def.empty() ? json() : json::parse(def, nullptr, false);
        std::string kind = c.value("kind", "");
        if (kind.empty()) kind = c.contains("options") ? "choice" : d.is_boolean() ? "check" : "slider";
        if (kind == "key") {
            k.kind = kKey;   // (a hotkey's combo: "F7", "Ctrl+Shift+R")
        } else if (kind == "check") {
            k.kind = kCheck;
        } else if (kind == "choice") {
            k.kind = kChoice;
            if (!c.contains("options") || !c["options"].is_array() || c["options"].empty()) { *why = k.key + ": a choice with no \"options\""; continue; }
            for (const json &o : c["options"]) {
                json v = o.is_object() ? o.value("value", json()) : o;
                k.values.push_back(v);
                k.labels.push_back(o.is_object() ? o.value("label", v.dump(-1, ' ', false, json::error_handler_t::replace)) : v.is_string() ? v.get<std::string>() : v.dump(-1, ' ', false, json::error_handler_t::replace));
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
        out->push_back(std::move(k));
    }
}

// A manifest's "settings" (s), its mod's id and name (the title's fallback: the name up to a colon).
bool ParseSettingsJson(const json &s, const std::string &id, const std::string &name, const DefaultOf &defaultOf, Settings *out,
                       std::string *why) {
    const bool listed = s.is_object() && s.contains("controls") && s["controls"].is_array(),
               sectioned = s.is_object() && s.contains("sections") && s["sections"].is_array();
    if (!listed && !sectioned) { *why = "\"settings\" has neither \"controls\" nor \"sections\""; return false; }
    out->mod = id;
    out->title = s.value("title", "");
    if (out->title.empty()) {
        out->title = name;
        size_t colon = out->title.find(':');   // ("Sun shafts: the sun's light..." -> "Sun shafts")
        if (colon != std::string::npos && colon > 0) out->title.resize(colon);
    }
    if (listed) {
        Section one;
        ParseControls(defaultOf, s["controls"], &one.controls, why);
        if (!one.controls.empty()) out->sections.push_back(std::move(one));
    }
    if (sectioned)
        for (const json &x : s["sections"]) {
            if (!x.is_object() || !x.contains("controls") || !x["controls"].is_array()) { *why = "a section with no \"controls\""; continue; }
            Section sec;
            sec.title = x.value("title", "");
            ParseControls(defaultOf, x["controls"], &sec.controls, why);
            if (sec.title.empty()) sec.title = "Part " + std::to_string(out->sections.size() + 1);
            if (!sec.controls.empty()) out->sections.push_back(std::move(sec));
        }
    return !out->sections.empty();
}

bool ParseSettings(MmlMod *mod, Settings *out, std::string *why) {
    std::string text = ModSettingsJson(mod);
    if (text.empty()) return false;
    return ParseSettingsJson(json::parse(text, nullptr, false), ModIdOf(mod), ModNameOf(mod),
                             [mod](const std::string &key) { return ConfigDefaultJson(mod, key.c_str()); }, out, why);
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
    if (k.kind == kKey) {
        json v = j.empty() ? json() : json::parse(j, nullptr, false);
        return v.is_string() && !v.get<std::string>().empty() ? v.get<std::string>() : std::string("(none)");
    }
    if (k.kind == kChoice) {
        json v = j.empty() ? json() : json::parse(j, nullptr, false);
        for (size_t i = 0; i < k.values.size(); i++) if (k.values[i] == v) return k.labels[i];
        return v.dump(-1, ' ', false, json::error_handler_t::replace);
    }
    double v = Number(j, k.min);
    if (k.percent) snprintf(b, sizeof b, "%d%%", static_cast<int>(std::lround(ToSlider(k, v) * 100)));
    else snprintf(b, sizeof b, "%g%s%s", std::round(v * 1000) / 1000, k.unit.empty() ? "" : " ", k.unit.c_str());
    return b;
}

// A label as one line, or two: a longer one is split at the last space that fits in a line (the rest is the second
// line). A label whose second line is still too long is cut there: the manifests keep their labels to two lines.
std::vector<std::string> LabelLines(const std::string &s, bool *lost = nullptr) {
    if (lost) *lost = false;
    if (s.size() <= kLabelChars) return {s};
    size_t cut = s.rfind(' ', kLabelChars);
    if (cut == std::string::npos || cut == 0) cut = kLabelChars;
    const size_t next = (cut < s.size() && s[cut] == ' ') ? cut + 1 : cut;
    if (lost) *lost = s.size() - next > kLabelChars;
    return {s.substr(0, cut), s.substr(next, kLabelChars)};
}

float RowHeight(const Control &k) { return LabelLines(k.label).size() > 1 ? kRow2 : kRow1; }

// A section's controls cut where their rows would pass `room` (one part when they fit).
std::vector<Part> Cut(const std::vector<Control> &cs, size_t si, float room) {
    std::vector<Part> mine;
    float used = 0;
    for (size_t i = 0; i < cs.size(); i++) {
        const float h = RowHeight(cs[i]);
        if (mine.empty() || (used + h > room && mine.back().count)) {
            mine.push_back({si, i, 0, ""});
            used = 0;
        }
        mine.back().count++;
        used += h;
    }
    return mine;
}

// The parts the page is cut into: each section whole when its rows fit the screen (less the rows of tabs above the
// buttons, when there are so many tabs), else in as few pieces as fit, of about the same height (no last part of a
// row or two). One part, and no tabs, when everything fits.
std::vector<Part> Parts(const Settings &s) {
    for (size_t tabRows = 0; tabRows < 4; tabRows++) {
        const float room = kRowsMax - kTabRow * static_cast<float>(tabRows);
        std::vector<Part> parts;
        for (size_t si = 0; si < s.sections.size(); si++) {
            const std::vector<Control> &cs = s.sections[si].controls;
            std::vector<Part> mine = Cut(cs, si, room);
            if (mine.size() > 1) {   // the same count of parts, evened: each cut near the average height
                float total = 0;
                for (const Control &k : cs) total += RowHeight(k);
                std::vector<Part> even = Cut(cs, si, std::min(room, total / static_cast<float>(mine.size()) + kRow2));
                if (even.size() == mine.size()) mine = even;
            }
            for (size_t p = 0; p < mine.size(); p++) {
                mine[p].label = s.sections[si].title.empty() ? s.title : s.sections[si].title;
                if (mine.size() > 1) mine[p].label += " " + std::to_string(p + 1) + "/" + std::to_string(mine.size());
            }
            parts.insert(parts.end(), mine.begin(), mine.end());
        }
        std::vector<std::string> labels;
        for (const Part &p : parts) labels.push_back(p.label);
        if (TabRows(labels) <= tabRows || tabRows == 3) return parts;   // (else cut again under the tabs' rows)
    }
    return {};
}

const Control *PartControl(const Settings &s, const Part &p, size_t i) {
    return i < p.count ? &s.sections[p.section].controls[p.first + i] : nullptr;
}

// The part's controls' values as the mod's settings have them now, and the texts beside the sliders.
void ShowValues(const Settings &s, const std::vector<Part> &parts, size_t part, MmlMod *mod) {
    if (part >= parts.size()) return;
    for (size_t i = 0; i < parts[part].count; i++) {
        const Control &k = *PartControl(s, parts[part], i);
        int id = kInput + static_cast<int>(i);
        std::string j = ConfigValueJson(mod, k.key.c_str());
        if (k.kind == kKey) {
            MenuPageText(kLoaderPage, id, Shown(k, mod).c_str());
        } else if (k.kind == kCheck) {
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
    const std::vector<Part> parts = Parts(s);
    if (g_pagePart >= parts.size()) { MenuPageClose(kLoaderPage); return; }     // (its manifest changed under the page)
    const Part &part = parts[g_pagePart];
    if (id == kBack) { KeyGrabStop(); g_grabbing = -1; g_reopen = -1; ConfigFlush(mod); MenuPageClose(kLoaderPage); return; }
    if (id >= kTab && id < kTab + static_cast<int>(parts.size())) {   // a tab: this page closed, that one opened
        const size_t to = TabPart(static_cast<size_t>(id - kTab), g_pagePart);   // once the game let it go (PanelFrame)
        if (to == g_pagePart) return;
        KeyGrabStop();
        g_grabbing = -1;
        g_reopen = static_cast<long>(to);
        MenuPageClose(kLoaderPage);
        return;
    }
    if (id == kDefaults) {   // the part's controls, those shown
        for (size_t i = 0; i < part.count; i++) ConfigSetLive(mod, PartControl(s, part, i)->key.c_str(), "null");
        ShowValues(s, parts, g_pagePart, mod);
        return;
    }
    int i = id - kInput;
    if (i < 0 || i >= static_cast<int>(part.count)) return;
    const Control &k = *PartControl(s, part, static_cast<size_t>(i));
    if (k.kind == kKey) {   // the next key pressed is the setting (Esc: as it was); PanelFrame takes it
        if (g_grabbing >= 0 && static_cast<size_t>(g_grabbing) < part.count)
            MenuPageText(kLoaderPage, kInput + g_grabbing, Shown(*PartControl(s, part, static_cast<size_t>(g_grabbing)), mod).c_str());
        g_grabbing = i;
        KeyGrabStart();
        MenuPageText(kLoaderPage, id, kKeyPrompt);
        return;
    }
    float v = 0;
    if (!MenuPageGetValue(kLoaderPage, id, &v)) return;
    json value;
    if (k.kind == kCheck) value = v > 0.5f;
    else if (k.kind == kChoice) value = k.values[std::min(k.values.size() - 1, static_cast<size_t>(std::max(0.0f, v)))];
    else value = FromSlider(k, v);
    ConfigSetLive(mod, k.key.c_str(), value.dump(-1, ' ', false, json::error_handler_t::replace));
    if (k.kind == kSlider) MenuPageText(kLoaderPage, kValue + i, Shown(k, mod).c_str());
}

// A page as the game is asked to make it: the window first, then its controls (places inside the window), each with
// its text; `sample`: the longest text a value field or a key's button may come to show (its text is " " until the
// values are shown); `cut`: the labels that lost text (more than two lines).
struct Item { int id; std::string kind; float x, y, w, h; std::string text, sample; };
// The estimated width of what an item shows (its text or its sample, the wider): a window's title, a static text's,
// a button's or a list row's label (a choice's widest option).
float InkWidth(const Item &i) {
    const float font = i.kind == "wind" ? kTitleFont : i.kind == "stxt" ? kTextFont : kButtonFont;
    float w = 0;
    std::string t = i.text;
    if (i.kind == "bsel") t = i.sample;   // (its options are "A|B|C": the widest is its sample)
    for (const std::string &x : {t, i.sample}) if (!x.empty() && x != " ") w = std::max(w, TextWidth(x, font));
    return w;
}
struct Page {
    std::vector<Item> items;
    std::vector<std::string> cut;
};

// What a slider's value field may show at most: its ends (as Shown writes them) and 100%.
std::string SliderSample(const Control &k) {
    if (k.percent) return "100%";
    char a[64], b[64];
    snprintf(a, sizeof a, "%g%s%s", std::round(k.min * 1000) / 1000, k.unit.empty() ? "" : " ", k.unit.c_str());
    snprintf(b, sizeof b, "%g%s%s", std::round(k.max * 1000) / 1000, k.unit.empty() ? "" : " ", k.unit.c_str());
    return strlen(a) > strlen(b) ? a : b;
}

// One part of a mod's page laid out.
void LayoutPage(const Settings &s, const std::vector<Part> &parts, size_t partAt, Page *out) {
    const Part &part = parts[partAt];
    // a row is 25 units high; a row whose label takes two lines is 36, its control centred between them
    const bool tabbed = parts.size() > 1;
    const float w = tabbed ? kTabbedWidth : kWidth, dx = (w - kWidth) / 2;   // (the rows centred in a wider window)
    std::vector<std::string> tabLabels;
    for (const Part &p : parts) tabLabels.push_back(p.label);
    size_t tabRows = 0;
    const std::vector<TabSpot> spots = TabSpots(tabLabels, &tabRows);
    const float top = kTop;
    std::vector<std::vector<std::string>> labels(part.count);
    std::vector<float> rowTop(part.count);
    float rows = 0;
    for (size_t i = 0; i < part.count; i++) {
        bool lost = false;
        labels[i] = LabelLines(PartControl(s, part, i)->label, &lost);
        if (lost) out->cut.push_back(PartControl(s, part, i)->label);
        rowTop[i] = top + rows;
        rows += labels[i].size() > 1 ? kRow2 : kRow1;
    }
    const float tabsTop = top + rows + 15;                                  // the tabs' own rows, when there are such
    const float bottom = tabsTop + kTabRow * static_cast<float>(tabRows);   // the buttons' row
    float h = kHeader + bottom + 25 + kFooter;
    const std::string title = tabbed ? s.title + ": " + part.label : s.title;
    std::vector<Item> &c = out->items;
    c.push_back({1, "wind", (800 - w) / 2, std::max(kMargin, (kScreen - h) / 2), w, h, title, ""});
    // The tabs: in the buttons' row, or rows of their own, in their order. The tab shown is the first of the page's
    // buttons, by id and in the definition: the game gives the focus (its underline) to that one when it makes the
    // page (1.2 the lowest id, 1.0 the first record), so the underline says which tab is open. A focus flag in the
    // definition does not move it, only adds a second underline.
    for (size_t k = 0; tabbed && k < parts.size(); k++) {
        const size_t p = TabPart(k, partAt);
        const TabSpot &t = spots[p];
        const float y = t.row ? tabsTop + kTabRow * static_cast<float>(t.row - 1) : bottom;
        c.push_back({kTab + static_cast<int>(k), "butt", t.x, y, t.w, 25, parts[p].label, ""});
    }
    for (size_t i = 0; i < part.count; i++) {
        const Control &k = *PartControl(s, part, i);
        const bool two = labels[i].size() > 1;
        const float ry = rowTop[i];
        const float y = ry + (two ? 6 : 0);   // the control's line (centred in a two-line row)
        int n = static_cast<int>(i);
        c.push_back({kLabel + n, "stxt", dx + 30, two ? ry + 3 : ry, 160, 18, labels[i][0], ""});
        if (two) c.push_back({kLabel2 + n, "stxt", dx + 30, ry + 18, 160, 18, labels[i][1], ""});
        if (k.kind == kKey) {
            c.push_back({kInput + n, "butt", dx + 200, y - 2, 200, 22, " ", kKeyPrompt});
        } else if (k.kind == kCheck) {
            c.push_back({kInput + n, "chck", dx + 200, y, 20, 20, "", ""});
        } else if (k.kind == kChoice) {
            std::string options, longest;
            for (size_t o = 0; o < k.labels.size(); o++) {
                options += (o ? "|" : "") + k.labels[o];
                if (k.labels[o].size() > longest.size()) longest = k.labels[o];
            }
            c.push_back({kInput + n, "bsel", dx + 200, y - 2, 200, 22, options, longest});
        } else {
            c.push_back({kInput + n, "slid", dx + 200, y + 1, 160, 16, "", ""});
            c.push_back({kValue + n, "stxt", dx + 375, y, 60, 18, " ", SliderSample(k)});
        }
    }
    if (tabbed) {   // as the game's Controls: Defaults and Back at the row's end, after the tabs
        c.push_back({kDefaults, "butt", w - 20 - kBackW - 5 - kDefaultsW, bottom, kDefaultsW, 25, "Defaults", ""});
        c.push_back({kBack, "butt", w - 20 - kBackW, bottom, kBackW, 25, "Back", ""});
    } else {
        c.push_back({kDefaults, "butt", 30, bottom, 170, 25, "Defaults", ""});
        c.push_back({kBack, "butt", w - 130, bottom, 100, 25, "Back", ""});
    }
}

// The game's controls for a laid-out page (the texts stay the page's).
std::vector<MmlMenuControl> Controls(const Page &page) {
    std::vector<MmlMenuControl> c;
    for (const Item &i : page.items) c.push_back({i.id, i.kind.c_str(), i.x, i.y, i.w, i.h, i.text.empty() ? nullptr : i.text.c_str(), 0});
    return c;
}

// A mod's page (one part of it), over the list (or over the open menu).
bool OpenModPage(const std::string &modId, size_t partAt, std::string *err) {
    Settings s;
    MmlMod *mod = ModById(modId);
    if (!mod || !Find(modId, &s)) { *err = "no loaded mod \"" + modId + "\" with settings"; return false; }
    const std::vector<Part> parts = Parts(s);
    if (partAt >= parts.size()) { *err = "part " + std::to_string(partAt) + ": the page has " + std::to_string(parts.size()); return false; }
    Page page;
    LayoutPage(s, parts, partAt, &page);
    std::vector<MmlMenuControl> c = Controls(page);
    if (!MenuPageOpen(kLoaderPage, "mod", c.data(), static_cast<int>(c.size()), OnModPage, nullptr)) {
        *err = "the game did not make the page (the log tells)";
        return false;
    }
    g_pageMod = modId;
    g_pagePart = partAt;
    ShowValues(s, parts, partAt, mod);   // (applied when the game makes the page)
    return true;
}

std::vector<std::string> g_listed;   // the list's rows: their mods

void OnList(int id, void *) {
    if (id == kListBack) { MenuPageClose(kLoaderOwner); return; }
    size_t i = static_cast<size_t>(id - kListRow);
    if (id < kListRow || i >= g_listed.size()) return;
    std::string err;
    if (!OpenModPage(g_listed[i], 0, &err)) Log(MML_LOG_WARN, "host", "panel: %s", err.c_str());
}

// The Mods list laid out: a row a mod with settings, Back.
void LayoutList(const std::vector<Settings> &all, Page *out) {
    std::vector<Item> &c = out->items;
    const float row = 30, w = 260;
    float rows = static_cast<float>(std::max<size_t>(all.size(), 1));
    float h = kHeader + 10 + row * rows + 10 + 20 + kFooter;
    c.push_back({1, "wind", 290, 180, w, h, "Mods", ""});
    if (all.empty()) c.push_back({2, "stxt", 15, 15, w - 30, 18, "No loaded mod has settings", ""});
    for (size_t i = 0; i < all.size(); i++)
        c.push_back({kListRow + static_cast<int>(i), "item", 0, 10 + row * static_cast<float>(i), w, row, all[i].title, ""});
    c.push_back({kListBack, "item", 0, 10 + row * rows + 10, w, 20, "Back", ""});
}

bool OpenList(std::string *err) {
    std::vector<Settings> all = AllSettings();
    g_listed.clear();
    for (const Settings &s : all) g_listed.push_back(s.mod);
    Page page;
    LayoutList(all, &page);
    std::vector<MmlMenuControl> c = Controls(page);
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

// Each frame (game_hooks.cpp): the part whose tab was chosen made once the old page is gone; a key taken for a "key"
// row made the mod's setting.
void PanelFrame() {
    if (g_reopen >= 0 && !MenuPageIsOpen(kLoaderPage)) {
        const size_t part = static_cast<size_t>(g_reopen);
        g_reopen = -1;
        std::string err;
        if (!OpenModPage(g_pageMod, part, &err)) Log(MML_LOG_WARN, "host", "panel: %s", err.c_str());
    }
    if (g_grabbing < 0) return;
    std::string combo;
    int r = KeyGrabResult(&combo);
    if (!r) {
        if (!KeyGrabbing()) g_grabbing = -1;   // (stopped elsewhere)
        return;
    }
    Settings s;
    MmlMod *mod = ModById(g_pageMod);
    int i = g_grabbing;
    g_grabbing = -1;
    if (!mod || !Find(g_pageMod, &s)) return;
    const std::vector<Part> parts = Parts(s);
    if (g_pagePart >= parts.size() || i >= static_cast<int>(parts[g_pagePart].count)) return;
    const Control &k = *PartControl(s, parts[g_pagePart], static_cast<size_t>(i));
    if (r > 0) ConfigSetLive(mod, k.key.c_str(), json(combo).dump(-1, ' ', false, json::error_handler_t::replace));
    MenuPageText(kLoaderPage, kInput + i, Shown(k, mod).c_str());
}

void PanelInit() {
    // Options (optionsb in both builds: 100 Controls, 101 Audio and Video, 400 Back): Mods under Audio and Video
    g_item = MenuItemAdd(kLoaderOwner, "optionsb", 101, "Mods", OnOptionsItem, nullptr);
    if (!g_item) HWARN("panel: the Mods row could not be asked for in Options");
}

bool PanelMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "panel.state" && method != "panel.open" && method != "panel.layout") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto pageJson = [](const Page &page) {
        json items = json::array();
        for (const Item &i : page.items) {
            json j = {{"id", i.id}, {"kind", i.kind}, {"x", i.x}, {"y", i.y}, {"w", i.w}, {"h", i.h}, {"text", i.text},
                      {"ink", std::round(InkWidth(i))}};
            if (!i.sample.empty()) j["sample"] = i.sample;
            items.push_back(j);
        }
        return json{{"items", items}, {"cut", page.cut}};
    };
    auto pagesJson = [&](const Settings &s) {
        json out = json::array();
        const std::vector<Part> parts = Parts(s);
        for (size_t i = 0; i < parts.size(); i++) {
            Page page;
            LayoutPage(s, parts, i, &page);
            json j = pageJson(page);
            j["part"] = parts[i].label;
            out.push_back(j);
        }
        return out;
    };
    if (method == "panel.layout") {   // the pages as they would be made: a manifest's given, or every loaded mod's
        json mods = json::array();
        std::vector<Settings> all;
        if (p.contains("settings")) {
            const json defaults = p.value("defaults", json::object());
            Settings s;
            std::string why;
            const std::string name = p.value("name", std::string("mod"));
            auto defaultOf = [&defaults](const std::string &key) {
                // a key of a section ("routes.show") is the defaults' nested object's, as the mods' config.json has it
                const json *at = &defaults;
                for (size_t from = 0; at && at->is_object();) {
                    size_t dot = key.find('.', from);
                    std::string part = key.substr(from, dot == std::string::npos ? std::string::npos : dot - from);
                    at = at->contains(part) ? &(*at)[part] : nullptr;
                    if (dot == std::string::npos) break;
                    from = dot + 1;
                }
                return at && !at->is_object() ? at->dump(-1, ' ', false, json::error_handler_t::replace) : std::string();
            };
            if (!ParseSettingsJson(p["settings"], name, name, defaultOf, &s, &why)) {
                *reply = "panel.layout: " + (why.empty() ? std::string("no controls") : why);
                return true;
            }
            all.push_back(std::move(s));
            if (!why.empty()) { *reply = "panel.layout: " + why; return true; }   // (a control the page would leave out)
        } else {
            all = AllSettings();
        }
        for (const Settings &s : all) mods.push_back({{"mod", s.mod}, {"title", s.title}, {"pages", pagesJson(s)}});
        Page list;
        LayoutList(all, &list);
        *reply = json{{"mods", mods}, {"list", pageJson(list)}}.dump(-1, ' ', false, json::error_handler_t::replace);
        *ok = true;
        return true;
    }
    if (method == "panel.open") {
        std::string mod = p.value("mod", ""), err;
        size_t part = 0;
        if (!mod.empty() && p.contains("part")) {   // its number (0..), or its name as its tab shows it
            Settings s;
            std::vector<Part> parts;
            if (Find(mod, &s)) parts = Parts(s);
            const json &q = p["part"];
            if (q.is_number_unsigned() || q.is_number_integer()) {
                part = static_cast<size_t>(std::max<long long>(0, q.get<long long>()));
            } else if (q.is_string()) {
                part = parts.size();
                for (size_t i = 0; i < parts.size(); i++) if (parts[i].label == q.get<std::string>()) part = i;
                if (part == parts.size()) { *reply = "panel.open: no part \"" + q.get<std::string>() + "\" (panel.state lists them)"; return true; }
            }
        }
        g_reopen = -1;
        bool done = mod.empty() ? OpenList(&err) : OpenModPage(mod, part, &err);
        if (!done) { *reply = "panel.open: " + err; return true; }
        *reply = json{{"opened", mod.empty() ? "mods" : mod}, {"part", part}}.dump(-1, ' ', false, json::error_handler_t::replace);
        *ok = true;
        return true;
    }
    auto controlJson = [](const Control &k, MmlMod *mod) {
        std::string v = ConfigValueJson(mod, k.key.c_str());
        json c = {{"key", k.key}, {"label", k.label}, {"kind", k.kind == kCheck ? "check" : k.kind == kChoice ? "choice" : k.kind == kKey ? "key" : "slider"},
                  {"value", v.empty() ? json() : json::parse(v, nullptr, false)}, {"shown", Shown(k, mod)}};
        if (k.kind == kSlider) { c["min"] = k.min; c["max"] = k.max; if (k.step > 0) c["step"] = k.step; }
        return c;
    };
    json mods = json::array();
    for (const Settings &s : AllSettings()) {
        MmlMod *mod = ModById(s.mod);
        json controls = json::array(), sections = json::array(), parts = json::array();
        for (const Section &sec : s.sections) {
            json mine = json::array();
            for (const Control &k : sec.controls) {
                controls.push_back(controlJson(k, mod));
                mine.push_back(k.key);
            }
            sections.push_back({{"title", sec.title}, {"keys", mine}});
        }
        for (const Part &part : Parts(s)) {
            json keys = json::array();
            for (size_t i = 0; i < part.count; i++) keys.push_back(PartControl(s, part, i)->key);
            parts.push_back({{"label", part.label}, {"section", part.section}, {"keys", keys}});
        }
        mods.push_back({{"mod", s.mod}, {"title", s.title}, {"controls", controls}, {"sections", sections}, {"parts", parts}});
    }
    *reply = json{{"item", g_item}, {"mods", mods}, {"page", g_pageMod}, {"part", g_pagePart}}.dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
