// dlss_nr: the records its two halves share (dlss_nr.cpp, the 32-bit half; dlss_nr64.cpp, the 64-bit half). Fixed-size
// fields only, no pointers: the same layout in both processes (the static_asserts are checked by both).
//
// "view"   (32 -> 64, in the graphics pass, after the pass has drawn this frame into the four input textures): the
//          frame, its size, the inputs' generations, NVIDIA's settings; the 64-bit half runs DLSS 5 Neural Rendering on
//          them into "color_out".
// "done"   (64 -> 32): that frame's picture is whole in "color_out" (or why not).
// "status" (64 -> 32, when it changes): what NGX (the driver and nvngx_dlssnr.dll) said: can this GPU run it, and why
//          not.
//
// The textures (the 64-bit half's, made at the frame's size, opened by the bridge's Direct3D 11 device):
// "color_in"  R16G16B16A16_FLOAT  the frame where the world is drawn and the HUD is not yet, linear (0..1: the game's
//                                 colours decoded from their gamma: what DLSS-NR takes, an SDR picture in linear terms)
// "depth_in"  R32_FLOAT           its depth as the game's projection makes it (0 near .. 1 far; the sky 1)
// "mvec_in"   R16G16B16A16_FLOAT  rg: each pixel's motion vector in pixels, from where it is to where it was the frame
//                                 before (DLSS's convention); the camera's motion (the depth reprojected)
// "mask_in"   R8G8B8A8_UNORM      DLSS-NR's control mask: 1 where it may change the picture fully, 0 where not at all
// "color_out" R16G16B16A16_FLOAT  DLSS-NR's picture, linear
#pragma once
#include <stdint.h>

namespace dnr {

enum : uint32_t { kColorIn, kDepthIn, kMvecIn, kMaskIn, kColorOut, kTextures };
static const char *const kTexNames[kTextures] = {"color_in", "depth_in", "mvec_in", "mask_in", "color_out"};
static const uint32_t kTexFormats[kTextures] = {10 /* R16G16B16A16_FLOAT */, 41 /* R32_FLOAT */, 10, 28 /* R8G8B8A8_UNORM */, 10};

enum : uint32_t { kModeNr = 0, kModeCopy = 1 };   // View::mode: DLSS-NR; the frame copied back as it came (the round trip tested)

struct View {
    uint64_t frame;
    int64_t askQpc;             // QueryPerformanceCounter when asked (the round trip)
    uint32_t width, height;
    uint32_t inGeneration[4];   // the input textures' generations the pass drew into
    uint32_t mode;              // kModeNr, kModeCopy
    uint32_t reset;             // 1: DLSS-NR's history dropped (the first frame, a camera cut, a new size or model)
    uint32_t preset;            // NVSDK_NGX_DLSSNR_Hint_Render_Preset at the feature's creation (0 default, 1.. A..)
    uint32_t style;             // InStyle: NVIDIA's model (0 A, 1 B, 2 C)
    uint32_t autoMask;          // InUseAutoMask: DLSS-NR's own mask of the people (skin) combined with ours
    float intensity;            // InIntensity 0..1
    float tone;                 // InLocalToneStrength 0..1
    float structure;            // InLocalStructureStrength 0..1
    float skin;                 // InSkinStructureStrength 0..1 (with autoMask)
    uint32_t pad;
};

struct Done {
    uint64_t frame;
    int64_t askQpc, doneQpc;
    uint32_t ok;                // 1: "color_out" holds this frame's picture
    uint32_t outGeneration;     // the "color_out" written
    uint32_t mode;              // the mode it was made in
    float workMs;               // the 64-bit half's time on it (the copies in, the evaluation, the copy out, waited)
    float gpuMs;                // its GPU time (timestamps)
    uint32_t pad;
    char error[160];
};

enum : uint32_t {   // Status::stage
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
    char gpu[128];
    char dll[64];               // nvngx_dlssnr.dll's version ("310.9.16.0"), empty: not found
    char dllPath[260];          // where it was found (UTF-8)
    char message[256];          // a sentence for the player
};

static_assert(sizeof(View) == 80, "View's layout is shared by both halves");
static_assert(sizeof(Done) == 208, "Done's layout is shared by both halves");
static_assert(sizeof(Status) == 752, "Status's layout is shared by both halves");

} // namespace dnr
