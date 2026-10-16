// The scene's lights (mml.h scene_lights, light_set, scene_sun; docs/MODDING.md "The scene's lights").
//
// The level's light frames: LS3D frames of type 2, I3D_light, found under the scene's top frame (the player's
// model's topmost parent, as mafia-ragdoll's scene_light.cpp finds them) and kept for the mission (scanned again
// only while none was found, or when asked: lights.list {"rescan": true}). In the labs a change of the sun and the
// ambient light changed the whole picture, the buildings and the sky with the people and cars. Their fields as LS3D's
// setters write them (game_build.h lightVtable): +0x154 the type, +0x158 the power, +0x15C the colour, +0x168/+0x16C
// the range, +0x170/+0x174 the cone, +0x140/+0x148 the sectors lit. A change goes through the setters (the vtable
// checked first), so LS3D works out what depends on it.
// The sun: the brightest directional light that is on; the ambient light: the brightest ambient one that lights a
// sector the sun lights (the backdrop's own white one lights only the backdrop), else the brightest.
//
// Bridge methods (game thread): lights.list {type, all, rescan}, light.set {light, color, power, range, on}.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr uint32_t kFrameLight = 2;
constexpr int kLightSectors = 0x140, kLightSectorCount = 0x148, kLightType = 0x154, kLightPower = 0x158, kLightColor = 0x15C,
              kLightRange = 0x168, kLightCone = 0x170;
constexpr int kVtSetOn = 0x24, kVtSetColor = 0x58, kVtSetPower = 0x5C, kVtSetRange = 0x64;

using SetOnFn = void(__stdcall *)(void *light, bool on);
using SetColorFn = void(__stdcall *)(void *light, const V3 *color);
using SetPowerFn = void(__stdcall *)(void *light, float power);
using SetRangeFn = void(__stdcall *)(void *light, float nearR, float farR);

std::vector<void *> g_lights;
DWORD g_scanned;
bool g_haveScan;

template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

uintptr_t Ls3dBase() { return ModuleBase("LS3DF.dll"); }
bool IsLight(void *f) {
    uintptr_t vt = game::g_ls3d && game::Ls3d().lightVtable ? Ls3dBase() + game::Ls3d().lightVtable : 0;
    return vt && f && game::at<uintptr_t>(f, 0) == vt && game::at<uint32_t>(f, game::kFrameType) == kFrameLight;
}

void *SceneTop() {
    void *pl = game::Player();
    void *top = pl ? game::at<void *>(pl, game::kActorFrame) : nullptr;
    for (int i = 0; top && game::at<void *>(top, game::kFrameParent) && i < 64; i++) top = game::at<void *>(top, game::kFrameParent);
    return top;
}

void Scan() {
    g_lights.clear();
    void *top = SceneTop();
    if (!top) return;
    std::vector<void *> all;
    game::EnumFramesDeep(top, &all);
    for (void *f : all) if (IsLight(f) && std::find(g_lights.begin(), g_lights.end(), f) == g_lights.end()) g_lights.push_back(f);
}

// The lights: scanned once a mission (the walk over every frame of the scene takes 13 ms in the 1.2 lab's freeride:
// a hitch if it were repeated), again only while none was found (every five seconds) or when asked.
void Fresh(bool again = false) {
    if (!game::g_exe || !game::Mission()) { g_lights.clear(); return; }
    if (!again && g_haveScan && (!g_lights.empty() || GetTickCount() - g_scanned < 5000)) return;
    ProfScope ps(nullptr, "scene lights scan");
    auto f = [&]() { Scan(); };
    if (Guarded(f)) g_lights.clear();
    g_scanned = GetTickCount();
    g_haveScan = true;
}

bool OnChain(void *f) {
    for (int i = 0; f && i < 64; i++, f = game::at<void *>(f, game::kFrameParent))
        if (!(game::at<uint32_t>(f, game::kFrameFlags) & 1u)) return false;
    return true;
}

