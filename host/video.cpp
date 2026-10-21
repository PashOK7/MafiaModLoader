// The game's videos played by the loader (knowledge/notes/videos.md; docs/MODDING.md "Videos").
//
// Every video of the game goes through LS3D's IShow (IGraph slot 0 makes one): 1.2's PlayVideo (the logos, the intro's
// script command) and 1.0's InitSystem and intro command alike call Open(name), SetRect, Play,
// its full-screen loop (until the end or a key) and Release. The game's own players are Bink 1.9c (1.2 only:
// the revisions BIKf..BIKi, nothing newer) and DirectShow (an AVI whose codec Windows decodes to RGB), into 16-bit
// textures; a file they cannot open is skipped without a word. The loader hooks IShow's open, close and that loop
// (game_build.h showOpen, showClose, showRenderUntilKey) and plays the video itself with FFmpeg (built by
// third_party/ffmpeg/build.py, loaded from <game>\mml\ffmpeg\ at the first video): any container and codec it was
// built with (MP4, MKV, WebM, AVI, Bink of every revision FFmpeg reads...; H.264, HEVC, VP9, AV1, MPEG-4, Bink 1,
// Indeo...), in 32-bit colour at its own size, fitted to the screen keeping its aspect.
//
// Which file: the game asks for a name ("logo1.avi", "logo1.bik", "intro.avi"); its base name ("logo1") is looked for
// with each extension in kExts' order, first among the mods' files (host/files.cpp: a loaded mod's files\logo1.mp4
// answers "logo1.mp4"; the later mod in load order wins a name), then loose in the game's folder (or in the folder the
// game's name has). The first one FFmpeg opens with a video it can decode is played; one it cannot (Bink 2, a codec
// not built, a broken file) is logged with the reason and the next is tried. None: the game's own player gets the
// name as before (the log says so). The game's file search (cdFind, cd_find_game_file) is answered "found" for a
// video name the loader can play from somewhere else (a mod's MP4 where the game's folder has no file of that name).
//
// The playing: a thread demuxes and decodes. The GPU decodes when it has a decoder for the stream (video_hw.cpp:
// D3D11VA, else DXVA2; the driver asked for the codec, its bit depth and the size), and turns each picture into BGRA
// at its size on the screen into a texture shared with the game's device (or copied through the CPU where the game's
// device cannot share one: DXVK's); otherwise FFmpeg's decoder on all cores and swscale on all cores, scaled to the
// size on the screen at most. A picture the clock has already passed by a quarter of a second is left out before any
// work. The sound becomes 16-bit stereo (swresample). The game thread, in the hooked loop, shows each picture when the
// clock reaches it (QueryPerformanceCounter from the first picture; an older picture still waiting is dropped) on the
// game's Direct3D 9 device (under dxwrapper's d3d8to9: QueryInterface; plain Direct3D 8 is left to the game's
// player), through IGraph's Clear, BeginScene, EndScene and Present (the loader's Present hook, screenshots, see
// it), the device's state kept in a state block around the loader's drawing (LS3D shadows the states it sets). The
// sound streams into a DirectSound buffer of the game's own IDirectSound (LS3D's sound driver, as Bink's), each
// sample placed on the same clock (late ones dropped, gaps filled with silence). The loop does what the game's
// does each turn: IGraph's ProcessWinMessages, keyboardUpdate and ReadKey (any key ends the video, as in the game),
// and it serves the bridge's game-thread methods (screenshot, video.state, video.skip) meanwhile.
//
// Bridge methods (game thread): video.state, video.find {name}, video.play {name}, video.skip. Events: VIDEO_STARTED,
// VIDEO_ENDED, VIDEO_REFUSED (the game's own player got it).
#include "host.h"
#include <d3d9.h>
#include <mmsystem.h>   // (WAVEFORMATEX: host.h leaves Windows lean)
#include <dsound.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "video_hw.h"

#ifndef MML_NO_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

using json = nlohmann::json;

