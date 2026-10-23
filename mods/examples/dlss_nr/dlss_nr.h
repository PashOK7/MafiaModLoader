// dlss_nr: the records its two halves share (dlss_nr.cpp, the 32-bit half; dlss_nr64.cpp, the 64-bit half). Fixed-size
// fields only, no pointers: the same layout in both processes (the static_asserts are checked by both).
//
// The frames go round a ring of kSlots slots, each its own set of textures, so the halves work at once: while the
// 64-bit half's GPU runs DLSS on frame N, the game draws frame N + 1. Two shared fences (side.h fence_create, mml.h
// gfx_shared_fence) order the GPUs without the CPU: the pass signals "ready" with N once N's inputs are queued, the
// 64-bit half's queue waits for it and signals "done" with N once N's picture is in its slot's "color_out"; the pass
// waits (on its GPU) for "done" before it reads that picture or writes that slot again.
//
// "view"   (32 -> 64, in the graphics pass): frame N, its slot, its sizes, NVIDIA's settings. `prepared` 0: a view
//          that only asks (the textures at a size, the render size of a mode): nothing was signalled for it.
// "done"   (64 -> 32): the last view taken: whether its work is queued (`signalled`: "done" will reach its frame,
//          whatever came of it) and whether its picture will be whole (`ok`); the render size the half wants.
// "status" (64 -> 32, when it changes): what NGX (the driver and NVIDIA's DLLs) said: can this GPU run them, why not.
//
// The textures of slot s (the 64-bit half's, opened by the bridge's Direct3D 11 device), the inputs at the render
// size, the picture at the frame's:
// "color_in<s>"  R16G16B16A16_FLOAT  the frame where the world is drawn and the HUD is not yet, in the display's gamma
//                                    (0..1, as the game drew it: DLSS-NR's model and DLSS's LDR mode both want gamma
//                                    values, as RTX Remix gives them), sampled at the render pixels' jittered points
// "depth_in<s>"  R32_FLOAT           its depth as the game's projection makes it (0 near .. 1 far; the sky 1)
// "mvec_in<s>"   R16G16B16A16_FLOAT  rg: each pixel's motion vector in render pixels, from where it is to where it
//                                    was the frame before (DLSS's convention), without the jitter
// "mask_in<s>"   R8G8B8A8_UNORM      DLSS-NR's control mask: 1 where it may change the picture fully, 0 not at all
// "color_out<s>" R16G16B16A16_FLOAT  the picture at the frame's size, in the display's gamma
#pragma once
#include <stdint.h>
#include <stdio.h>