void Read(void *f, MmlLight *l) {
    std::memset(l, 0, sizeof *l);
    l->frame = f;
    if (const char *n = game::at<const char *>(f, game::kFrameName)) strncpy_s(l->name, n, _TRUNCATE);
    l->type = static_cast<int>(game::at<uint32_t>(f, kLightType));
    l->on = OnChain(f) ? 1 : 0;
    for (int i = 0; i < 3; i++) l->color[i] = game::at<float>(f, kLightColor + 4 * i);
    l->power = game::at<float>(f, kLightPower);
    l->range[0] = game::at<float>(f, kLightRange);
    l->range[1] = game::at<float>(f, kLightRange + 4);
    l->cone[0] = game::at<float>(f, kLightCone);
    l->cone[1] = game::at<float>(f, kLightCone + 4);
    const float *w = game::WorldMatrix(f);
    if (w) for (int i = 0; i < 3; i++) { l->pos[i] = w[12 + i]; l->dir[i] = w[8 + i]; }
    uint32_t n = game::at<uint32_t>(f, kLightSectorCount);
    l->sectors = n > 4096 ? 0 : static_cast<int>(n);
}

std::vector<void *> SectorsOf(void *f) {
    std::vector<void *> out;
    void **s = game::at<void **>(f, kLightSectors);
    uint32_t n = game::at<uint32_t>(f, kLightSectorCount);
    if (!s || n > 4096) return out;
    for (uint32_t i = 0; i < n; i++) if (s[i]) out.push_back(s[i]);
    return out;
}
float Lum(const MmlLight &l) { return (0.3f * l.color[0] + 0.59f * l.color[1] + 0.11f * l.color[2]) * l.power; }

struct Sun { bool have; MmlLight sun, ambient; bool haveAmbient; };
Sun FindSun() {
    Sun s{};
    std::vector<MmlLight> all(g_lights.size());
    for (size_t i = 0; i < g_lights.size(); i++) Read(g_lights[i], &all[i]);
    float best = 0;
    for (const MmlLight &l : all)
        if (l.type == MML_LIGHT_DIRECTIONAL && l.on && Lum(l) > best) { best = Lum(l); s.sun = l; s.have = true; }
    std::vector<void *> lit = s.have ? SectorsOf(s.sun.frame) : std::vector<void *>();
    float bestShared = 0, bestAny = 0;
    MmlLight shared{}, any{};
    for (const MmlLight &l : all) {
        if (l.type != MML_LIGHT_AMBIENT || !l.on) continue;
        if (Lum(l) > bestAny) { bestAny = Lum(l); any = l; }
        bool share = false;
        for (void *sec : SectorsOf(l.frame)) if (std::find(lit.begin(), lit.end(), sec) != lit.end()) { share = true; break; }
        if (share && Lum(l) > bestShared) { bestShared = Lum(l); shared = l; }
    }
    if (bestShared > 0) { s.ambient = shared; s.haveAmbient = true; }
    else if (bestAny > 0) { s.ambient = any; s.haveAmbient = true; }
    return s;
}

std::string HexOf(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}
json R3(const float *v) { return json::array({std::round(v[0] * 1000) / 1000, std::round(v[1] * 1000) / 1000, std::round(v[2] * 1000) / 1000}); }
const char *TypeName(int t) {
    switch (t) {
    case 1: return "point";
    case 2: return "spot";
    case 3: return "directional";
    case 4: return "ambient";
    case 5: return "fog";
    }
    return "other";
}
json LightJson(const MmlLight &l) {
    return {{"light", HexOf(l.frame)}, {"name", l.name}, {"type", TypeName(l.type)}, {"type_id", l.type}, {"on", l.on != 0},
            {"color", R3(l.color)}, {"power", l.power}, {"range", {l.range[0], l.range[1]}}, {"cone", {l.cone[0], l.cone[1]}},
            {"pos", R3(l.pos)}, {"dir", R3(l.dir)}, {"sectors", l.sectors}};
}

} // namespace

void LightsMissionClosed() {
    g_lights.clear();
    g_haveScan = false;
}

int SceneLights(MmlLight *out, int max) {
    Fresh();
    int n = 0;
    auto f = [&]() {
        for (void *l : g_lights) {
            if (out && n < max) Read(l, &out[n]);
            n++;
        }
    };
    if (Guarded(f)) { g_haveScan = false; return 0; }
    return n;
}

