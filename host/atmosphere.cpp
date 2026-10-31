// Shared, owner-scoped atmosphere service. Weather policy belongs to mods; this file copies/validates requests,
// arbitrates ownership and snapshots explicit parameters for the existing current-frame graphics composition.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "mml/atmosphere.h"
#include "json.hpp"
using json = nlohmann::json;

namespace host {
namespace {
struct Request { MmlMod *owner; MmlAtmosphereSettings settings; uint64_t acquired; };
std::vector<Request> g_requests;
uint64_t g_sequence;
MmlAtmosphereSettings g_frame{};
bool g_ready;

const Request *Winner() {
    const Request *best = nullptr;
    for (const auto &r : g_requests)
        if (!best || r.settings.priority > best->settings.priority ||
            (r.settings.priority == best->settings.priority && r.acquired > best->acquired)) best = &r;
    return best;
}
const char *Name(MmlMod *owner) { return owner ? ModIdOf(owner) : "bridge"; }
bool Between(float v, float lo, float hi) { return std::isfinite(v) && v >= lo && v <= hi; }
bool Valid(const MmlAtmosphereSettings &s) {
    return s.size >= sizeof s && s.priority >= -1000000 && s.priority <= 1000000 &&
        Between(s.clear_distance,0,100000) && Between(s.extinction,0,10) &&
        Between(s.base_height,-100000,100000) && Between(s.scale_height,0,100000) &&
        (s.scale_height == 0 || s.scale_height >= 0.001f) &&
        Between(s.color[0],0,1) && Between(s.color[1],0,1) && Between(s.color[2],0,1) &&
        Between(s.sun_scattering,0,100) && Between(s.sky_distance,0.01f,1000000) &&
        Between(s.max_opacity,0,1) && Between(s.sky_blend,0,1) &&
        Between(s.anisotropy,-0.95f,0.95f) && Between(s.sky_falloff,0,1000);
}
json Settings(const MmlAtmosphereSettings &s) {
    return {{"priority",s.priority},{"clear_distance",s.clear_distance},{"extinction",s.extinction},
        {"base_height",s.base_height},{"scale_height",s.scale_height},{"color",{s.color[0],s.color[1],s.color[2]}},
        {"sun_scattering",s.sun_scattering},{"sky_distance",s.sky_distance},{"max_opacity",s.max_opacity},
        {"sky_blend",s.sky_blend},{"anisotropy",s.anisotropy},{"sky_falloff",s.sky_falloff}};
}
json State() {
    const Request *winner = Winner();
    json requests = json::array();
    for (const auto &r : g_requests) requests.push_back({{"owner",Name(r.owner)},{"acquired",r.acquired},
        {"selected",&r == winner},{"settings",Settings(r.settings)}});
    return {{"owner",winner ? json(Name(winner->owner)) : json(nullptr)}, {"ready",g_ready},
        {"request_count",g_requests.size()},{"settings",winner ? Settings(winner->settings) : json(nullptr)},
        {"requests",requests}};
}
} // namespace

bool AtmosphereSet(MmlMod *owner, const MmlAtmosphereSettings *settings, std::string *err) {
    if (GetCurrentThreadId() != MainThread()) { *err = "scene_atmosphere requires the game thread"; return false; }
    if (!settings) { AtmosphereModGone(owner); return true; }
    if (settings->size < sizeof *settings || !Valid(*settings)) {
        *err = "invalid atmosphere settings (size, finite physical parameters, color/opacity ranges)"; return false;
    }
    for (auto &r : g_requests) if (r.owner == owner) { r.settings = *settings; r.settings.size = sizeof *settings; return true; }
    g_requests.push_back({owner,*settings,++g_sequence});
    g_requests.back().settings.size = sizeof *settings;
    return true;
}
void AtmosphereModGone(MmlMod *owner) {
    g_requests.erase(std::remove_if(g_requests.begin(),g_requests.end(),[&](const Request &r){return r.owner == owner;}),g_requests.end());
}
bool AtmosphereState(MmlMod *owner, MmlAtmosphereState *out) {
    if (!out || out->size < sizeof *out || GetCurrentThreadId() != MainThread()) return false;
    const Request *winner = Winner();
    MmlAtmosphereState value{}; value.size = sizeof value;
    value.ready = g_ready; value.request_count = static_cast<int>(g_requests.size());
    for (const auto &r : g_requests) if (r.owner == owner) value.requested = 1;
    if (winner) { value.selected = winner->owner == owner; value.settings = winner->settings; strncpy_s(value.owner,Name(winner->owner),_TRUNCATE); }
    *out = value;
    return true;
}
bool AtmosphereActive() { return !g_requests.empty(); }
bool AtmosphereReady() { return g_ready; }
void AtmosphereSceneClosed() { g_ready = false; g_frame = {}; }
void AtmosphereFrameBegin() {
    std::string why;
    const Request *winner = Winner();
    g_ready = winner && GfxStateNow(&why) == 1;
    g_frame = winner ? winner->settings : MmlAtmosphereSettings{};
}
void AtmosphereConstants(MmlGfxConstants *k) {
    const auto &s = g_frame;
    k->atmosphere[0] = s.clear_distance; k->atmosphere[1] = s.extinction;
    k->atmosphere[2] = s.base_height; k->atmosphere[3] = s.scale_height > 0 ? 1/s.scale_height : 0;
    for (int i=0;i<3;i++) k->atmosphere_color[i] = s.color[i];
    k->atmosphere_color[3] = s.sun_scattering;
    k->atmosphere_info[0] = g_ready ? 1.0f : 0;
    k->atmosphere_info[1] = s.sky_distance; k->atmosphere_info[2] = s.max_opacity; k->atmosphere_info[3] = s.sky_blend;
    k->atmosphere_phase[0] = s.anisotropy; k->atmosphere_phase[1] = s.sky_falloff;
}
bool AtmosphereMethod(const std::string &method, const std::string &params, std::string *reply, bool *ok) {
    if (method != "atmosphere.state" && method != "atmosphere.set") return false;
    if (method == "atmosphere.set") {
        const json p = json::parse(params.empty() ? "{}" : params,nullptr,false);
        if (!p.is_object()) { *ok=false; *reply="atmosphere.set expects an object"; return true; }
        auto s = mml_atmosphere_defaults();
        for (const auto &r : g_requests) if (!r.owner) { s=r.settings; break; }
        s.priority=p.value("priority",s.priority);
        s.clear_distance=p.value("clear_distance",s.clear_distance); s.extinction=p.value("extinction",s.extinction);
        s.base_height=p.value("base_height",s.base_height); s.scale_height=p.value("scale_height",s.scale_height);
        s.sun_scattering=p.value("sun_scattering",s.sun_scattering); s.sky_distance=p.value("sky_distance",s.sky_distance);
        s.max_opacity=p.value("max_opacity",s.max_opacity); s.sky_blend=p.value("sky_blend",s.sky_blend);
        s.anisotropy=p.value("anisotropy",s.anisotropy); s.sky_falloff=p.value("sky_falloff",s.sky_falloff);
        if (p.contains("color")) {
            if (!p["color"].is_array() || p["color"].size()!=3) { *ok=false; *reply="color requires three RGB values"; return true; }
            for(int i=0;i<3;i++) s.color[i]=p["color"][i].get<float>();
        }
        std::string err;
        if (!AtmosphereSet(nullptr,p.value("on",true)?&s:nullptr,&err)) { *ok=false; *reply=err; return true; }
    }
    *reply=State().dump(-1,' ',false,json::error_handler_t::replace); *ok=true; return true;
}
} // namespace host
