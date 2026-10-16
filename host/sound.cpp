// Sound and music (mml.h sound_play, sound_set, sound_move, sound_stop, sound_state, music_play, music_set,
// music_stop, music_game; docs/MODDING.md "Sound").
//
// A sound is LS3D's I3D_sound, a frame of type 4 made by the I3D driver, its file opened by its own Open (the menus
// open "sounds\\menuopen.wav" so; the game's archives and a mod's files\ answer it through dtaOpen). In 2D it is an
// ambient sound in no scene, as the menus' own sounds are (heard the same everywhere, in a menu too). In 3D it is a
// point sound placed in the mission's world and linked into the room (sector) at its place, as MafiaOnline's sounds
// and the game's Play3DSound place theirs; one that follows a frame (a bone, a car, a prop) is moved to the frame's
// place every frame and linked again when it has gone 10 m (a sound must stay in the scene's sectors, not under
// the frame, for LS3D to hear it). Its slots (game_build.h soundVtable): SetOn plays it, SetOn2(false, true) stops
// it (the scripts' STOPSOUND), SetVolume, SetFrequency (the pitch), SetLoop, SetRange (inner, outer, 0.4, 0.3: the
// curve and the volume at outer the game's Play3DSound gives), SetCone (all round).
// A sound that is not looped is released once its length (divided by the pitch) has gone by. The host keeps every
// sound: a mod's go with its unload, the 3D ones with the mission (before its scene goes). LS3D holds the world's
// (3D) sounds itself while the game is paused (seen: their time stood in the pause menu and went on after); a 2D
// one plays on unless MML_SOUND_PAUSABLE, which holds it too (stopped, then started again where it was).
//
// Music is LS3D's IShow (IGraph slot 0 makes one), what C_game streams the city's music and the films' with: Open, Play,
// SetVolume (times the options' music volume, game_build.h musicVolume, as the game does), Stop, Close, Release. Its
// end is told by GetEvent (1: stopped); a looped one is then played again from its start, another released. music_game pauses the game's
// own city music (C_game::CityMusicPaused).
//
// Bridge methods (game thread): sounds.list, sound.play, sound.set, sound.stop, music.play, music.set, music.stop,
// music.game.
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

enum { kFrameSound = 4 };
enum { kSndPoint = 1, kSndAmbient = 3 };
// Open's flag 8: the ISND sound made with ISND_FREQUENCY (0x10), without which SetFrequency does nothing (the cars'
// engine sounds are opened so: C_Vehicle::SoundInit's flags & 8)
constexpr uint32_t kOpenPitch = 8;
// I3D_frame's and I3D_sound's slots (game_build.h soundVtable)
enum {
    kVtRelease = 0x00, kVtUpdate = 0x18, kVtSetOn = 0x24, kVtSetName = 0x28, kVtLinkTo = 0x2C, kVtOpen = 0x50, kVtIsPlaying = 0x54,
    kVtSetType = 0x58, kVtSetRange = 0x5C, kVtSetCone = 0x60, kVtSetOutVol = 0x64, kVtSetVolume = 0x68, kVtSetLoop = 0x6C,
    kVtSetCurrTime = 0x70, kVtSetFreq = 0x74, kVtGetCurrTime = 0x80, kVtGetPlayTime = 0x84, kVtSetOn2 = 0x88
};
// IShow's slots
enum { kShRelease = 0x04, kShOpen = 0x0C, kShClose = 0x10, kShPlay = 0x14, kShStop = 0x18, kShGetEvent = 0x2C, kShSetPos = 0x30, kShGetPos = 0x34, kShSetVolume = 0x38 };

