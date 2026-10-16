// engine_note: sound (sound_play, sound_set, sound_stop, music_play, music_stop, music_game; docs/MODDING.md "Sound").
//
// A second engine voice rides on the player's car: a looped 3D sound attached to its model (it goes where the car
// goes), its pitch following the engine's revolutions from idle to the top and its volume the engine's state (off:
// silent). The hotkey (F9) turns it on and off. `engine_note.music {"file": "13 - Game Over.ogg"}` plays a track in
// place of the city's music (the game's own faded out while it plays); `engine_note.music {}` stops it and gives
// the city's music back.
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

namespace {
const MmlHost *H;
MmlMod *M;
bool g_game;             // a build of the game whose addresses are known (mml/game/game_build.h)
bool g_on = true;
int g_voice;             // the sound's id (0: none)
void *g_car;             // the car it rides on
int g_music;
float g_volume = 0.8f, g_idle = 0.6f, g_top = 1.8f;
std::string g_sound = "th_1.wav";

void Silence() {
    if (g_voice) H->sound_stop(M, g_voice);
    g_voice = 0;
    g_car = nullptr;
}

void OnKey(void *) {
    g_on = !g_on;
    if (!g_on) Silence();
}

void ReadSettings() {
    std::string sound = H->config_string(M, "sound", "th_1.wav");
    if (sound != g_sound) { g_sound = sound; Silence(); }   // (made again with the new one)
    g_volume = static_cast<float>(H->config_number(M, "volume", 0.8));
    g_idle = static_cast<float>(H->config_number(M, "pitch_idle", 0.6));
    g_top = static_cast<float>(H->config_number(M, "pitch_top", 1.8));
    H->hotkey(M, H->config_string(M, "hotkey", "F9"), OnKey, nullptr);
}

// Every tick of the mission: the voice follows the car's engine.
void OnTick(const MmlEvent *, void *) {
    if (!g_game || !g_on) return;
    void *carActor = game::PlayerCar();
    if (carActor != g_car) Silence();                    // (out of the car, or another car)
    if (!carActor) return;
    game::Car car(carActor);
    if (!g_voice) {
        g_voice = H->sound_play(M, g_sound.c_str(), car.Frame(), nullptr, 0, g_idle, 4, 60, MML_SOUND_LOOP);
        g_car = carActor;
        if (!g_voice) { g_on = false; return; }           // (the log says why; F9 tries again)
    }
    float top = car.v<float>(game::Exe().vehRpsMax) * 60.0f;
    float share = top > 1 ? std::fmin(std::fmax(car.Rpm() / top, 0.0f), 1.0f) : 0.0f;
    H->sound_set(M, g_voice, car.EngineOn() ? g_volume : 0.0f, g_idle + (g_top - g_idle) * share);
}

void OnEvent(const MmlEvent *ev, void *) {
    if (ev->type == MML_EVENT_CONFIG_CHANGED) ReadSettings();
    if (ev->type == MML_EVENT_MISSION_CLOSE) { g_voice = 0; g_car = nullptr; }   // (the host released the 3D sounds)
}

int Status(const char *, MmlReply *reply, void *) {
    MmlSoundState st{};
    st.size = sizeof st;
    int known = g_voice && H->sound_state(M, g_voice, &st);
    char out[256];
    snprintf(out, sizeof out, "{\"on\":%s,\"voice\":%d,\"playing\":%s,\"pitch\":%.3f,\"volume\":%.3f,\"music\":%d}", g_on ? "true" : "false",
             known ? g_voice : 0, known && st.playing ? "true" : "false", st.pitch, st.volume, g_music);
    H->reply_append(reply, out, static_cast<size_t>(-1));
    return 0;
}

// engine_note.music {"file": "..."}: a track instead of the city's music; {} stops it.
int Music(const char *args, MmlReply *reply, void *u) {
    char file[260] = "";
    if (const char *p = args ? strstr(args, "\"file\"") : nullptr) {
        p = strchr(p + 6, '"');
        const char *e = p ? strchr(p + 1, '"') : nullptr;
        if (p && e && e - p - 1 < static_cast<int>(sizeof file)) { memcpy(file, p + 1, e - p - 1); file[e - p - 1] = 0; }
    }
    if (g_music) H->music_stop(M, g_music);
    g_music = 0;
    if (*file) {
        g_music = H->music_play(M, file, 1.0f, MML_SOUND_LOOP);
        if (!g_music) { H->reply_error(reply, "no such music (the log says why)"); return 1; }
        H->music_game(M, 0);
    } else {
        H->music_game(M, 1);
    }
    return Status(args, reply, u);
}
} // namespace

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, music_game)) return 1;                          // a loader without sound
    g_game = game::Detect(host->module_base("LS3DF.dll"));
    ReadSettings();
    host->subscribe(mod, MML_EVENT_MISSION_TICK, OnTick, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_CLOSE, OnEvent, nullptr);
    host->command(mod, "status", "the voice: on, playing, its pitch and volume now", Status, nullptr);
    host->command(mod, "music", "{\"file\"}: a track in place of the city's music; {}: stopped, the city's back", Music, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}   // (the host stops a mod's sounds and music and gives the city's music back)