namespace host {
namespace {

// the extensions a video's base name is looked for with, in this order (in the mods' files, then the game's folder)
const char *const kExts[] = {"mp4", "mkv", "webm", "mov", "bik", "avi"};

const game::ExeBuild &E() { return game::Exe(); }
const game::Ls3dBuild &L() { return game::Ls3d(); }
uintptr_t Ls3dBase() { return ModuleBase("LS3DF.dll"); }
void *Vt(void *obj, int off) { return (*reinterpret_cast<void ***>(obj))[off / 4]; }
template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

#define VLOG(...) ::host::Log(MML_LOG_INFO, "video", __VA_ARGS__)
#define VWARN(...) ::host::Log(MML_LOG_WARN, "video", __VA_ARGS__)

std::string Lower(std::string s) {
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
bool EndsWith(const std::string &s, const char *tail) {
    size_t n = strlen(tail);
    return s.size() >= n && Lower(s.substr(s.size() - n)) == tail;
}
// a name the game opens a video by (its IShow also opens .ogg and .mp3 music: never the loader's)
bool IsVideoName(const char *name) {
    if (!name) return false;
    std::string n = name;
    for (const char *e : kExts)
        if (EndsWith(n, (std::string(".") + e).c_str())) return true;
    return false;
}

// One file found for a name: where, and from whom.
struct Candidate { std::string path, from; };

// The game's name split: its folder as given ("" for a bare name; may be absolute: a disc's X:\MafiaGame\) and its base
// name without the extension.
void SplitName(const std::string &name, std::string *dir, std::string *base) {
    size_t slash = name.find_last_of("\\/");
    *dir = slash == std::string::npos ? "" : name.substr(0, slash + 1);
    std::string file = slash == std::string::npos ? name : name.substr(slash + 1);
    size_t dot = file.find_last_of('.');
    *base = dot == std::string::npos ? file : file.substr(0, dot);
}
bool IsAbsolute(const std::string &p) { return p.size() > 1 && (p[1] == ':' || (p[0] == '\\' && p[1] == '\\')); }

// The mod whose files\ holds a path (<game>\mml\...\mods\<id>\files\...), "a mod" when it cannot be told.
std::string ModOfPath(const std::string &path) {
    std::string l = Lower(path);
    size_t f = l.rfind("\\files\\");
    if (f == std::string::npos) f = l.rfind("/files/");
    if (f == std::string::npos) return "a mod";
    size_t s = l.find_last_of("\\/", f - 1);
    return "mod " + path.substr(s + 1, f - s - 1);
}

std::vector<Candidate> Candidates(const std::string &name) {
    std::string dir, base;
    SplitName(name, &dir, &base);
    std::vector<Candidate> out;
    if (base.empty()) return out;
    // (the game's name is in the system's code page; the mods' names and the paths are UTF-8)
    std::string rel = IsAbsolute(dir) ? "" : AcpToUtf8(dir.c_str());
    std::string b8 = AcpToUtf8(base.c_str());
    char path[1024];
    for (const char *e : kExts)
        if (FilesSource((rel + b8 + "." + e).c_str(), path, sizeof path)) out.push_back({path, ModOfPath(path)});
    std::string folder = IsAbsolute(dir) ? AcpToUtf8(dir.c_str()) : GameDir() + rel;
    for (const char *e : kExts) {
        std::string p = folder + b8 + "." + e;
        if (PathExists(p)) out.push_back({p, IsAbsolute(dir) ? "the disc's folder" : "the game's folder"});
    }
    return out;
}

// What was played (or refused) lately, for video.state.
struct Record { std::string name, file, from, video, audio, result, path; int frames = 0, dropped = 0; double ms = 0; bool skipped = false; json timing; };
std::deque<Record> g_records;
void Remember(const Record &r) {
    g_records.push_back(r);
    if (g_records.size() > 16) g_records.pop_front();
}

struct Video;
Video *g_playing;                       // the video being played (the hooked loop runs)
bool g_skipAsked;                       // video.skip: it ends at the loop's next turn
std::string g_pendingPlay;              // video.play's name, played at the next frame
std::string g_pendingDecode;            // and its `decode`

#ifndef MML_NO_FFMPEG

// ---- FFmpeg, loaded from <game>\mml\ffmpeg\ at the first video (its DLLs are delay-loaded: CMakeLists.txt)
enum class FfState { Unknown, Ok, Missing };
FfState g_ff = FfState::Unknown;
std::string g_ffWhy, g_ffVersion, g_ffLicense;
std::atomic<int> g_ffLogBudget;   // FFmpeg's own warnings logged for the current video (the rest dropped)

#define MML_STR2(x) #x
#define MML_STR(x) MML_STR2(x)
const char *const kFfDlls[] = {
    "avutil-" MML_STR(LIBAVUTIL_VERSION_MAJOR) ".dll", "swresample-" MML_STR(LIBSWRESAMPLE_VERSION_MAJOR) ".dll",
    "swscale-" MML_STR(LIBSWSCALE_VERSION_MAJOR) ".dll", "avcodec-" MML_STR(LIBAVCODEC_VERSION_MAJOR) ".dll",
    "avformat-" MML_STR(LIBAVFORMAT_VERSION_MAJOR) ".dll"};

void __cdecl FfLog(void *avcl, int level, const char *fmt, va_list ap) {
    if (level > AV_LOG_WARNING || g_ffLogBudget.fetch_sub(1) <= 0) return;
    char b[512];
    vsnprintf(b, sizeof b, fmt, ap);
    size_t n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = 0;
    const AVClass *c = avcl ? *reinterpret_cast<const AVClass **>(avcl) : nullptr;
    Log(MML_LOG_WARN, "video", "ffmpeg%s%s: %s", c ? " " : "", c ? c->item_name(avcl) : "", b);
}

bool FfLoad() {
    if (g_ff != FfState::Unknown) return g_ff == FfState::Ok;
    g_ff = FfState::Missing;
    std::string dir = MmlDir() + "ffmpeg\\";
    for (const char *dll : kFfDlls)
        if (!PathLoadLibrary(dir + dll, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            char b[64];
            snprintf(b, sizeof b, " (error %lu)", GetLastError());
            g_ffWhy = std::string("cannot load ") + dir + dll + b;
            return false;
        }
    unsigned v = 0;
    auto f = [&] {
        v = avformat_version();
        g_ffVersion = av_version_info();
        g_ffLicense = avcodec_license();
        av_log_set_callback(&FfLog);
    };
    if (DWORD ex = Guarded(f)) {
        char b[96];
        snprintf(b, sizeof b, "FFmpeg's functions not found in mml\\ffmpeg (exception 0x%08lX)", ex);
        g_ffWhy = b;
        return false;
    }
    if (AV_VERSION_MAJOR(v) != LIBAVFORMAT_VERSION_MAJOR) {
        g_ffWhy = "mml\\ffmpeg holds another FFmpeg than the loader was built with";
        return false;
    }
    g_ff = FfState::Ok;
    VLOG("FFmpeg %s loaded from %s (%s)", g_ffVersion.c_str(), dir.c_str(), g_ffLicense.c_str());
    return true;
}

std::string FfError(int err) {
    char b[128];
    av_strerror(err, b, sizeof b);
    return b;
}

// ---- one video: the file opened, its decoders, the decoding thread and what it made
// A picture on its way to the screen: its BGRA pixels (decoded in software, or the GPU's copied through the CPU) or the
// GPU slot it is in (a texture shared with the game's device), and its time.
struct Pic { std::vector<uint8_t> bgra; int slot = -1; double pts = 0; };
struct Chunk { std::vector<int16_t> s; size_t at = 0; double pts; };   // stereo samples, the first one's time

std::string g_forceDecode;              // video.play's `decode` for the video it plays ("": the GPU when it can)
constexpr int kSlots = 4;               // GPU pictures on their way at once (in the queue, on the screen, retiring)

struct Video {
    std::string name, file, from, desc, adesc;
    AVFormatContext *fmt = nullptr;
    AVCodecContext *vdec = nullptr, *adec = nullptr;
    int vs = -1, as = -1;
    int w = 0, h = 0, tw = 0, th = 0;   // the picture's size, and the size it is turned to (its size on the screen at most)
    double aspect = 1;                  // the picture's shape on the screen (its pixels' aspect included)
    double origin = 0;                  // the time of the file's start (the first picture's)
    double duration = 0;
    int rate = 0;                       // the sound's samples a second (0: none)
    // the GPU (video_hw.cpp)
    videohw::Api api = videohw::kSoftware;   // what the decoder was opened with
    std::string hwWhy;                  // why not the GPU (or not the first of its APIs)
    videohw::Session *gpu = nullptr;    // the shared slots or the copy (made and ended by the game thread)
    std::vector<int> freeSlots;         // (under mx)
    bool gpuFailed = false;             // a GPU conversion failed: its pictures go through the CPU from then on
    bool hwSeen = false, swSeen = false;   // the decoder gave GPU pictures, CPU pictures
    std::atomic<double> clockStart{0};  // Now() when the first picture was shown (0 before): the decoding thread's clock
    // made by the thread
    std::thread worker;
    std::atomic<bool> stop{false};
    std::mutex mx;
    std::condition_variable cv;
    std::deque<Pic> pics;
    std::vector<std::vector<uint8_t>> spare;
    size_t maxPics = 3;
    std::deque<Chunk> chunks;
    double audioQueued = 0;             // seconds of sound waiting
    bool ended = false;                 // the file read to its end, the decoders drained
    std::string error;                  // the thread stopped on this
    double lastPts = 0, audioEnd = 0;
    // where the time goes (ms, summed; read once the thread has ended): the decoder's calls and the conversion on
    // the decoding thread, the upload and the frame (Clear to Present) on the game thread
    double tDecode = 0, tConvert = 0, tUpload = 0, tPresent = 0, maxConvert = 0, maxUpload = 0, maxPresent = 0;
    int decoded = 0, late = 0, uploads = 0;
};
std::map<void *, Video *> g_videos;     // IShow* -> the loader's video

double Now();

void Free(Video *v) {
    if (!v) return;
    v->stop = true;
    v->cv.notify_all();
    if (v->worker.joinable()) v->worker.join();
    avcodec_free_context(&v->vdec);
    avcodec_free_context(&v->adec);
    avformat_close_input(&v->fmt);
    delete v;
}

double Seconds(int64_t ts, AVRational tb) { return ts == AV_NOPTS_VALUE ? -1 : ts * av_q2d(tb); }

std::string CodecName(const AVCodecContext *c) {
    const AVCodecDescriptor *d = avcodec_descriptor_get(c->codec_id);
    return d ? d->name : "?";
}

// The decoder's choice of picture format: the GPU's when FFmpeg offers it, else the first of the CPU's.
AVPixelFormat GetFormat(AVCodecContext *c, const AVPixelFormat *fmts) {
    const Video *v = static_cast<const Video *>(c->opaque);
    int want = videohw::PixelFormat(v->api);
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == want) return *p;
    for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
    }
    return AV_PIX_FMT_NONE;
}

// The video's decoder, for the GPU (`device`: FFmpeg's device, taken) or the CPU. AV1 has two: FFmpeg's own decodes
// only on a GPU, dav1d only on the CPU.
bool OpenVideoDecoder(Video *v, videohw::Api api, AVBufferRef *device, std::string *why) {
    AVStream *st = v->fmt->streams[v->vs];
    const AVCodec *dc = nullptr;
    if (st->codecpar->codec_id == AV_CODEC_ID_AV1) dc = avcodec_find_decoder_by_name(api != videohw::kSoftware ? "av1" : "libdav1d");
    if (!dc) dc = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dc) {
        av_buffer_unref(&device);
        *why = "no decoder for its video";
        return false;
    }
    avcodec_free_context(&v->vdec);
    v->vdec = avcodec_alloc_context3(dc);
    avcodec_parameters_to_context(v->vdec, st->codecpar);
    v->api = api;
    if (api != videohw::kSoftware) {
        v->vdec->hw_device_ctx = device;   // (the context's reference now)
        v->vdec->opaque = v;
        v->vdec->get_format = GetFormat;
        v->vdec->thread_count = 1;         // (the GPU decodes; a thread a frame would only hold more of its surfaces)
    } else {
        av_buffer_unref(&device);
        v->vdec->thread_count = 0;         // (as many as the machine has)
        v->vdec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    int r = avcodec_open2(v->vdec, dc, nullptr);
    if (r < 0) {
        *why = std::string("its ") + dc->name + " decoder refused it (" + FfError(r) + ")";
        return false;
    }
    return true;
}

void Decode(Video *v) {
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc(), *cpu = av_frame_alloc(), *dst = av_frame_alloc();
    SwsContext *sws = sws_alloc_context();
    if (sws) {
        sws->threads = 0;   // (as many as the machine has)
        sws->flags = SWS_BICUBIC;
    }
    SwrContext *swr = nullptr;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    double nextVideo = 0, nextAudio = 0;
    bool anyPicture = false;
    int errors = 0;
    const bool shared = videohw::Shared(v->gpu);
    // A picture into BGRA through the CPU (swscale on all cores: the picture's colours, scaled to tw x th)
    auto toBgra = [&](const AVFrame *src, std::vector<uint8_t> *buf) -> bool {
        buf->resize(static_cast<size_t>(v->tw) * v->th * 4);
        av_frame_unref(dst);
        dst->format = AV_PIX_FMT_BGRA;
        dst->width = v->tw;
        dst->height = v->th;
        dst->data[0] = buf->data();
        dst->linesize[0] = v->tw * 4;
        return sws && sws_scale_frame(sws, dst, src) >= 0;
    };
    auto picture = [&]() -> bool {   // fr onto the queue, turned for the screen; false: stopped
        double pts = Seconds(fr->best_effort_timestamp, v->fmt->streams[v->vs]->time_base);
        pts = pts < 0 ? nextVideo : pts - v->origin;
        AVRational fps = v->fmt->streams[v->vs]->avg_frame_rate;
        nextVideo = pts + (fps.num > 0 ? 1.0 / av_q2d(fps) : 0.04);
        anyPicture = true;
        // a picture the clock has passed by a quarter of a second is left out before any work on it
        double cs = v->clockStart.load();
        if (cs > 0 && pts < Now() - cs - 0.25) { v->late++; return true; }
        const bool hw = v->api != videohw::kSoftware && fr->format == videohw::PixelFormat(v->api);
        (hw ? v->hwSeen : v->swSeen) = true;
        // a stream that does not say its colours: BT.709 from 720 lines up, BT.601 below (as the GPU paths take it, as
        // players do; swscale alone would take BT.601 for all)
        if (fr->colorspace == AVCOL_SPC_UNSPECIFIED) fr->colorspace = fr->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
        Pic pic;
        pic.pts = pts;
        const bool gpu = hw && v->gpu && !v->gpuFailed;
        {
            std::unique_lock<std::mutex> lk(v->mx);
            v->cv.wait(lk, [&] { return v->stop || (v->pics.size() < v->maxPics && (!gpu || !shared || !v->freeSlots.empty())); });
            if (v->stop) return false;
            if (gpu && shared) { pic.slot = v->freeSlots.back(); v->freeSlots.pop_back(); }
            else if (!v->spare.empty()) { pic.bgra.swap(v->spare.back()); v->spare.pop_back(); }
        }
        double t0 = Now();
        bool ok = false;
        if (gpu) {
            std::string err;
            ok = videohw::Convert(v->gpu, fr, pic.slot, &pic.bgra, &err);
            if (!ok) {
                VWARN("'%s': the GPU could not turn a picture for the screen (%s): its pictures go through the CPU from now",
                      AcpToUtf8(v->name.c_str()).c_str(), err.c_str());
                v->gpuFailed = true;
                std::lock_guard<std::mutex> lk(v->mx);
                if (pic.slot >= 0) v->freeSlots.push_back(pic.slot);
                pic.slot = -1;
            }
        }
        if (!ok) {
            const AVFrame *src = fr;
            if (hw) {   // the GPU's picture brought to the CPU
                av_frame_unref(cpu);
                if (av_hwframe_transfer_data(cpu, fr, 0) < 0) { v->error = "a GPU picture could not be read back"; return false; }
                av_frame_copy_props(cpu, fr);
                src = cpu;
            }
            if (!toBgra(src, &pic.bgra)) { v->error = "swscale cannot turn its pictures into BGRA"; return false; }
        }
        double ms = (Now() - t0) * 1000.0;
        v->tConvert += ms;
        v->maxConvert = std::max(v->maxConvert, ms);
        v->decoded++;
        std::lock_guard<std::mutex> lk(v->mx);
        v->pics.push_back(std::move(pic));
        v->lastPts = std::max(v->lastPts, pts);
        return true;
    };
    auto sound = [&]() -> bool {   // fr into 16-bit stereo at the file's rate
        if (!swr) {
            if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, v->rate, &fr->ch_layout,
                                    static_cast<AVSampleFormat>(fr->format), fr->sample_rate, 0, nullptr) < 0 || swr_init(swr) < 0) {
                swr_free(&swr);
                v->rate = 0;   // (no sound: the picture plays on)
                return true;
            }
        }
        double pts = Seconds(fr->best_effort_timestamp, v->fmt->streams[v->as]->time_base);
        pts = pts < 0 ? nextAudio : pts - v->origin;
        Chunk c;
        c.pts = pts;
        int cap = swr_get_out_samples(swr, fr->nb_samples);
        c.s.resize(static_cast<size_t>(std::max(cap, 0)) * 2);
        uint8_t *out = reinterpret_cast<uint8_t *>(c.s.data());
        int n = swr_convert(swr, &out, cap, const_cast<const uint8_t **>(fr->extended_data), fr->nb_samples);
        if (n <= 0) return true;
        c.s.resize(static_cast<size_t>(n) * 2);
        nextAudio = pts + static_cast<double>(n) / v->rate;
        std::unique_lock<std::mutex> lk(v->mx);
        v->cv.wait(lk, [&] { return v->stop || v->audioQueued < 3.0; });
        if (v->stop) return false;
        v->audioQueued += static_cast<double>(n) / v->rate;
        v->audioEnd = std::max(v->audioEnd, nextAudio);
        v->chunks.push_back(std::move(c));
        return true;
    };
    auto receive = [&](AVCodecContext *dec, bool video) -> bool {
        for (;;) {
            double t0 = Now();
            int r = avcodec_receive_frame(dec, fr);
            if (video) v->tDecode += (Now() - t0) * 1000.0;
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
            if (r < 0) { if (video) errors++; return true; }   // (a broken frame: the next one)
            bool ok = video ? picture() : sound();
            av_frame_unref(fr);
            if (!ok) return false;
        }
    };
    while (!v->stop) {
        // the GPU's decoder failing from the start (a driver that took the stream and then refused it): the CPU's,
        // from the file's start
        if (v->api != videohw::kSoftware && !anyPicture && errors >= 3) {
            std::string why;
            VWARN("'%s': the GPU's %s decoder failed on its first pictures: decoded in software", AcpToUtf8(v->name.c_str()).c_str(),
                  videohw::ApiName(v->api));
            v->hwWhy = std::string("the GPU's ") + videohw::ApiName(v->api) + " decoder failed on its first pictures";
            if (!OpenVideoDecoder(v, videohw::kSoftware, nullptr, &why)) { v->error = why; break; }
            av_seek_frame(v->fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
            if (v->adec) avcodec_flush_buffers(v->adec);
            errors = 0;
        }
        int r = av_read_frame(v->fmt, pkt);
        if (r < 0) {   // the end: the decoders drained
            if (v->vdec) { avcodec_send_packet(v->vdec, nullptr); if (!receive(v->vdec, true)) break; }
            if (v->adec && v->rate) { avcodec_send_packet(v->adec, nullptr); if (!receive(v->adec, false)) break; }
            break;
        }
        // (each packet's pictures and sound taken out at once: the decoder never refuses the next packet as full)
        bool go = true;
        if (pkt->stream_index == v->vs) {
            double t0 = Now();
            int s = avcodec_send_packet(v->vdec, pkt);
            v->tDecode += (Now() - t0) * 1000.0;
            if (s < 0 && s != AVERROR(EAGAIN)) errors++;
            go = receive(v->vdec, true);
        } else if (pkt->stream_index == v->as && v->adec && v->rate) {
            avcodec_send_packet(v->adec, pkt);
            go = receive(v->adec, false);
        }
        av_packet_unref(pkt);
        if (!go) break;
    }
    sws_free_context(&sws);
    swr_free(&swr);
    av_frame_free(&dst);
    av_frame_free(&cpu);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    std::lock_guard<std::mutex> lk(v->mx);
    v->ended = true;
    v->cv.notify_all();
}

// The file opened, its streams' decoders ready; null and *why when FFmpeg cannot play it.
Video *Prepare(const std::string &name, const Candidate &c, std::string *why) {
    // Bink 2 first: FFmpeg's Bink demuxer reads its header, but no decoder exists for its pictures
    if (FILE *f = FileOpen(c.path, "rb")) {
        char head[4] = {};
        size_t n = fread(head, 1, 4, f);
        fclose(f);
        if (n == 4 && !memcmp(head, "KB2", 3)) { *why = std::string("Bink 2 (KB2") + head[3] + "): no open decoder exists for it"; return nullptr; }
    } else {
        *why = "cannot be opened";
        return nullptr;
    }
    Video *v = new Video;
    v->name = name;
    v->file = c.path;
    v->from = c.from;
    int r = avformat_open_input(&v->fmt, c.path.c_str(), nullptr, nullptr);
    if (r < 0) { *why = "not a container FFmpeg reads (" + FfError(r) + ")"; Free(v); return nullptr; }
    avformat_find_stream_info(v->fmt, nullptr);
    const AVCodec *ac = nullptr;
    v->vs = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v->vs < 0) { *why = "no video in it"; Free(v); return nullptr; }
    AVStream *st = v->fmt->streams[v->vs];
    if (!avcodec_find_decoder(st->codecpar->codec_id)) {
        const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
        *why = std::string("its video's codec (") + (d ? d->name : "?") + ") is not one this FFmpeg decodes";
        Free(v);
        return nullptr;
    }
    if (st->codecpar->codec_id == AV_CODEC_ID_BINKVIDEO && st->codecpar->codec_tag >> 24 &&
        (st->codecpar->codec_tag & 0xFFFFFF) == ('B' | 'I' << 8 | 'K' << 16)) {
        char rev = static_cast<char>(st->codecpar->codec_tag >> 24);
        v->desc = std::string("Bink 1 (BIK") + rev + ")";
    }
    // the GPU's decoder when it has one for this stream (video_hw.cpp asks the driver), else the CPU's
    AVBufferRef *device = nullptr;
    videohw::Api api = videohw::Choose(st->codecpar, g_forceDecode, &device, &v->hwWhy);
    std::string whyHw;
    if (!OpenVideoDecoder(v, api, device, api != videohw::kSoftware ? &whyHw : why)) {
        if (api == videohw::kSoftware) { Free(v); return nullptr; }
        v->hwWhy = std::string(videohw::ApiName(api)) + ": " + whyHw;
        if (!OpenVideoDecoder(v, videohw::kSoftware, nullptr, why)) { Free(v); return nullptr; }
    }
    v->w = v->vdec->width;
    v->h = v->vdec->height;
    if (v->w <= 0 || v->h <= 0) { *why = "its pictures have no size"; Free(v); return nullptr; }
    AVRational sar = av_guess_sample_aspect_ratio(v->fmt, st, nullptr);
    v->aspect = static_cast<double>(v->w) / v->h * (sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0);
    double start = Seconds(st->start_time, st->time_base);
    v->origin = start > 0 ? start : 0;
    v->duration = v->fmt->duration > 0 ? v->fmt->duration / static_cast<double>(AV_TIME_BASE) : Seconds(st->duration, st->time_base);
    AVRational fps = st->avg_frame_rate;
    const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(st->codecpar->format));
    char b[160];
    snprintf(b, sizeof b, "%s%s%dx%d, %.3g fps%s", v->desc.c_str(), v->desc.empty() ? "" : ", ", v->w, v->h, fps.num > 0 ? av_q2d(fps) : 0.0,
             pd && pd->comp[0].depth > 8 ? ", 10-bit" : "");
    v->desc = (v->desc.empty() ? CodecName(v->vdec) + " " : "") + b;
    v->as = av_find_best_stream(v->fmt, AVMEDIA_TYPE_AUDIO, -1, v->vs, &ac, 0);
    if (v->as >= 0) {
        v->adec = avcodec_alloc_context3(ac);
        avcodec_parameters_to_context(v->adec, v->fmt->streams[v->as]->codecpar);
        if (avcodec_open2(v->adec, ac, nullptr) >= 0 && v->adec->sample_rate > 0) {
            v->rate = std::clamp(v->adec->sample_rate, 8000, 192000);
            snprintf(b, sizeof b, "%s %d Hz, %d channels", CodecName(v->adec).c_str(), v->adec->sample_rate, v->adec->ch_layout.nb_channels);
            v->adesc = b;
        } else {
            avcodec_free_context(&v->adec);
            v->adesc = "its sound cannot be decoded: silent";
        }
    } else if (v->as == AVERROR_DECODER_NOT_FOUND) {
        v->adesc = "its sound's codec is not one this FFmpeg decodes: silent";
    }
    return v;
}