using CreateFrameFn = void *(__stdcall *)(void *driver, int type);
using ReleaseFn = int(__stdcall *)(void *obj);
using VoidFn = void(__stdcall *)(void *obj);
using SetOnFn = void(__stdcall *)(void *frame, bool on);
using SetOn2Fn = void(__stdcall *)(void *frame, bool on, bool now);
using SetNameFn = int(__stdcall *)(void *frame, const char *name);
using LinkToFn = int(__stdcall *)(void *frame, void *parent, uint32_t flags);
using OpenFn = int(__stdcall *)(void *sound, const char *file, uint32_t flags, void *cb, void *ctx);
using IsPlayingFn = bool(__stdcall *)(void *sound, bool);
using IntFn = void(__stdcall *)(void *sound, int value);
using BoolFn = void(__stdcall *)(void *sound, bool value);
using FloatFn = void(__stdcall *)(void *sound, float value);
using Float2Fn = void(__stdcall *)(void *sound, float a, float b);
using Float4Fn = void(__stdcall *)(void *sound, float a, float b, float c, float d);
using GetIntFn = int(__stdcall *)(void *sound);
using SetPosFn = void(__fastcall *)(void *frame, void *edx, const V3 *pos);
using SceneMovedFn = int(__fastcall *)(void *scene, void *edx, void *frame);
using CreateShowFn = void *(__stdcall *)(void *igraph);
using ShowOpenFn = int(__stdcall *)(void *show, const char *file, uint32_t flags);
using ShowFn = int(__stdcall *)(void *show);
using ShowPosFn = int(__stdcall *)(void *show, uint32_t pos);
using ShowGetPosFn = int(__stdcall *)(void *show, uint32_t *pos);
using ShowVolFn = int(__stdcall *)(void *show, float volume);
using CityPausedFn = void(__fastcall *)(void *game, void *edx, bool paused);

struct Sound {
    int id;
    MmlMod *mod;
    void *frame;            // the I3D_sound
    std::string file;
    bool world, looped, pausable;
    void *follow;           // the frame it follows (null: none)
    V3 offset;              // in that frame's space (or the place in the world)
    V3 linkedAt;            // where it was last linked into a sector
    float volume, pitch;
    int length;             // the file's length (ms)
    DWORD started;          // when it started (a one-shot's end), moved on by its pauses
    bool held;              // stopped by a pause, to go on
    int heldAt;             // where it was then (ms)
    DWORD heldSince;
};
struct Music {
    int id;
    MmlMod *mod;
    void *show;
    std::string file;
    float volume;
    bool looped;
    uint32_t lastPos;
    int still;              // frames its position has not moved
};
std::vector<Sound> g_sounds;
std::vector<Music> g_music;
int g_nextId;
MmlMod *g_cityPausedBy;     // the mod that paused the city's music (null: none)
bool g_cityPausedByBridge;

const game::ExeBuild &E() { return game::Exe(); }
uintptr_t Ls3dBase() { return ModuleBase("LS3DF.dll"); }
void *Vt(void *obj, int off) { return (*reinterpret_cast<void ***>(obj))[off / 4]; }
bool SoundKnown() { return game::g_exe && game::g_ls3d && game::Ls3d().soundVtable && E().i3dDriver && E().frameSetPos && E().sceneMoved; }
bool MusicKnown() { return game::g_exe && E().igraph && E().musicVolume; }

template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

void *MissionScene() {
    void *m = game::Mission();
    return m ? game::at<void *>(m, game::kMissionScene) : nullptr;
}

Sound *FindSound(int id) {
    for (Sound &s : g_sounds) if (s.id == id) return &s;
    return nullptr;
}
Music *FindMusic(int id) {
    for (Music &m : g_music) if (m.id == id) return &m;
    return nullptr;
}

std::string SoundPath(const char *file) {
    std::string f = file ? file : "";
    std::replace(f.begin(), f.end(), '/', '\\');
    if (f.find('\\') == std::string::npos) f = "sounds\\" + f;
    if (f.find('.') == std::string::npos) f += ".wav";
    return f;
}

// Where a 3D sound is now: its place, or its frame's world matrix times its offset (row vectors).
bool WorldPlace(const Sound &s, V3 *out) {
    if (!s.follow) { *out = s.offset; return true; }
    const float *w = nullptr;
    auto g = [&] { w = game::WorldMatrix(s.follow); };
    if (Guarded(g) || !w) return false;
    *out = {w[12] + s.offset.x * w[0] + s.offset.y * w[4] + s.offset.z * w[8], w[13] + s.offset.x * w[1] + s.offset.y * w[5] + s.offset.z * w[9],
            w[14] + s.offset.x * w[2] + s.offset.y * w[6] + s.offset.z * w[10]};
    return std::isfinite(out->x) && std::isfinite(out->y) && std::isfinite(out->z);
}

