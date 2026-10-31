// Weather policy only: no game addresses, hooks, private headers, shaders, GPU resources or DLSS handling.
// The host owns arbitration, native fog restoration and composition. See docs/ATMOSPHERE_SDK.md.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mml/atmosphere.h"

namespace {
const MmlHost *H;
MmlMod *M;
bool on, published;
int tint;
MmlAtmosphereSettings settings, last;

float Number(const char *key, float def, float lo, float hi) {
    const float value = static_cast<float>(H->config_number(M,key,def));
    return std::isfinite(value) ? std::clamp(value,lo,hi) : def;
}
void ReadSettings() {
    on = H->config_number(M,"on",1) != 0;
    settings = mml_atmosphere_defaults();
    settings.priority = static_cast<int>(Number("priority",0,-100,100));
    settings.clear_distance = Number("start",100,0,10000);
    settings.extinction = 3 * Number("density",1,0,3) / Number("distance",900,50,10000);
    settings.base_height = Number("base",-10,-500,1000);
    settings.scale_height = Number("height",60,0,500);
    settings.sun_scattering = Number("sun",0.6f,0,3);
    settings.sky_distance = Number("sky_distance",8000,1000,20000);
    settings.max_opacity = Number("max_opacity",0.995f,0,1);
    settings.sky_blend = Number("sky_blend",1,0,1);
    settings.anisotropy = Number("anisotropy",0.55f,-0.9f,0.9f);
    settings.sky_falloff = Number("sky_falloff",8,0,20);
    tint = static_cast<int>(Number("tint",0,0,3));
}
void Publish() {
    if (!on) { H->scene_atmosphere(M,nullptr); published=false; return; }
    float sun[3]{}, color[3]{}, ambient[3]{0.5f,0.5f,0.5f};
    H->scene_sun(M,sun,color,ambient);
    const float peak = std::max({ambient[0],ambient[1],ambient[2],0.001f});
    const float brightness = std::clamp(peak * 1.15f,0.02f,0.9f);
    constexpr float tints[3][3] = {{0.76f,0.84f,1},{1,0.87f,0.72f},{1,1,1}};
    for (int i=0;i<3;i++) settings.color[i] = brightness *
        (tint == 0 ? std::pow(std::max(ambient[i],0.0f)/peak,0.25f) : tints[tint-1][i]);
    if (!published || std::memcmp(&settings,&last,sizeof settings)) {
        if (H->scene_atmosphere(M,&settings)) { published=true; last=settings; }
    }
}
void Event(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) { ReadSettings(); Publish(); }
    else if (ev->type == MML_EVENT_RENDER_BEGIN && ev->world) Publish();
}
int Status(const char *, MmlReply *r, void *) {
    MmlAtmosphereState s{}; s.size=sizeof s;
    H->scene_atmosphere_state(M,&s);
    char text[256];
    snprintf(text,sizeof text,"{\"on\":%s,\"requested\":%s,\"selected\":%s,\"ready\":%s,\"owners\":%d}",
        on?"true":"false",s.requested?"true":"false",s.selected?"true":"false",s.ready?"true":"false",s.request_count);
    H->reply_append(r,text,static_cast<size_t>(-1)); return 0;
}
}
MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H=host; M=mod;
    if (!MML_HAS(H,scene_atmosphere_state)) return 1;
    ReadSettings(); Publish();
    H->subscribe(M,MML_EVENT_CONFIG_CHANGED,Event,nullptr);
    H->subscribe(M,MML_EVENT_RENDER_BEGIN,Event,nullptr);
    H->command(M,"status","on/requested/selected/ready: the shared SDK atmosphere request",Status,nullptr);
    return 0;
}
MML_EXPORT void mml_mod_unload(MmlMod *) { H->scene_atmosphere(M,nullptr); }