// The device the game draws with, as Direct3D 9 (null: plain Direct3D 8, or not up yet). AddRef'd.
IDirect3DDevice9 *Device9() {
    if (!L().d3dDevice) return nullptr;
    void *dev8 = nullptr;
    MemRead(Ls3dBase() + L().d3dDevice, &dev8, 4);
    if (!dev8) return nullptr;
    // IID_IDirect3DDevice9 {D0223B96-BF7A-43FD-92BD-A43B0D82B9EB}
    static const GUID kIid = {0xD0223B96, 0xBF7A, 0x43FD, {0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB}};
    IDirect3DDevice9 *d9 = nullptr;
    if (FAILED(static_cast<IUnknown *>(dev8)->QueryInterface(kIid, reinterpret_cast<void **>(&d9)))) return nullptr;
    return d9;
}

// LS3D's IDirectSound (its sound driver's; null: the game has no sound)
IDirectSound *GameSound() {
    if (!L().sndDriver) return nullptr;
    void *drv = nullptr;
    MemRead(Ls3dBase() + L().sndDriver, &drv, 4);
    if (!drv) return nullptr;
    bool up = false;
    auto f = [&] { up = reinterpret_cast<bool(__stdcall *)(void *)>(Vt(drv, 0x0C))(drv); };
    if (Guarded(f) || !up) return nullptr;
    IDirectSound *ds = nullptr;
    MemRead(reinterpret_cast<uintptr_t>(drv) + 0x0C, &ds, 4);
    return ds;
}