void Place(Sound &s, const V3 &p, bool relink) {
    reinterpret_cast<SetPosFn>(E().frameSetPos)(s.frame, nullptr, &p);
    if (relink) {
        reinterpret_cast<SceneMovedFn>(E().sceneMoved)(MissionScene(), nullptr, s.frame);
        s.linkedAt = p;
    }
    reinterpret_cast<VoidFn>(Vt(s.frame, kVtUpdate))(s.frame);
}

void Unmake(Sound &s) {
    auto f = [&] {
        reinterpret_cast<SetOn2Fn>(Vt(s.frame, kVtSetOn2))(s.frame, false, true);
        if (s.world) reinterpret_cast<LinkToFn>(Vt(s.frame, kVtLinkTo))(s.frame, nullptr, 0);
        reinterpret_cast<ReleaseFn>(Vt(s.frame, kVtRelease))(s.frame);
    };
    if (DWORD code = Guarded(f)) HWARN("sound: releasing %s faulted (0x%08lX)", s.file.c_str(), code);
}

void UnmakeMusic(Music &m) {
    auto f = [&] {
        reinterpret_cast<ShowFn>(Vt(m.show, kShStop))(m.show);
        reinterpret_cast<ShowFn>(Vt(m.show, kShClose))(m.show);
        reinterpret_cast<ShowFn>(Vt(m.show, kShRelease))(m.show);
    };
    if (DWORD code = Guarded(f)) HWARN("music: releasing %s faulted (0x%08lX)", m.file.c_str(), code);
}

float MusicScale() {
    float v = 1;
    MemRead(E().musicVolume, &v, sizeof v);
    return std::isfinite(v) && v >= 0 ? v : 1.0f;
}

void ApplyMusicVolume(Music &m) {
    auto f = [&] { reinterpret_cast<ShowVolFn>(Vt(m.show, kShSetVolume))(m.show, m.volume * MusicScale()); };
    Guarded(f);
}

} // namespace