namespace dnr {

constexpr uint32_t kSlots = 2;
enum : uint32_t { kColorIn, kDepthIn, kMvecIn, kMaskIn, kColorOut, kPerSlot };
static const char *const kKindNames[kPerSlot] = {"color_in", "depth_in", "mvec_in", "mask_in", "color_out"};
static const uint32_t kTexFormats[kPerSlot] = {10 /* R16G16B16A16_FLOAT */, 41 /* R32_FLOAT */, 10, 28 /* R8G8B8A8_UNORM */, 10};
inline void TexName(char *b, size_t n, uint32_t slot, uint32_t kind) { snprintf(b, n, "%s%u", kKindNames[kind], slot); }
static const char *const kFenceReady = "ready";   // signalled by the pass's device: frame N's inputs are written
static const char *const kFenceDone = "done";     // signalled by the 64-bit half's queue: frame N's work is over

enum : uint32_t { kModeNr = 0, kModeCopy = 1 };   // View::mode: DLSS-NR; the frame copied in its place (NR bypassed)

enum : uint32_t {   // View::upscale: DLSS Super Resolution's mode (the render size: NGX's optimal one for it)
    kUpOff = 0,           // no DLSS SR: the inputs at the frame's size
    kUpDlaa = 1,          // DLSS SR at the frame's size: its anti-aliasing alone
    kUpQuality = 2,
    kUpBalanced = 3,
    kUpPerformance = 4,
    kUpUltra = 5,         // Ultra Performance
    kUpModes
};

struct View {
    uint64_t frame;
    int64_t askQpc;             // QueryPerformanceCounter when asked (the round trip)
    uint32_t outW, outH;        // the frame's size
    uint32_t renderW, renderH;  // the inputs' size (prepared views)
    uint32_t prepared;          // 1: slot's inputs written, "ready" signalled with frame: the half signals "done" with it
    uint32_t slot;
    uint32_t inGen, outGen;     // the generations of color_in<slot> and color_out<slot> the pass used
    uint32_t mode;              // kModeNr, kModeCopy
    uint32_t nr;                // (kModeNr: 1)
    uint32_t upscale;           // kUp*
    uint32_t reset;             // 1: the histories dropped (the first frame, a camera cut, a new size or mode)
    uint32_t preset;            // NVSDK_NGX_DLSSNR_Hint_Render_Preset at the NR feature's creation (0 default, 1.. A..)
    uint32_t style;             // InStyle: NVIDIA's NR model (0 A, 1 B, 2 C)
    uint32_t autoMask;          // InUseAutoMask: DLSS-NR's own mask of the people (skin) combined with ours
    uint32_t srPreset;          // DLSS SR's render preset hint (0 the DLL's default)
    float intensity;            // InIntensity 0..1
    float tone;                 // InLocalToneStrength 0..1
    float structure;            // InLocalStructureStrength 0..1
    float skin;                 // InSkinStructureStrength 0..1 (with autoMask)
    float jitter[2];            // the inputs' sub-pixel jitter as DLSS takes it (InJitterOffset: render pixels, -0.5..0.5)
};

struct Done {
    uint64_t frame;             // the view taken last
    int64_t askQpc, doneQpc;    // its ask, and when its work was queued
    uint32_t ok;                // 1: color_out<slot> holds its picture once "done" reaches frame
    uint32_t signalled;         // 1: "done" will reach frame (a prepared view's work queued, whatever its outcome)
    uint32_t slot;
    uint32_t outGen;            // the color_out<slot> written
    uint32_t renderW, renderH;  // the render size the half wants for this view's frame size and upscale (0: none yet)
    uint32_t forW, forH;        // ... for this frame size
    uint32_t forUpscale;        // ... and this upscale (what it was asked for: SR may be refused)
    uint32_t mode;
    float workMs;               // the 64-bit half's CPU time on it (recording and queueing)
    float gpuMs;                // the GPU's time of the last frame finished (timestamps): the whole
    float gpuNrMs, gpuSrMs;     // ... DLSS-NR's and DLSS SR's parts
    char error[160];
};

// ---- frame generation (DLSS-FG, an RTX 40 or newer; MFG up to 4x on an RTX 50): at the frame's showing (mml.h
// gfx_show) the pass draws the back buffer (the HUD on it) into "fg_color", signals "fgready" F and writes "fgview";
// the 64-bit half queues DLSS-FG on it (the depth and motion vectors of the DLSS frame whose picture is shown: its
// slot's "depth_in" and "mvec_in", as DLSS SR had them), its pictures into "fg_out<i>", signals "fgdone" F and answers
// in "fgdone"; the pass's GPU waits for it and draws the pictures into the bridge's targets, shown before the frame.
constexpr uint32_t kFgMax = 3;   // the generated frames between two (MML_GFX_SHOW_MAX)
static const char *const kFgColor = "fg_color";   // R8G8B8A8_UNORM, the back buffer's size
static const char *const kFgOut = "fg_out";       // fg_out0..2: R8G8B8A8_UNORM, the back buffer's size
static const char *const kFenceFgReady = "fgready";
static const char *const kFenceFgDone = "fgdone";
static const uint32_t kFgFormat = 28;              // R8G8B8A8_UNORM

struct FgView {
    uint64_t frame;             // F: the showing's number ("fgready" signalled with it when prepared)
    uint64_t picture;           // the DLSS frame whose picture the back buffer shows
    int64_t askQpc;
    uint32_t outW, outH;        // the back buffer
    uint32_t renderW, renderH;  // that frame's inputs (its slot's depth and motion vectors)
    uint32_t slot;
    uint32_t count;             // the frames to generate (1: 2x .. 3: 4x)
    uint32_t prepared;          // 1: fg_color written and "fgready" signalled: the half signals "fgdone" with frame
    uint32_t reset;             // 1: the frames before are unrelated (a cut, a new size)
    uint32_t colorGen;          // fg_color's generation the pass drew into
    uint32_t outGen[kFgMax];    // fg_out<i>'s generations it will read
    uint32_t inGen;             // depth_in<slot>'s generation (the slot's set)
    float clipToPrevClip[16], prevClipToClip[16], viewToClip[16], clipToView[16];   // row-major, row vectors (Direct3D's)
    float camPos[4], camUp[4], camRight[4], camFwd[4];
    float camNear, camFar, camFov, camAspect;
    uint32_t pad;
};

struct FgDone {
    uint64_t frame;             // the view taken last
    uint32_t ok;                // 1: fg_out0..count-1 hold its pictures once "fgdone" reaches frame
    uint32_t signalled;         // 1: "fgdone" will reach frame
    uint32_t count;
    uint32_t outGen[kFgMax];
    uint32_t outW, outH;        // the size the half wants fg_color and fg_out at (0: none yet)
    float workMs, gpuMs;
    uint32_t pad;
    char error[124];
};

enum : uint32_t {   // Status::stage (DLSS-NR's)
    kStarting = 0,      // waiting for the game's adapter (the graphics bridge's first frame)
    kNoVulkan = 1,      // no Vulkan device on that adapter (or vulkan-1.dll)
    kNgxFailed = 2,     // NGX did not start (the driver)
    kNoDll = 3,         // nvngx_dlssnr.dll not found where it is looked for
    kOldDriver = 4,     // the driver is older than DLSS-NR needs
    kUnsupported = 5,   // the GPU cannot run DLSS-NR (it needs an RTX 50: Blackwell)
    kReady = 6,         // DLSS-NR can run here
    kFailed = 7,        // it could, but making or running the feature failed (message)
    kOtherGpu = 8,      // the game draws on a GPU that is not NVIDIA's (a laptop's or a CPU's own graphics: message)
};

struct Status {
    uint32_t stage;
    uint32_t ngxResult;         // NVSDK_NGX_Result of the NGX init
    uint32_t supportBits;       // NVSDK_NGX_FeatureRequirement::FeatureSupported (0 supported; 2 driver, 4 adapter, 8 OS)
    uint32_t minArch;           // its MinHWArchitecture (NV_GPU_ARCHITECTURE_ID: 0x1B0 Blackwell)
    uint32_t available;         // DLSSNR.Available
    uint32_t needsDriver;       // DLSSNR.NeedsUpdatedDriver
    uint32_t minDriver[2];      // DLSSNR.MinDriverVersionMajor, Minor
    uint32_t featureInit;       // DLSSNR.FeatureInitResult
    uint32_t driver[2];         // the GPU's driver (major, minor)
    uint32_t srAvailable;       // 1: DLSS Super Resolution runs here (an RTX 20 or newer, nvngx_dlss.dll)
    uint32_t fgAvailable;       // 1: DLSS Frame Generation runs here (an RTX 40 or newer, nvngx_dlssg.dll)
    uint32_t fgMaxFrames;       // the generated frames it makes between two at most (3: an RTX 50's 4x)
    char gpu[128];
    char dll[64];               // nvngx_dlssnr.dll's version ("310.9.16.0"), empty: not found
    char dllPath[260];          // where it was found (UTF-8)
    char message[256];          // a sentence for the player (DLSS-NR's)
    char srDll[32];             // nvngx_dlss.dll's version
    char fgDll[32];             // nvngx_dlssg.dll's version
    char srMessage[160];        // DLSS SR's and Frame Generation's sentence
};

static_assert(sizeof(View) == 104, "View's layout is shared by both halves");
static_assert(sizeof(Done) == 240, "Done's layout is shared by both halves");
static_assert(sizeof(Status) == 988, "Status's layout is shared by both halves");
static_assert(sizeof(FgView) == 416, "FgView's layout is shared by both halves");
static_assert(sizeof(FgDone) == 176, "FgDone's layout is shared by both halves");

} // namespace dnr