void *Igraph() {
    void *ig = nullptr;
    if (E().igraph) MemRead(E().igraph, &ig, 4);
    return ig;
}
using IgFn = void(__stdcall *)(void *ig);
using IgClearFn = void(__stdcall *)(void *ig, uint32_t color, float z, uint32_t flags);
using IgKeyFn = int(__stdcall *)(void *ig);

// A high-resolution wait (the loop sleeps until the next picture is due)
void WaitMs(double ms) {
    if (ms <= 0.2) { SwitchToThread(); return; }
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) { Sleep(1); return; }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(ms * 10000.0);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
}

// ---- the playing (the game thread, inside the hooked loop)
struct Screen {
    IDirect3DDevice9 *d9 = nullptr;
    IDirect3DTexture9 *tex = nullptr;   // the CPU's pictures' (software decoding, the copy path), made at the first one
    IDirect3DStateBlock9 *saved = nullptr;
    UINT bw = 0, bh = 0;
    UINT texW = 0, texH = 0;            // its size (a power of two where the device asks for one)
    float u1 = 1, v1 = 1;
};

// The picture's place on the screen: fitted to the back buffer, its aspect kept
void FitRect(const Screen *s, double aspect, float *x, float *y, float *w, float *h) {
    float sw = static_cast<float>(s->bw), sh = static_cast<float>(s->bh);
    *w = sw;
    *h = static_cast<float>(sw / aspect);
    if (*h > sh) { *h = sh; *w = static_cast<float>(sh * aspect); }
    *x = (sw - *w) / 2;
    *y = (sh - *h) / 2;
}