int SoundPlay(MmlMod *mod, const char *file, void *follow, const float *pos, float volume, float pitch, float rangeNear, float rangeFar, uint32_t flags,
              std::string *err) {
    if (!SoundKnown()) { *err = "sound is not known for this build"; return 0; }
    if (!file || !*file) { *err = "file: a sound of the game's (\"sounds\\\\menuopen.wav\") or a mod's files\\sounds\\"; return 0; }
    bool world = follow || pos;
    if (world && (!game::Mission() || !MissionScene())) { *err = "no mission loaded (a 3D sound is in the mission's world; a 2D one plays anywhere)"; return 0; }
    void *driver = *reinterpret_cast<void **>(E().i3dDriver);
    if (!driver) { *err = "no I3D driver"; return 0; }
    std::string path = SoundPath(file);
    void *frame = nullptr;
    int opened = -1;
    auto open = [&] {
        frame = reinterpret_cast<CreateFrameFn>(Vt(driver, E().drvCreateFrame))(driver, kFrameSound);
        if (frame && game::at<uintptr_t>(frame, 0) == Ls3dBase() + game::Ls3d().soundVtable)
            opened = reinterpret_cast<OpenFn>(Vt(frame, kVtOpen))(frame, path.c_str(), kOpenPitch, nullptr, nullptr);
    };
    if (DWORD code = Guarded(open)) { *err = "opening it faulted"; (void)code; return 0; }
    if (!frame) { *err = "the driver made no sound frame"; return 0; }
    if (opened != 0) {
        auto release = [&] { reinterpret_cast<ReleaseFn>(Vt(frame, kVtRelease))(frame); };
        Guarded(release);
        char b[16];
        snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(opened));
        *err = "no sound \"" + path + "\" (" + b + "; the game's sounds\\ and archives, or a mod's files\\sounds\\)";
        return 0;
    }
    Sound s{};
    s.id = ++g_nextId;
    s.mod = mod;
    s.frame = frame;
    s.file = path;
    s.world = world;
    s.looped = (flags & MML_SOUND_LOOP) != 0;
    s.pausable = (flags & MML_SOUND_PAUSABLE) != 0;
    s.follow = follow;
    s.offset = pos ? V3{pos[0], pos[1], pos[2]} : V3{0, 0, 0};
    s.volume = std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 1.0f;
    s.pitch = std::isfinite(pitch) && pitch > 0 ? std::clamp(pitch, 0.05f, 8.0f) : 1.0f;
    float nearR = rangeNear > 0 ? rangeNear : 5, farR = rangeFar > 0 ? rangeFar : 40;
    if (farR < nearR) farR = nearR;
    char name[32];
    snprintf(name, sizeof name, "mml_sound%d", s.id);
    bool placed = true;
    auto start = [&] {
        reinterpret_cast<SetNameFn>(Vt(frame, kVtSetName))(frame, name);
        reinterpret_cast<IntFn>(Vt(frame, kVtSetType))(frame, world ? kSndPoint : kSndAmbient);
        if (world) {
            reinterpret_cast<Float4Fn>(Vt(frame, kVtSetRange))(frame, nearR, farR, 0.4f, 0.3f);
            reinterpret_cast<Float2Fn>(Vt(frame, kVtSetCone))(frame, 6.2831855f, 6.2831855f);
        }
        reinterpret_cast<FloatFn>(Vt(frame, kVtSetOutVol))(frame, 1.0f);
        reinterpret_cast<FloatFn>(Vt(frame, kVtSetVolume))(frame, s.volume);
        reinterpret_cast<FloatFn>(Vt(frame, kVtSetFreq))(frame, s.pitch);
        reinterpret_cast<BoolFn>(Vt(frame, kVtSetLoop))(frame, s.looped);
        if (world) {
            V3 p;
            if (!WorldPlace(s, &p)) { placed = false; return; }
            Place(s, p, true);
        } else {
            reinterpret_cast<VoidFn>(Vt(frame, kVtUpdate))(frame);
        }
        s.length = reinterpret_cast<GetIntFn>(Vt(frame, kVtGetPlayTime))(frame);
        reinterpret_cast<SetOnFn>(Vt(frame, kVtSetOn))(frame, true);
    };
    if (Guarded(start) || !placed) {
        Unmake(s);
        *err = placed ? "starting it faulted (the frame is not a frame?)" : "the frame it follows has no place";
        return 0;
    }
    s.started = GetTickCount();
    g_sounds.push_back(s);
    return s.id;
}

bool SoundSet(int id, float volume, float pitch, std::string *err) {
    Sound *s = FindSound(id);
    if (!s) { *err = "not a sound (it ended, or sounds.list)"; return false; }
    auto f = [&] {
        if (std::isfinite(volume)) {
            s->volume = std::clamp(volume, 0.0f, 1.0f);
            reinterpret_cast<FloatFn>(Vt(s->frame, kVtSetVolume))(s->frame, s->volume);
        }
        if (std::isfinite(pitch) && pitch > 0) {
            float before = s->pitch;
            s->pitch = std::clamp(pitch, 0.05f, 8.0f);
            reinterpret_cast<FloatFn>(Vt(s->frame, kVtSetFreq))(s->frame, s->pitch);
            // (a one-shot's end: what is left of it goes at the new pitch)
            DWORD now = GetTickCount(), gone = now - s->started;
            s->started = now - static_cast<DWORD>(gone * before / s->pitch);
        }
    };
    if (Guarded(f)) { *err = "faulted"; return false; }
    return true;
}

bool SoundMove(int id, void *follow, const float *pos, std::string *err) {
    Sound *s = FindSound(id);
    if (!s) { *err = "not a sound (it ended, or sounds.list)"; return false; }
    if (!s->world) { *err = "a 2D sound has no place (play it with a place or a frame)"; return false; }
    s->follow = follow;
    s->offset = pos ? V3{pos[0], pos[1], pos[2]} : V3{0, 0, 0};
    V3 p;
    if (!WorldPlace(*s, &p)) { *err = "the frame has no place"; return false; }
    auto f = [&] { Place(*s, p, true); };
    if (Guarded(f)) { *err = "faulted"; return false; }
    return true;
}