bool LightSet(void *light, const float *color, float power, float rangeNear, float rangeFar, int on, std::string *err) {
    Fresh();
    if (std::find(g_lights.begin(), g_lights.end(), light) == g_lights.end()) { *err = "not one of the scene's lights (lights.list)"; return false; }
    bool ok = true;
    auto f = [&]() {
        if (!IsLight(light)) { ok = false; return; }
        void **vt = *reinterpret_cast<void ***>(light);
        if (color) { V3 c{color[0], color[1], color[2]}; reinterpret_cast<SetColorFn>(vt[kVtSetColor / 4])(light, &c); }
        if (!std::isnan(power)) reinterpret_cast<SetPowerFn>(vt[kVtSetPower / 4])(light, power);
        if (!std::isnan(rangeNear) || !std::isnan(rangeFar)) {
            float n = std::isnan(rangeNear) ? game::at<float>(light, kLightRange) : rangeNear;
            float fa = std::isnan(rangeFar) ? game::at<float>(light, kLightRange + 4) : rangeFar;
            reinterpret_cast<SetRangeFn>(vt[kVtSetRange / 4])(light, n, fa);
        }
        if (on >= 0) reinterpret_cast<SetOnFn>(vt[kVtSetOn / 4])(light, on != 0);
    };
    if (Guarded(f)) { *err = "LS3D's setter faulted"; return false; }
    if (!ok) { *err = "its vtable is not I3D_light's any more (the mission changed?)"; g_haveScan = false; return false; }
    return true;
}

int SceneSun(float toSun[3], float sunColor[3], float ambient[3]) {
    Fresh();
    Sun s{};
    auto f = [&]() { s = FindSun(); };
    if (Guarded(f) || !s.have) return 0;
    for (int i = 0; i < 3; i++) {
        if (toSun) toSun[i] = -s.sun.dir[i];
        if (sunColor) sunColor[i] = s.sun.color[i] * s.sun.power;
        if (ambient) ambient[i] = s.haveAmbient ? s.ambient.color[i] * s.ambient.power : 0.0f;
    }
    if (toSun) {
        float l = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
        if (l > 1e-6f) for (int i = 0; i < 3; i++) toSun[i] /= l;
    }
    return 1;
}

bool LightsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "lights.list" && method != "light.set") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string err;
    try {
        if (method == "light.set") {
            std::string a = p.value("light", "");
            void *light = reinterpret_cast<void *>(ParseAddress(a));
            if (!light) { *reply = "light.set: light (an address from lights.list)"; return true; }
            float c[3];
            bool haveColor = p.contains("color") && p["color"].is_array() && p["color"].size() == 3;
            if (haveColor) for (int i = 0; i < 3; i++) c[i] = p["color"][i].get<float>();
            float rn = NAN, rf = NAN;
            if (p.contains("range") && p["range"].is_array() && p["range"].size() == 2) { rn = p["range"][0].get<float>(); rf = p["range"][1].get<float>(); }
            int on = p.contains("on") ? (p["on"].get<bool>() ? 1 : 0) : -1;
            if (!LightSet(light, haveColor ? c : nullptr, p.contains("power") ? p["power"].get<float>() : NAN, rn, rf, on, &err)) {
                *reply = "light.set: " + err;
                return true;
            }
            MmlLight l;
            auto f = [&]() { Read(light, &l); };
            Guarded(f);
            *reply = dump(LightJson(l));
        } else {
            if (p.value("rescan", false)) Fresh(true);
            std::vector<MmlLight> all(4096);
            int n = std::min(SceneLights(all.data(), 4096), 4096);
            std::string type = p.value("type", "");
            bool every = p.value("all", false);
            json list = json::array(), kinds = json::object();
            int max = std::max(1, p.value("max", 100));
            for (int i = 0; i < n; i++) {
                std::string t = TypeName(all[i].type);
                kinds[t] = kinds.value(t, 0) + 1;
                if (!type.empty() && t != type) continue;
                if (!every && !all[i].on) continue;
                if (static_cast<int>(list.size()) < max) list.push_back(LightJson(all[i]));
            }
            json out = {{"count", n}, {"kinds", kinds}, {"lights", list}};
            float s[3], sc[3], am[3];
            if (SceneSun(s, sc, am)) out["sun"] = {{"to_sun", R3(s)}, {"color", R3(sc)}, {"ambient", R3(am)}};
            *reply = dump(out);
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