bool ScreenOpen(Screen *s, Video *v, std::string *why) {
    s->d9 = Device9();
    if (!s->d9) { *why = "the game's device is not Direct3D 9 underneath"; return false; }
    IDirect3DSurface9 *bb = nullptr;
    D3DSURFACE_DESC d{};
    if (SUCCEEDED(s->d9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) { bb->GetDesc(&d); bb->Release(); }
    s->bw = d.Width;
    s->bh = d.Height;
    if (!s->bw || !s->bh) { *why = "the game's back buffer has no size"; return false; }
    D3DCAPS9 caps{};
    s->d9->GetDeviceCaps(&caps);
    // The size a picture is turned to: its size on the screen (the GPU's pictures are scaled to it, up or down, by the
    // GPU), a CPU picture never larger than its own size (the device's filter makes it larger), never larger than
    // the device's largest texture.
    float x, y, fw, fh;
    FitRect(s, v->aspect, &x, &y, &fw, &fh);
    double k = v->api != videohw::kSoftware ? fw / v->w : std::min(1.0, static_cast<double>(fw) / v->w);
    k = std::min({k, static_cast<double>(caps.MaxTextureWidth) / v->w, static_cast<double>(caps.MaxTextureHeight) / v->h});
    v->tw = std::max(2, static_cast<int>(v->w * k + 0.5) & ~1);
    v->th = std::max(2, static_cast<int>(v->h * k + 0.5) & ~1);
    s->texW = v->tw;
    s->texH = v->th;
    if ((caps.TextureCaps & D3DPTEXTURECAPS_POW2) && !(caps.TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL)) {
        s->texW = 1; while (s->texW < static_cast<UINT>(v->tw)) s->texW <<= 1;
        s->texH = 1; while (s->texH < static_cast<UINT>(v->th)) s->texH <<= 1;
    }
    s->u1 = static_cast<float>(v->tw) / s->texW;
    s->v1 = static_cast<float>(v->th) / s->texH;
    s->d9->CreateStateBlock(D3DSBT_ALL, &s->saved);
    return true;
}

void ScreenClose(Screen *s) {
    if (s->saved) s->saved->Release();
    if (s->tex) s->tex->Release();
    if (s->d9) s->d9->Release();
    *s = Screen{};
}

bool Upload(Screen *s, const Pic &p, int w, int h, std::string *why) {
    if (!s->tex) {
        HRESULT hr = s->d9->CreateTexture(s->texW, s->texH, 1, D3DUSAGE_DYNAMIC, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &s->tex, nullptr);
        if (FAILED(hr)) {
            char b[96];
            snprintf(b, sizeof b, "a %ux%u texture could not be made (0x%08lX)", s->texW, s->texH, hr);
            *why = b;
            return false;
        }
    }
    D3DLOCKED_RECT lr{};
    if (FAILED(s->tex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) return true;
    for (int y = 0; y < h; y++)
        memcpy(static_cast<uint8_t *>(lr.pBits) + static_cast<size_t>(lr.Pitch) * y, p.bgra.data() + static_cast<size_t>(w) * 4 * y, static_cast<size_t>(w) * 4);
    s->tex->UnlockRect(0);
    return true;
}

// A picture drawn in its place on the screen (black around it: IGraph's Clear before): the CPU's texture, or a GPU
// slot's (already at its size on the screen: drawn 1:1)
void Draw(Screen *s, double aspect, IDirect3DTexture9 *tex, float u1, float v1) {
    IDirect3DDevice9 *d = s->d9;
    if (s->saved) s->saved->Capture();
    float x, y, w, h;
    FitRect(s, aspect, &x, &y, &w, &h);
    float x0 = x - 0.5f, y0 = y - 0.5f;
    struct V { float x, y, z, rhw, u, v; } q[4] = {
        {x0, y0, 0, 1, 0, 0}, {x0 + w, y0, 0, 1, u1, 0}, {x0, y0 + h, 0, 1, 0, v1}, {x0 + w, y0 + h, 0, 1, u1, v1}};
    D3DVIEWPORT9 vp = {0, 0, s->bw, s->bh, 0, 1};
    d->SetViewport(&vp);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    d->SetRenderState(D3DRS_ZENABLE, FALSE);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    d->SetTexture(0, tex);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    if (s->saved) s->saved->Apply();
}

// The sound: a DirectSound buffer of a second, kept 0.3 s ahead of what plays, each sample at its time on the clock.
struct Speaker {
    IDirectSoundBuffer *buf = nullptr;
    DWORD size = 0, wpos = 0, lastPlay = 0;
    uint64_t written = 0, played = 0;   // samples (stereo pairs)
    int rate = 0;
    bool started = false;
};

bool SpeakerOpen(Speaker *sp, int rate) {
    IDirectSound *ds = GameSound();
    if (!ds || rate <= 0) return false;
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = static_cast<DWORD>(rate);
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * 4;
    DSBUFFERDESC bd{};
    bd.dwSize = sizeof bd;
    bd.dwFlags = DSBCAPS_GETCURRENTPOSITION2;   // (heard while the game's window has the focus, as the game's sounds)
    bd.dwBufferBytes = wf.nAvgBytesPerSec;
    bd.lpwfxFormat = &wf;
    if (FAILED(ds->CreateSoundBuffer(&bd, &sp->buf, nullptr))) return false;
    sp->size = bd.dwBufferBytes;
    sp->rate = rate;
    return true;
}

// Fills the buffer up to 0.3 s ahead: the samples whose time is the next one to play (startTime: the time the
// buffer's first sample stands for); late ones dropped, a gap filled with silence.
void SpeakerFill(Speaker *sp, Video *v, double startTime) {
    if (!sp->buf) return;
    DWORD play = 0, wr = 0;
    if (sp->started && SUCCEEDED(sp->buf->GetCurrentPosition(&play, &wr))) {
        sp->played += ((play + sp->size - sp->lastPlay) % sp->size) / 4;
        sp->lastPlay = play;
    }
    if (sp->played > sp->written) sp->written = sp->played;   // (it ran dry: what it played is gone)
    uint64_t ahead = static_cast<uint64_t>(sp->rate * 0.3);
    uint64_t want = sp->played + ahead > sp->written ? sp->played + ahead - sp->written : 0;
    if (want < 64) return;
    std::vector<int16_t> out(static_cast<size_t>(want) * 2, 0);
    {
        std::lock_guard<std::mutex> lk(v->mx);
        uint64_t i = 0;
        while (i < want && !v->chunks.empty()) {
            Chunk &c = v->chunks.front();
            double t = startTime + static_cast<double>(sp->written + i) / sp->rate;   // the time of the sample to write
            double head = c.pts + static_cast<double>(c.at / 2) / sp->rate;
            size_t left = (c.s.size() - c.at) / 2;
            if (head > t + 0.03) {   // a gap before this chunk: silence up to it
                uint64_t gap = std::min<uint64_t>(want - i, static_cast<uint64_t>((head - t) * sp->rate));
                i += std::max<uint64_t>(gap, 1);
                continue;
            }
            if (head < t - 0.03) {   // late: dropped up to now
                size_t drop = std::min<size_t>(left, static_cast<size_t>((t - head) * sp->rate) + 1);
                c.at += drop * 2;
                v->audioQueued -= static_cast<double>(drop) / sp->rate;
            } else {
                size_t take = static_cast<size_t>(std::min<uint64_t>(left, want - i));
                memcpy(&out[static_cast<size_t>(i) * 2], &c.s[c.at], take * 4);
                c.at += take * 2;
                i += take;
                v->audioQueued -= static_cast<double>(take) / sp->rate;
            }
            if (c.at >= c.s.size()) v->chunks.pop_front();
        }
        v->cv.notify_all();
    }
    void *p1 = nullptr, *p2 = nullptr;
    DWORD n1 = 0, n2 = 0, bytes = static_cast<DWORD>(want * 4);
    if (SUCCEEDED(sp->buf->Lock(sp->wpos, bytes, &p1, &n1, &p2, &n2, 0))) {
        memcpy(p1, out.data(), n1);
        if (p2) memcpy(p2, reinterpret_cast<uint8_t *>(out.data()) + n1, n2);
        sp->buf->Unlock(p1, n1, p2, n2);
        sp->wpos = (sp->wpos + bytes) % sp->size;
        sp->written += want;
    }
    if (!sp->started) {
        sp->buf->Play(0, 0, DSBPLAY_LOOPING);
        sp->started = true;
    }
}

void SpeakerClose(Speaker *sp) {
    if (sp->buf) { sp->buf->Stop(); sp->buf->Release(); }
    *sp = Speaker{};
}

double Now() {
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

// How the video's pictures were decoded and reached the screen ("D3D11VA, a shared texture", "software"...)
std::string PathOf(const Video *v) {
    if (v->api == videohw::kSoftware || !v->hwSeen) return "software";
    if (v->gpuFailed || !v->gpu) return std::string(videohw::ApiName(v->api)) + ", turned for the screen by the CPU";
    return videohw::Path(v->gpu);
}

json Describe(const Video *v) {
    json j = {{"name", AcpToUtf8(v->name.c_str())}, {"file", v->file}, {"from", v->from}, {"video", v->desc}, {"audio", v->adesc},
              {"width", v->w}, {"height", v->h}, {"duration", v->duration}, {"decode", videohw::ApiName(v->api)}};
    if (!v->hwWhy.empty()) j["decode_why"] = v->hwWhy;
    return j;
}

// The whole video, as the game's own loop plays it (to its end, a key or video.skip).
void Play(Video *v) {
    Record rec{AcpToUtf8(v->name.c_str()), v->file, v->from, v->desc, v->adesc, "played"};
    Screen scr;
    std::string why;
    void *ig = Igraph();
    if (!ig || !ScreenOpen(&scr, v, &why)) {
        VWARN("'%s': not shown: %s", rec.name.c_str(), ig ? why.c_str() : "no IGraph");
        ScreenClose(&scr);
        rec.result = "not shown: " + why;
        Remember(rec);
        return;
    }
    // the GPU's slots (or its copy), at the picture's size on the screen
    if (v->api != videohw::kSoftware) {
        std::string whyGpu;
        v->gpu = videohw::Begin(v->api, scr.d9, v->tw, v->th, kSlots, &whyGpu);
        if (!v->gpu) {
            VWARN("'%s': %s decodes, but its pictures go through the CPU: %s", rec.name.c_str(), videohw::ApiName(v->api), whyGpu.c_str());
            v->gpuFailed = true;
        } else if (videohw::Shared(v->gpu)) {
            for (int i = 0; i < kSlots; i++) v->freeSlots.push_back(i);
        }
    }
    size_t picBytes = static_cast<size_t>(v->tw) * v->th * 4;
    v->maxPics = videohw::Shared(v->gpu) ? kSlots - 1 : std::clamp<size_t>((48u << 20) / std::max<size_t>(picBytes, 1), 2, 6);
    Speaker sp;
    bool sound = v->rate && SpeakerOpen(&sp, v->rate);
    if (!sound) v->rate = 0;   // (no sound decoded: nothing would take it)
    g_ffLogBudget = 20;
    v->worker = std::thread(Decode, v);
    g_playing = v;
    g_skipAsked = false;
    json ev = Describe(v);
    EventsRecord("VIDEO_STARTED", ev.dump(-1, ' ', false, json::error_handler_t::replace));
    VLOG("'%s': playing %s (%s): %s%s%s; decoded by %s%s%s, shown at %dx%d", rec.name.c_str(), v->file.c_str(), v->from.c_str(),
         v->desc.c_str(), v->adesc.empty() ? "" : "; ", v->adesc.c_str(), videohw::ApiName(v->api), v->hwWhy.empty() ? "" : " (",
         v->hwWhy.empty() ? "" : (v->hwWhy + ")").c_str(), v->tw, v->th);

    // the first picture (up to 5 s for it), then the clock starts with the sound
    double begin = Now(), start = 0;
    bool first = true;
    int shown = 0, dropped = 0;
    double lastShown = -1;
    bool skipped = false;
    std::deque<int> retiring;   // GPU slots the game's device may still be drawing from
    auto freeSlot = [&](int slot) {
        std::lock_guard<std::mutex> lk(v->mx);
        v->freeSlots.push_back(slot);
        v->cv.notify_all();
    };
    for (;;) {
        // what the game's loop does every turn: its window's messages, the keyboard, any key ends the video
        auto turn = [&] {
            reinterpret_cast<IgFn>(Vt(ig, 0x2C))(ig);
            reinterpret_cast<IgFn>(Ls3dBase() + L().keyboardUpdate)(ig);
            if (reinterpret_cast<IgKeyFn>(Ls3dBase() + L().readKey)(ig)) skipped = true;
        };
        if (Guarded(turn)) { why = "the game's input faulted"; break; }
        BridgeFrame();   // (screenshot, video.state, video.skip meanwhile)
        if (skipped || g_skipAsked) { skipped = true; break; }
        while (!retiring.empty() && videohw::Retired(v->gpu, retiring.front())) {
            freeSlot(retiring.front());
            retiring.pop_front();
        }
        if (first) {
            std::lock_guard<std::mutex> lk(v->mx);
            if (v->pics.empty() && !v->ended && Now() - begin < 5) { /* waiting for the first picture */ }
            else { first = false; start = Now(); v->clockStart = start; }
        }
        if (first) { WaitMs(2); continue; }
        double clock = Now() - start;
        if (sound) SpeakerFill(&sp, v, 0.0);
        // the newest picture that is due; older ones are dropped
        Pic pick;
        bool have = false, done = false;
        double next = clock + 0.005;
        std::vector<int> passed;   // GPU slots of pictures dropped (never drawn)
        {
            std::lock_guard<std::mutex> lk(v->mx);
            while (!v->pics.empty() && v->pics.front().pts <= clock) {
                if (have) {
                    dropped++;
                    if (pick.slot >= 0) passed.push_back(pick.slot);
                    else v->spare.push_back(std::move(pick.bgra));
                }
                pick = std::move(v->pics.front());
                v->pics.pop_front();
                have = true;
            }
            for (int slot : passed) v->freeSlots.push_back(slot);
            if (!v->pics.empty()) next = v->pics.front().pts;
            if (have) v->cv.notify_all();
            double end = std::max(v->lastPts + 0.04, sound ? v->audioEnd : 0.0);
            done = v->ended && v->pics.empty() && clock >= end;
            if (v->ended && v->pics.empty() && !v->error.empty()) { why = v->error; done = true; }
        }
        if (have) {
            double t0 = Now();
            IDirect3DTexture9 *tex = scr.tex;
            float u1 = scr.u1, v1 = scr.v1;
            if (pick.slot >= 0) {
                tex = videohw::SlotTexture(v->gpu, pick.slot);
                u1 = v1 = 1;
            } else {
                if (!Upload(&scr, pick, v->tw, v->th, &why)) break;
                tex = scr.tex;
                v->uploads++;
            }
            double t1 = Now();
            auto frame = [&] {
                reinterpret_cast<IgClearFn>(Vt(ig, 0x30))(ig, 0, 1.0f, 3);
                reinterpret_cast<IgFn>(Vt(ig, 0x34))(ig);
                Draw(&scr, v->aspect, tex, u1, v1);
                if (pick.slot >= 0) videohw::Drawn(v->gpu, pick.slot);
                reinterpret_cast<IgFn>(Vt(ig, 0x38))(ig);
                reinterpret_cast<IgFn>(Vt(ig, 0x3C))(ig);
            };
            if (DWORD ex = Guarded(frame)) {
                char b[64];
                snprintf(b, sizeof b, "drawing faulted (0x%08lX)", ex);
                why = b;
                break;
            }
            double t2 = Now();
            if (pick.slot < 0) {
                v->tUpload += (t1 - t0) * 1000.0;
                v->maxUpload = std::max(v->maxUpload, (t1 - t0) * 1000.0);
            }
            v->tPresent += (t2 - t1) * 1000.0;
            v->maxPresent = std::max(v->maxPresent, (t2 - t1) * 1000.0);
            shown++;
            lastShown = pick.pts;
            if (pick.slot >= 0) retiring.push_back(pick.slot);
            else {
                std::lock_guard<std::mutex> lk(v->mx);
                v->spare.push_back(std::move(pick.bgra));
            }
        }
        if (done) break;
        if (clock > v->duration + 10 && v->duration > 0) { why = "it ran past its length"; break; }
        WaitMs(std::min(4.0, (next - (Now() - start)) * 1000.0));
    }
    double ms = (Now() - begin) * 1000.0;
    g_playing = nullptr;
    SpeakerClose(&sp);
    v->stop = true;
    v->cv.notify_all();
    if (v->worker.joinable()) v->worker.join();
    std::string path = PathOf(v);
    videohw::End(v->gpu);   // (after the thread: it converts into the slots)
    v->gpu = nullptr;
    ScreenClose(&scr);
    rec.frames = shown;
    rec.dropped = dropped + v->late;
    {   // per picture: the decoder's and the conversion's (every picture decoded), the upload's (the CPU's) and the frame's
        auto avg = [](double sum, int n) { return n ? sum / n : 0.0; };
        rec.timing = {{"decoded", v->decoded}, {"late", v->late}, {"decode_ms", avg(v->tDecode, v->decoded)},
                      {"convert_ms", avg(v->tConvert, v->decoded)}, {"convert_max_ms", v->maxConvert}, {"uploads", v->uploads},
                      {"upload_ms", avg(v->tUpload, v->uploads)}, {"upload_max_ms", v->maxUpload},
                      {"present_ms", avg(v->tPresent, shown)}, {"present_max_ms", v->maxPresent}};
    }
    rec.path = path;
    rec.ms = ms;
    rec.skipped = skipped;
    if (!why.empty()) rec.result = why;
    else if (skipped) rec.result = "skipped";
    Remember(rec);
    json end = Describe(v);
    end["frames"] = shown;
    end["dropped"] = rec.dropped;
    end["ms"] = static_cast<int>(ms);
    end["skipped"] = skipped;
    end["result"] = rec.result;
    end["path"] = path;
    end["timing"] = rec.timing;
    EventsRecord("VIDEO_ENDED", end.dump(-1, ' ', false, json::error_handler_t::replace));
    VLOG("'%s': %s after %.1f s, %d pictures shown, %d dropped (%d of them late, left out by the decoding thread; the last at "
         "%.2f s), %s; a picture: decode %.2f ms, convert %.2f, upload %.2f, frame %.2f", rec.name.c_str(), rec.result.c_str(),
         ms / 1000.0, shown, rec.dropped, v->late, lastShown, path.c_str(), rec.timing["decode_ms"].get<double>(),
         rec.timing["convert_ms"].get<double>(), rec.timing["upload_ms"].get<double>(), rec.timing["present_ms"].get<double>());
}

// The loader's video for a name, or null (and the reasons) when the game's own player is to get it.
Video *Find(const std::string &name, std::vector<std::string> *refusals) {
    if (!FfLoad()) {
        refusals->push_back("FFmpeg is not there: " + g_ffWhy);
        return nullptr;
    }
    IDirect3DDevice9 *d9 = Device9();
    if (!d9) {
        refusals->push_back("the game's device is not Direct3D 9 underneath (dxwrapper's d3d8to9)");
        return nullptr;
    }
    d9->Release();
    for (const Candidate &c : Candidates(name)) {
        std::string why;
        if (Video *v = Prepare(name, c, &why)) return v;
        refusals->push_back(c.path + " (" + c.from + "): " + why);
    }
    if (refusals->empty()) refusals->push_back("no file of that name");
    return nullptr;
}

#endif   // MML_NO_FFMPEG

// ---- the hooks
using ShowOpenFn = int(__stdcall *)(void *show, const char *name, uint32_t flags);
using ShowFn = int(__stdcall *)(void *show);
using CdFindFn = bool(__fastcall *)(const char *name, char *path);
ShowOpenFn oShowOpen;
ShowFn oShowClose, oShowUntilKey;
CdFindFn oCdFind;

int __stdcall ShowOpenHook(void *show, const char *name, uint32_t flags) {
#ifndef MML_NO_FFMPEG
    if (IsVideoName(name)) {
        auto it = g_videos.find(show);
        if (it != g_videos.end()) { Free(it->second); g_videos.erase(it); }
        std::vector<std::string> refusals;
        if (Video *v = Find(name, &refusals)) {
            for (const std::string &r : refusals) VWARN("'%s': passed over %s", name, r.c_str());
            oShowClose(show);   // (as the game's Open does first)
            g_videos[show] = v;
            return 0;
        }
        std::string all;
        for (const std::string &r : refusals) all += (all.empty() ? "" : "; ") + r;
        VWARN("'%s': the game's own player gets it: %s", name, all.c_str());
        Record rec;
        rec.name = AcpToUtf8(name);
        rec.result = "the game's own player: " + all;
        Remember(rec);
        json ev = {{"name", rec.name}, {"why", all}};
        EventsRecord("VIDEO_REFUSED", ev.dump(-1, ' ', false, json::error_handler_t::replace));
    }
#endif
    return oShowOpen(show, name, flags);
}

int __stdcall ShowCloseHook(void *show) {
#ifndef MML_NO_FFMPEG
    auto it = g_videos.find(show);
    if (it != g_videos.end()) { Free(it->second); g_videos.erase(it); }
#endif
    return oShowClose(show);
}

int __stdcall ShowUntilKeyHook(void *show) {
#ifndef MML_NO_FFMPEG
    auto it = g_videos.find(show);
    if (it != g_videos.end()) {
        if (g_playing) return 0;   // (one at a time)
        Play(it->second);
        return 0;
    }
#endif
    return oShowUntilKey(show);
}

// The game's search for a video's file: "found" for a name the loader can play from elsewhere (a mod's MP4 where
// the game's folder has none), the name given back as the path to open (the Open hook finds the file again).
bool __fastcall CdFindHook(const char *name, char *path) {
    bool found = oCdFind(name, path);
#ifndef MML_NO_FFMPEG
    if (!found && IsVideoName(name) && FfLoad() && !Candidates(name).empty()) {
        strcpy(path, name);
        return true;
    }
#endif
    return found;
}

bool g_hooked;

} // namespace

void VideoInit() {
    if (!game::g_exe || !game::g_ls3d || !L().showOpen || !E().cdFind) return;
    uintptr_t ls = Ls3dBase();
    std::string err;
    // (innermost: the start's own hooks, which skip the logos, stay in front of the loader's player)
    bool ok = HookAddInner(ls + L().showOpen, reinterpret_cast<void *>(&ShowOpenHook), reinterpret_cast<void **>(&oShowOpen), &err) &&
              HookAddInner(ls + L().showClose, reinterpret_cast<void *>(&ShowCloseHook), reinterpret_cast<void **>(&oShowClose), &err) &&
              HookAddInner(ls + L().showRenderUntilKey, reinterpret_cast<void *>(&ShowUntilKeyHook), reinterpret_cast<void **>(&oShowUntilKey), &err) &&
              HookAddInner(E().cdFind, reinterpret_cast<void *>(&CdFindHook), reinterpret_cast<void **>(&oCdFind), &err);
    if (!ok) { HWARN("video: cannot hook the game's videos (%s): its own players play them", err.c_str()); return; }
    g_hooked = true;
#ifdef MML_NO_FFMPEG
    HLOG("video: built without FFmpeg: the game's own players play the videos");
#endif
}

// Each frame: video.play's video, played as the game plays one (IGraph's CreateIShow, Open, SetRect, Play, the
// loop, Release), from the frame's own call (never inside a bridge command).
void VideoFrame() {
    if (g_pendingPlay.empty()) return;
    std::string name = g_pendingPlay;
    g_pendingPlay.clear();
#ifndef MML_NO_FFMPEG
    void *ig = Igraph();
    if (!ig) return;
    g_forceDecode = g_pendingDecode;   // (for the Open below; the game's own videos choose for themselves)
    auto f = [&] {
        void *show = reinterpret_cast<void *(__stdcall *)(void *)>(Vt(ig, 0x00))(ig);
        if (!show) return;
        if (reinterpret_cast<ShowOpenFn>(Vt(show, 0x0C))(show, name.c_str(), 0) == 0) {
            reinterpret_cast<int(__stdcall *)(void *, void *)>(Vt(show, 0x20))(show, nullptr);
            reinterpret_cast<ShowFn>(Vt(show, 0x14))(show);
            reinterpret_cast<ShowFn>(Ls3dBase() + L().showRenderUntilKey)(show);
        }
        reinterpret_cast<ShowFn>(Vt(show, 0x04))(show);
    };
    if (DWORD ex = Guarded(f)) VWARN("video.play '%s' faulted (0x%08lX)", name.c_str(), ex);
    g_forceDecode.clear();
#endif
}

bool VideoMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method.compare(0, 6, "video.") != 0) return false;
    *ok = false;
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::vector<std::string> order;
    for (const char *e : kExts) order.push_back(e);
    if (method == "video.state") {
        json r = {{"hooked", g_hooked}, {"extensions", order}};
#ifdef MML_NO_FFMPEG
        r["ffmpeg"] = {{"built", false}};
#else
        r["ffmpeg"] = {{"built", true}, {"dir", MmlDir() + "ffmpeg\\"},
                       {"state", g_ff == FfState::Ok ? "loaded" : g_ff == FfState::Missing ? "missing" : "not yet loaded (at the first video)"}};
        if (g_ff == FfState::Ok) { r["ffmpeg"]["version"] = g_ffVersion; r["ffmpeg"]["license"] = g_ffLicense; }
        if (g_ff == FfState::Missing) r["ffmpeg"]["why"] = g_ffWhy;
        r["playing"] = g_playing ? Describe(g_playing) : json(nullptr);
#endif
        json last = json::array();
        for (const Record &x : g_records)
            last.push_back({{"name", x.name}, {"file", x.file}, {"from", x.from}, {"video", x.video}, {"audio", x.audio},
                            {"result", x.result}, {"path", x.path}, {"frames", x.frames}, {"dropped", x.dropped}, {"ms", static_cast<int>(x.ms)},
                            {"skipped", x.skipped}, {"timing", x.timing}});
        r["last"] = last;
        *reply = dump(r);
        *ok = true;
        return true;
    }
    if (method == "video.skip") {
        bool playing = false;
#ifndef MML_NO_FFMPEG
        playing = g_playing != nullptr;
#endif
        g_skipAsked = playing;
        *reply = dump({{"skipped", playing}});
        *ok = true;
        return true;
    }
    std::string name = p.value("name", "");
    if (name.empty()) { *reply = "name: the video's name as the game asks for it (\"logo1.avi\", \"intro\")"; return true; }
    if (name.find('.') == std::string::npos) name += ".avi";   // (a bare name, as 1.2's PlayVideo first asks)
    std::string acp = AcpOf(name);
    std::string decode = p.value("decode", "");
    if (!decode.empty() && decode != "auto" && decode != "d3d11va" && decode != "dxva2" && decode != "software") {
        *reply = "decode: auto, d3d11va, dxva2 or software";
        return true;
    }
    if (decode == "auto") decode.clear();
    if (method == "video.find") {
        json r = {{"name", name}, {"extensions", order}};
        json list = json::array();
        for (const Candidate &c : Candidates(acp)) {
            json e = {{"file", c.path}, {"from", c.from}};
#ifndef MML_NO_FFMPEG
            std::string why;
            g_forceDecode = decode;
            Video *v = FfLoad() ? Prepare(acp, c, &why) : nullptr;
            g_forceDecode.clear();
            if (!FfLoad()) e["playable"] = false, e["why"] = "FFmpeg is not there: " + g_ffWhy;
            else if (v) {
                e["playable"] = true;
                e["decode"] = videohw::ApiName(v->api);
                if (!v->hwWhy.empty()) e["decode_why"] = v->hwWhy;
                e["video"] = v->desc;
                e["audio"] = v->adesc;
                e["width"] = v->w;
                e["height"] = v->h;
                e["duration"] = v->duration;
                Free(v);
            } else {
                e["playable"] = false;
                e["why"] = why;
            }
#endif
            list.push_back(e);
        }
        r["candidates"] = list;
        *reply = dump(r);
        *ok = true;
        return true;
    }
    if (method == "video.play") {
        if (!g_hooked) { *reply = "the game's videos are not hooked (no game, or not a known build)"; return true; }
        if (g_playing) { *reply = "a video is playing"; return true; }
        g_pendingPlay = acp;
        g_pendingDecode = decode;
        *reply = dump({{"queued", name}, {"note", "played at the next frame; wait for VIDEO_ENDED (or VIDEO_REFUSED)"}});
        *ok = true;
        return true;
    }
    *reply = "unknown video method";
    return true;
}

} // namespace host