bool SoundStop(int id, std::string *err) {
    auto it = std::find_if(g_sounds.begin(), g_sounds.end(), [&](const Sound &s) { return s.id == id; });
    if (it == g_sounds.end()) { *err = "not a sound (it ended, or sounds.list)"; return false; }
    Sound s = *it;
    g_sounds.erase(it);
    Unmake(s);
    return true;
}

int SoundState(int id, MmlSoundState *out) {
    Sound *s = FindSound(id);
    if (!s) return 0;
    if (!out) return 1;
    MmlSoundState st{};
    st.size = sizeof st;
    st.looped = s->looped;
    st.in_world = s->world;
    st.volume = s->volume;
    st.pitch = s->pitch;
    st.length_ms = s->length;
    auto f = [&] {
        st.playing = !s->held && reinterpret_cast<IsPlayingFn>(Vt(s->frame, kVtIsPlaying))(s->frame, false);
        st.ms = s->held ? s->heldAt : reinterpret_cast<GetIntFn>(Vt(s->frame, kVtGetCurrTime))(s->frame);
        if (s->world) {
            const float *w = game::WorldMatrix(s->frame);
            if (w) { st.pos[0] = w[12]; st.pos[1] = w[13]; st.pos[2] = w[14]; }
        }
    };
    Guarded(f);
    uint32_t size = out->size && out->size < sizeof st ? out->size : sizeof st;
    memcpy(out, &st, size);
    out->size = size;
    return 1;
}

int MusicPlay(MmlMod *mod, const char *file, float volume, uint32_t flags, std::string *err) {
    if (!MusicKnown()) { *err = "music is not known for this build"; return 0; }
    if (!file || !*file) { *err = "file: an .ogg or .mp3 (\"sounds\\\\music\\\\...\")"; return 0; }
    void *ig = *reinterpret_cast<void **>(E().igraph);
    if (!ig) { *err = "no IGraph"; return 0; }
    std::string path = file;
    std::replace(path.begin(), path.end(), '/', '\\');
    if (path.find('\\') == std::string::npos) path = "sounds\\music\\" + path;
    void *show = nullptr;
    int opened = -1;
    auto open = [&] {
        show = reinterpret_cast<CreateShowFn>(Vt(ig, 0))(ig);
        if (show) opened = reinterpret_cast<ShowOpenFn>(Vt(show, kShOpen))(show, path.c_str(), 0);
        // (as C_game::OpenIShow: an .ogg that is not there tried as an .mp3; and here the other way too, the
        // scripts name "...Game Over.mp3" where the archives have the .ogg)
        size_t dot = path.rfind('.');
        std::string ext = dot == std::string::npos ? "" : path.substr(dot);
        for (char &c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (show && opened != 0 && (ext == ".ogg" || ext == ".mp3")) {
            std::string other = path.substr(0, dot) + (ext == ".ogg" ? ".mp3" : ".ogg");
            if (reinterpret_cast<ShowOpenFn>(Vt(show, kShOpen))(show, other.c_str(), 0) == 0) { opened = 0; path = other; }
        }
    };
    if (Guarded(open)) { *err = "opening it faulted"; return 0; }
    if (!show) { *err = "IGraph made no player"; return 0; }
    Music m{++g_nextId, mod, show, path, std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 1.0f, (flags & MML_SOUND_LOOP) != 0, 0, 0};
    if (opened != 0) {
        UnmakeMusic(m);
        char b[16];
        snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(opened));
        *err = "no music \"" + path + "\" (" + b + ")";
        return 0;
    }
    ApplyMusicVolume(m);
    auto play = [&] { reinterpret_cast<ShowFn>(Vt(show, kShPlay))(show); };
    if (Guarded(play)) { UnmakeMusic(m); *err = "playing it faulted"; return 0; }
    g_music.push_back(m);
    return m.id;
}

bool MusicSet(int id, float volume, std::string *err) {
    Music *m = FindMusic(id);
    if (!m) { *err = "not a music (music.play's id)"; return false; }
    if (std::isfinite(volume)) m->volume = std::clamp(volume, 0.0f, 1.0f);
    ApplyMusicVolume(*m);
    return true;
}

bool MusicStop(int id, std::string *err) {
    auto it = std::find_if(g_music.begin(), g_music.end(), [&](const Music &m) { return m.id == id; });
    if (it == g_music.end()) { *err = "not a music (music.play's id)"; return false; }
    Music m = *it;
    g_music.erase(it);
    UnmakeMusic(m);
    return true;
}

int MusicGame(MmlMod *mod, int on) {
    void *g = game::g_exe && E().cityMusicPaused ? game::Game() : nullptr;
    if (!g) return -1;
    auto f = [&] { reinterpret_cast<CityPausedFn>(E().cityMusicPaused)(g, nullptr, on == 0); };
    if (Guarded(f)) return -1;
    g_cityPausedBy = on ? nullptr : mod;
    g_cityPausedByBridge = !on && !mod;
    return 1;
}

void SoundFrame() {
    if (g_sounds.empty() && g_music.empty()) return;
    bool paused = MissionState() == MML_MISSION_PAUSED;
    static DWORD last;
    DWORD now = GetTickCount(), step = last ? now - last : 0;
    last = now;
    for (size_t i = 0; i < g_sounds.size();) {
        Sound &s = g_sounds[i];
        bool gone = false;
        auto f = [&] {
            // (LS3D holds the world's sounds itself while the game is paused, and goes on with them after: their
            // time stands too)
            if (s.world && paused) s.started += step;
            if (!s.world && s.pausable && paused && !s.held) {   // a 2D one held where it is
                s.heldAt = reinterpret_cast<GetIntFn>(Vt(s.frame, kVtGetCurrTime))(s.frame);
                reinterpret_cast<SetOn2Fn>(Vt(s.frame, kVtSetOn2))(s.frame, false, true);
                s.held = true;
                s.heldSince = now;
            } else if (s.held && !paused) {
                reinterpret_cast<IntFn>(Vt(s.frame, kVtSetCurrTime))(s.frame, s.heldAt);
                reinterpret_cast<SetOnFn>(Vt(s.frame, kVtSetOn))(s.frame, true);
                s.started += now - s.heldSince;
                s.held = false;
            }
            if (s.world && s.follow && !s.held) {
                V3 p;
                if (WorldPlace(s, &p)) Place(s, p, Len(p - s.linkedAt) > 10.0f);
            }
            if (!s.looped && !s.held && s.length > 0 && now - s.started > static_cast<DWORD>(s.length / s.pitch) + 300) gone = true;
        };
        if (Guarded(f)) gone = true;   // (its frame went: a follow that was destroyed under it)
        if (gone) {
            Sound dead = s;
            g_sounds.erase(g_sounds.begin() + i);
            Unmake(dead);
        } else {
            i++;
        }
    }
    for (size_t i = 0; i < g_music.size();) {
        Music &m = g_music[i];
        // GetEvent: 0 while it plays, 1 once it has stopped (its end), 2 paused (an .ogg's: its flags +0x28; the
        // position is the decoder's, ahead of what is heard and moving in steps, so it does not tell the end)
        uint32_t pos = 0;
        int ev = 0;
        auto f = [&] {
            reinterpret_cast<ShowGetPosFn>(Vt(m.show, kShGetPos))(m.show, &pos);
            ev = reinterpret_cast<ShowFn>(Vt(m.show, kShGetEvent))(m.show);
        };
        Guarded(f);
        m.still = ev == 1 ? m.still + 1 : 0;
        m.lastPos = pos;
        if (!m.looped && m.still > 3) {   // (ended: released)
            Music dead = m;
            g_music.erase(g_music.begin() + i);
            UnmakeMusic(dead);
            continue;
        }
        i++;
        if (m.looped && m.still > 3) {   // (ended: played again from its start)
            auto again = [&] {
                reinterpret_cast<ShowFn>(Vt(m.show, kShStop))(m.show);
                reinterpret_cast<ShowPosFn>(Vt(m.show, kShSetPos))(m.show, 0);
                reinterpret_cast<ShowFn>(Vt(m.show, kShPlay))(m.show);
            };
            Guarded(again);
            m.still = 0;
            m.lastPos = 0;
        }
    }
}

void SoundModGone(MmlMod *mod) {
    std::vector<int> ids;
    for (const Sound &s : g_sounds) if (s.mod == mod) ids.push_back(s.id);
    std::string err;
    for (int id : ids) SoundStop(id, &err);
    ids.clear();
    for (const Music &m : g_music) if (m.mod == mod) ids.push_back(m.id);
    for (int id : ids) MusicStop(id, &err);
    if (mod && g_cityPausedBy == mod) MusicGame(mod, 1);
}

void SoundMissionClose() {   // (before the mission's scene goes: its 3D sounds; the 2D ones and the music stay)
    for (size_t i = 0; i < g_sounds.size();) {
        if (!g_sounds[i].world) { i++; continue; }
        Sound s = g_sounds[i];
        g_sounds.erase(g_sounds.begin() + i);
        Unmake(s);
    }
    g_cityPausedBy = nullptr;   // (the city's music starts again with the next mission's C_game)
    g_cityPausedByBridge = false;
}

namespace {
bool IsVec(const json &v) { return v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number(); }
json R3(const float *v) { return json::array({std::round(v[0] * 1000) / 1000, std::round(v[1] * 1000) / 1000, std::round(v[2] * 1000) / 1000}); }

json SoundJson(const Sound &s) {
    MmlSoundState st{};
    st.size = sizeof st;
    SoundState(s.id, &st);
    json j = {{"sound", s.id}, {"file", s.file}, {"by", s.mod ? ModIdOf(s.mod) : "bridge"}, {"playing", st.playing != 0}, {"looped", s.looped},
              {"in_world", s.world}, {"ms", st.ms}, {"length_ms", st.length_ms}, {"volume", s.volume}, {"pitch", s.pitch}, {"held", s.held}};
    if (s.world) j["pos"] = R3(st.pos);
    char f[16];
    snprintf(f, sizeof f, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(s.frame)));
    j["frame"] = f;   // (its I3D_sound: +0x15C the ISND sound, whose +0x08 is the DirectSound buffer)
    if (s.follow) {
        char b[16];
        snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(s.follow)));
        j["follows"] = b;
    }
    return j;
}

// Where a sound goes from the bridge: none (2D); `at` [x, y, z] (the world); `on` an actor ("player", "car",
// "0x...") and `bone`, or `frame` (any frame's address), with `pos` in its space.
bool Where(const json &p, void **follow, float *pos, bool *havePos, std::string *err) {
    *follow = nullptr;
    *havePos = false;
    if (p.contains("frame") && p["frame"].is_string()) {
        *follow = reinterpret_cast<void *>(ParseAddress(p["frame"].get<std::string>()));
        if (!*follow) { *err = "frame: an address"; return false; }
    } else if (p.contains("on") && p["on"].is_string()) {
        void *actor = nullptr;
        if (!ParseActor(p["on"].get<std::string>(), &actor, err)) { *err = "on: " + *err; return false; }
        void *model = ActorFrame(actor);
        if (!model) { *err = "on: the actor has no model"; return false; }
        *follow = model;
        if (p.contains("bone") && p["bone"].is_string()) {
            std::string b = p["bone"].get<std::string>();
            void *f = nullptr;
            auto g = [&] { f = game::FindChild(model, b.c_str()); };
            Guarded(g);
            if (!f) { *err = "bone: no frame \"" + b + "\" in its model"; return false; }
            *follow = f;
        }
    } else if (p.contains("at") && IsVec(p["at"])) {
        for (int i = 0; i < 3; i++) pos[i] = p["at"][i].get<float>();
        *havePos = true;
        return true;
    }
    if (p.contains("pos") && IsVec(p["pos"])) {
        for (int i = 0; i < 3; i++) pos[i] = p["pos"][i].get<float>();
        *havePos = true;
    }
    return true;
}
} // namespace

bool SoundMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    static const char *kMethods[] = {"sounds.list", "sound.play", "sound.set", "sound.stop", "music.play", "music.set", "music.stop", "music.game"};
    if (std::none_of(std::begin(kMethods), std::end(kMethods), [&](const char *m) { return method == m; })) return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string err;
    try {
        if (method == "sounds.list") {
            json sounds = json::array(), music = json::array();
            for (const Sound &s : g_sounds) sounds.push_back(SoundJson(s));
            for (const Music &m : g_music)
                music.push_back({{"music", m.id}, {"file", m.file}, {"by", m.mod ? ModIdOf(m.mod) : "bridge"}, {"volume", m.volume}, {"looped", m.looped},
                                 {"pos", m.lastPos}});
            json j = {{"known", SoundKnown()}, {"music_known", MusicKnown()}, {"sounds", sounds}, {"music", music}};
            if (MusicKnown()) j["music_volume_option"] = MusicScale();
            j["city_music_paused"] = g_cityPausedBy || g_cityPausedByBridge;
            *reply = dump(j);
        } else if (method == "sound.play") {
            void *follow = nullptr;
            float pos[3] = {0, 0, 0};
            bool havePos = false;
            if (!Where(p, &follow, pos, &havePos, &err)) { *reply = "sound.play: " + err; return true; }
            float range[2] = {0, 0};
            if (p.contains("range") && p["range"].is_array() && p["range"].size() == 2) { range[0] = p["range"][0].get<float>(); range[1] = p["range"][1].get<float>(); }
            else if (p.contains("range") && p["range"].is_number()) { range[1] = p["range"].get<float>(); range[0] = range[1] / 4; }
            uint32_t flags = (p.value("loop", false) ? MML_SOUND_LOOP : 0) | (p.value("pausable", false) ? MML_SOUND_PAUSABLE : 0);
            int id = SoundPlay(nullptr, p.value("file", "").c_str(), follow, havePos || follow ? pos : nullptr, p.value("volume", 1.0f), p.value("pitch", 1.0f),
                               range[0], range[1], flags, &err);
            if (!id) { *reply = "sound.play: " + err; return true; }
            *reply = dump(SoundJson(*FindSound(id)));
        } else if (method == "sound.set" || method == "sound.stop") {
            if (method == "sound.stop" && p.value("all", false)) {
                std::vector<int> ids;
                for (const Sound &s : g_sounds) ids.push_back(s.id);
                for (int id : ids) SoundStop(id, &err);
                *reply = dump({{"stopped", ids.size()}});
                *ok = true;
                return true;
            }
            int id = p.value("sound", 0);
            if (method == "sound.stop") {
                if (!SoundStop(id, &err)) { *reply = "sound.stop: " + err; return true; }
                *reply = dump({{"stopped", 1}});
            } else {
                if (p.contains("at") || p.contains("on") || p.contains("frame")) {
                    void *follow = nullptr;
                    float pos[3] = {0, 0, 0};
                    bool havePos = false;
                    if (!Where(p, &follow, pos, &havePos, &err) || !SoundMove(id, follow, havePos ? pos : nullptr, &err)) { *reply = "sound.set: " + err; return true; }
                }
                if (!SoundSet(id, p.value("volume", NAN), p.value("pitch", NAN), &err)) { *reply = "sound.set: " + err; return true; }
                *reply = dump(SoundJson(*FindSound(id)));
            }
        } else if (method == "music.play") {
            int id = MusicPlay(nullptr, p.value("file", "").c_str(), p.value("volume", 1.0f), p.value("loop", false) ? MML_SOUND_LOOP : 0, &err);
            if (!id) { *reply = "music.play: " + err; return true; }
            *reply = dump({{"music", id}, {"file", FindMusic(id)->file}});
        } else if (method == "music.set") {
            if (!MusicSet(p.value("music", 0), p.value("volume", NAN), &err)) { *reply = "music.set: " + err; return true; }
            *reply = dump({{"music", p.value("music", 0)}, {"volume", FindMusic(p.value("music", 0))->volume}});
        } else if (method == "music.stop") {
            if (p.value("all", false)) {
                std::vector<int> ids;
                for (const Music &m : g_music) ids.push_back(m.id);
                for (int id : ids) MusicStop(id, &err);
                *reply = dump({{"stopped", ids.size()}});
                *ok = true;
                return true;
            }
            if (!MusicStop(p.value("music", 0), &err)) { *reply = "music.stop: " + err; return true; }
            *reply = dump({{"stopped", 1}});
        } else {   // music.game
            int r = MusicGame(nullptr, p.value("on", true) ? 1 : 0);
            if (r < 0) { *reply = "music.game: no mission (the city's music is the mission's)"; return true; }
            *reply = dump({{"city_music", p.value("on", true) ? "on" : "paused"}});
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
