// dlss_nr's 64-bit half (docs/MODDING.md "64-bit halves"): NVIDIA DLSS over the game's own frame. DLSS 5 Neural
// Rendering ("3D-guided neural rendering", NGX feature DLSSNR, nvngx_dlssnr.dll) and DLSS Super Resolution (NGX feature
// SuperSampling, nvngx_dlss.dll: DLAA to Ultra Performance). NGX and its features exist only for 64 bits and DLSS-NR
// only for Vulkan, so they run here, in the sidecar, on a Vulkan device of its own on the adapter the game draws on
// (the sidecar's adapter_luid, matched to Vulkan's deviceLUID).
//
// The frame (dlss_nr.h): the 32-bit half draws frame N's inputs (colour, depth, motion vectors, a control mask) at the
// render size into its slot's shared textures (Direct3D 12, made here: texture_create; imported into Vulkan as D3D12
// resources), signals the shared fence "ready" with N and writes "view". The worker records N's work into the slot's
// own command buffer: the inputs copied into images of its own (DLSS reads and writes images made for it: storage,
// sampled; the shared ones are render targets), DLSS-NR at the render size, DLSS SR from the render size to the
// frame's, the picture copied into the slot's "color_out"; it queues it waiting for "ready" N and signalling "done" N
// (the fences imported as timeline semaphores), answers in "done" at once and goes on: the CPU waits for nothing, so
// frame N's DLSS runs while the game draws N + 1. Every prepared view gets its "done" signal, whatever came of it:
// the 32-bit half's GPU waits for it before it writes that slot again.
//
// Whether this GPU can run each feature is NGX's to say (the driver and the DLLs: DLSS-NR needs an RTX 50, Blackwell;
// DLSS SR an RTX 20 or newer). What NGX said is in "status" and `dlss_nr.side`. View::mode kModeCopy runs the same path
// with NR's place a copy (the round trip, and DLSS SR alone, on any GPU that has it).
//
// NVIDIA's DLLs are not part of this mod (NVIDIA's, under its license): they are looked for in the mod's folder
// (<game>\mml\mods\dlss_nr\), where the player puts them. NGX keeps its log in the mod's data folder (nvngx.log).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_defs_dlssnr.h>
#include <nvsdk_ngx_helpers_dlssnr_vk.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "json.hpp"
#include "mml/side.h"
#include "dlss_nr.h"

namespace {
const MmlSideHost *H;
MmlSideMod *M;
MmlSideChannel *g_viewCh, *g_doneCh, *g_statusCh;
double g_freqMs;

// The mod's own identity for NGX (an application with no id from NVIDIA: a project of a custom engine).
const char *kProjectId = "a4f1c7e2-6d3b-4f58-9b1e-2c7d5e8f0a91";

double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart / g_freqMs;
}

std::wstring Wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// ---- errors: kept for `side` and "done", logged on the mod thread
std::mutex g_errMx;
std::string g_lastError, g_toLog;
void Error(const std::string &e) {
    std::lock_guard<std::mutex> lk(g_errMx);
    g_lastError = e;
    g_toLog += (g_toLog.empty() ? "" : "\n") + e;
}
std::string LastError() {
    std::lock_guard<std::mutex> lk(g_errMx);
    return g_lastError;
}

// ---- what NGX said: written into "status" whenever it changes
std::mutex g_statusMx;
dnr::Status g_status{};
void PutStatus(uint32_t stage, const char *fmt, ...) {
    std::lock_guard<std::mutex> lk(g_statusMx);
    g_status.stage = stage;
    va_list a;
    va_start(a, fmt);
    vsnprintf(g_status.message, sizeof g_status.message, fmt, a);
    va_end(a);
    H->channel_write(g_statusCh, &g_status, sizeof g_status);
}
void PutSrMessage(const char *fmt, ...) {
    std::lock_guard<std::mutex> lk(g_statusMx);
    va_list a;
    va_start(a, fmt);
    vsnprintf(g_status.srMessage, sizeof g_status.srMessage, fmt, a);
    va_end(a);
    H->channel_write(g_statusCh, &g_status, sizeof g_status);
}

// ---- Vulkan, loaded at run time (vulkan-1.dll: the driver's loader)
HMODULE g_vkDll;
PFN_vkGetInstanceProcAddr gipa;
PFN_vkGetDeviceProcAddr gdpa;
#define VK_FUNCS(X)                                                                                                    \
    X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)            \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkEnumerateDeviceExtensionProperties) X(vkDestroyDevice) X(vkGetDeviceQueue)                    \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)                 \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence)                  \
    X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus) X(vkCreateImage) X(vkDestroyImage)                         \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView)      \
    X(vkDestroyImageView) X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkGetMemoryWin32HandlePropertiesKHR)             \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkGetQueryPoolResults)  \
    X(vkDeviceWaitIdle) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkImportSemaphoreWin32HandleKHR)                  \
    X(vkGetSemaphoreCounterValue)
struct Vk {
#define X(f) PFN_##f f = nullptr;
    VK_FUNCS(X)
#undef X
} vk;

VkInstance g_inst;
VkPhysicalDevice g_pd;
VkDevice g_dev;
VkQueue g_queue;
uint32_t g_family = UINT32_MAX;
VkCommandPool g_pool;
VkCommandBuffer g_initCmd;      // the features' creation (waited for)
VkFence g_initFence;
float g_tickNs = 1;
VkPhysicalDeviceMemoryProperties g_mem;

// a slot's own command buffer, its CPU fence (the buffer's reuse) and its timestamps
enum : uint32_t { kTsStart, kTsNr, kTsSr, kTsEnd, kTimestamps };
struct SlotWork {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool timer = VK_NULL_HANDLE;
    bool pending = false;       // queued, its timestamps not read yet
};
SlotWork g_slotWork[dnr::kSlots];

bool Check(VkResult r, const char *what) {
    if (r == VK_SUCCESS) return true;
    Error(std::string(what) + " failed (VkResult " + std::to_string(r) + ")");
    return false;
}

void LoadInstanceFuncs(VkInstance inst) {
#define X(f) if (!vk.f) vk.f = reinterpret_cast<PFN_##f>(gipa(inst, #f));
    VK_FUNCS(X)
#undef X
}

// ---- images: the shared textures imported, and the half's own ones DLSS works on
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    bool fresh = true;   // its layout still UNDEFINED (made GENERAL by its first barrier)
};
void Free(Image &i) {
    if (i.view) vk.vkDestroyImageView(g_dev, i.view, nullptr);
    if (i.image) vk.vkDestroyImage(g_dev, i.image, nullptr);
    if (i.memory) vk.vkFreeMemory(g_dev, i.memory, nullptr);
    i = Image{};
}

VkFormat VkFormatOf(uint32_t dxgi) {
    switch (dxgi) {
    case 10: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 41: return VK_FORMAT_R32_SFLOAT;
    case 28: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

int MemoryType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & want) == want) return int(i);
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
        if (bits & (1u << i)) return int(i);
    return -1;
}

bool MakeView(Image &i) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = i.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = i.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return Check(vk.vkCreateImageView(g_dev, &vi, nullptr, &i.view), "vkCreateImageView");
}

// A shared texture (its NT handle) as a Vulkan image: a dedicated allocation of the D3D12 resource.
bool Import(Image &i, HANDLE handle, uint32_t w, uint32_t h, VkFormat format) {
    Free(i);
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &ext;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vk.vkCreateImage(g_dev, &ci, nullptr, &i.image), "vkCreateImage (shared)")) return false;
    i.format = format, i.width = w, i.height = h;
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(g_dev, i.image, &req);
    VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    if (!Check(vk.vkGetMemoryWin32HandlePropertiesKHR(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, handle, &hp),
               "vkGetMemoryWin32HandlePropertiesKHR"))
        return Free(i), false;
    int type = MemoryType(req.memoryTypeBits & hp.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) return Error("no memory type for a shared texture"), Free(i), false;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = i.image;
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    imp.pNext = &ded;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    imp.handle = handle;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = uint32_t(type);
    if (!Check(vk.vkAllocateMemory(g_dev, &ai, nullptr, &i.memory), "vkAllocateMemory (import)") ||
        !Check(vk.vkBindImageMemory(g_dev, i.image, i.memory, 0), "vkBindImageMemory (import)"))
        return Free(i), false;
    return true;
}

// One of the half's own images: what DLSS reads (sampled) and writes (storage).
bool MakeLocal(Image &i, uint32_t w, uint32_t h, VkFormat format) {
    Free(i);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!Check(vk.vkCreateImage(g_dev, &ci, nullptr, &i.image), "vkCreateImage")) return false;
    i.format = format, i.width = w, i.height = h;
    VkMemoryRequirements req;
    vk.vkGetImageMemoryRequirements(g_dev, i.image, &req);
    int type = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = uint32_t(type < 0 ? 0 : type);
    if (!Check(vk.vkAllocateMemory(g_dev, &ai, nullptr, &i.memory), "vkAllocateMemory") ||
        !Check(vk.vkBindImageMemory(g_dev, i.image, i.memory, 0), "vkBindImageMemory") || !MakeView(i))
        return Free(i), false;
    return true;
}

// ---- the textures and the fences: made on the mod thread (texture_create, fence_create), imported by the worker
struct Shared {
    HANDLE handle = nullptr;
    uint32_t gen = 0;          // the sidecar's generation of it
    uint32_t importedGen = 0;  // the generation `img` holds
    Image img;
};
struct SharedFence {
    HANDLE handle = nullptr;
    uint32_t gen = 0, importedGen = 0;
    VkSemaphore sem = VK_NULL_HANDLE;
};
std::mutex g_texMx;            // the worker holds it for a frame's work; MakeTextures for its change
Shared g_tex[dnr::kSlots][dnr::kPerSlot];
SharedFence g_ready, g_done;
uint32_t g_texOutW, g_texOutH, g_texRW, g_texRH;   // the size the textures are at (the frame's, the render's)
struct Want { uint32_t outW, outH, rw, rh; };
std::mutex g_wantMx;
Want g_want{};                 // the size the worker wants them at (the mod thread makes them)

// the half's own images (one set: the queue runs the slots' work in order, each buffer's barriers wait for the last)
struct Locals {
    Image color, depth, mvec, mask;   // the inputs at the render size
    Image nr;                         // DLSS-NR's picture at the render size
    Image sr;                         // DLSS SR's picture at the frame's size
} L;

// ---- NGX
NVSDK_NGX_Parameter *g_caps, *g_params;
NVSDK_NGX_Handle *g_nrFeature, *g_srFeature;
uint32_t g_nrW, g_nrH, g_nrPreset = UINT32_MAX;
struct SrKey { uint32_t rw, rh, outW, outH, upscale, preset; } g_srKey{};
bool g_ngx, g_available, g_srAvailable;
std::atomic<bool> g_ready_{false}, g_stop{false}, g_broken{false};
bool g_tried;
std::thread g_worker;
uint64_t g_viewSeen;
std::atomic<uint64_t> g_evals{0}, g_srEvals{0}, g_copies{0}, g_failures{0}, g_signals{0};
std::atomic<float> g_workMs{0}, g_gpuMs{0}, g_gpuNrMs{0}, g_gpuSrMs{0};
std::string g_gpuName;

const char *NgxText(NVSDK_NGX_Result r) {
    switch (r) {
    case NVSDK_NGX_Result_Success: return "success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported: return "feature not supported";
    case NVSDK_NGX_Result_FAIL_PlatformError: return "platform error";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "feature already exists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound: return "feature not found";
    case NVSDK_NGX_Result_FAIL_InvalidParameter: return "invalid parameter";
    case NVSDK_NGX_Result_FAIL_NotInitialized: return "not initialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "unsupported input format";
    case NVSDK_NGX_Result_FAIL_OutOfDate: return "out of date (the driver)";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory: return "out of GPU memory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat: return "unsupported format";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "cannot write to the data path";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "unsupported parameter";
    case NVSDK_NGX_Result_FAIL_Denied: return "denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented: return "not implemented";
    default: return "failed";
    }
}

// A DLL's file version ("310.9.16.0"), empty when it is not there.
std::string FileVersion(const std::wstring &path) {
    DWORD z = 0, n = GetFileVersionInfoSizeW(path.c_str(), &z);
    if (!n) return "";
    std::vector<char> b(n);
    VS_FIXEDFILEINFO *fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, n, b.data()) || !VerQueryValueW(b.data(), L"\\", reinterpret_cast<void **>(&fi), &len) || !fi)
        return "";
    char v[64];
    snprintf(v, sizeof v, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
             LOWORD(fi->dwFileVersionLS));
    return v;
}

NVSDK_NGX_FeatureDiscoveryInfo Discovery(const NVSDK_NGX_FeatureCommonInfo *ci, const wchar_t *dataPath, NVSDK_NGX_Feature feature) {
    NVSDK_NGX_FeatureDiscoveryInfo di{};
    di.SDKVersion = NVSDK_NGX_Version_API;
    di.FeatureID = feature;
    di.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    di.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    di.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    di.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    di.ApplicationDataPath = dataPath;
    di.FeatureInfo = ci;
    return di;
}

std::wstring g_modDirW, g_dataDirW;
const wchar_t *g_paths[1];
NVSDK_NGX_FeatureCommonInfo g_common{};
const NVSDK_NGX_Feature kFeatures[] = {NVSDK_NGX_Feature_DLSSNR, NVSDK_NGX_Feature_SuperSampling};

// Vulkan on the game's adapter, with what NGX's features ask of the instance and the device, and NGX on it.
bool Start(const uint32_t luid[2]) {
    g_vkDll = LoadLibraryW(L"vulkan-1.dll");
    if (!g_vkDll) return PutStatus(dnr::kNoVulkan, "Vulkan is not installed (vulkan-1.dll: the NVIDIA driver installs it)"), false;
    gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(g_vkDll, "vkGetInstanceProcAddr"));
    if (!gipa) return PutStatus(dnr::kNoVulkan, "vulkan-1.dll has no vkGetInstanceProcAddr"), false;
    LoadInstanceFuncs(VK_NULL_HANDLE);

    std::string modDir = H->mod_dir(M);   // (it ends with a separator: taken off, NGX and the player see one path)
    while (!modDir.empty() && (modDir.back() == '\\' || modDir.back() == '/')) modDir.pop_back();
    g_modDirW = Wide(modDir.c_str());
    g_dataDirW = Wide(H->data_dir(M));
    CreateDirectoryW(g_dataDirW.c_str(), nullptr);
    g_paths[0] = g_modDirW.c_str();
    g_common = NVSDK_NGX_FeatureCommonInfo{};
    g_common.PathListInfo.Path = g_paths;
    g_common.PathListInfo.Length = 1;
    g_common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;   // (nvngx.log in the data folder)

    {   // the DLLs, before anything: their versions for the player
        std::string nr = FileVersion(g_modDirW + L"\\nvngx_dlssnr.dll"), sr = FileVersion(g_modDirW + L"\\nvngx_dlss.dll"),
                    fg = FileVersion(g_modDirW + L"\\nvngx_dlssg.dll");
        std::lock_guard<std::mutex> lk(g_statusMx);
        snprintf(g_status.dll, sizeof g_status.dll, "%s", nr.c_str());
        snprintf(g_status.dllPath, sizeof g_status.dllPath, "%s\\nvngx_dlssnr.dll", modDir.c_str());
        snprintf(g_status.srDll, sizeof g_status.srDll, "%s", sr.c_str());
        snprintf(g_status.fgDll, sizeof g_status.fgDll, "%s", fg.c_str());
    }

    // the instance: what every feature asks of it
    std::vector<std::string> iext;
    auto addUnique = [](std::vector<std::string> &v, const char *e) {
        if (std::find(v.begin(), v.end(), e) == v.end()) v.push_back(e);
    };
    for (NVSDK_NGX_Feature feat : kFeatures) {
        NVSDK_NGX_FeatureDiscoveryInfo di = Discovery(&g_common, g_dataDirW.c_str(), feat);
        uint32_t n = 0;
        VkExtensionProperties *props = nullptr;
        if (!NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&di, &n, &props)))
            for (uint32_t i = 0; i < n; i++) addUnique(iext, props[i].extensionName);
    }
    std::vector<const char *> iptr;
    for (auto &e : iext) iptr.push_back(e.c_str());
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MafiaModLoader dlss_nr";
    app.pEngineName = "LS3D";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(iptr.size());
    ici.ppEnabledExtensionNames = iptr.data();
    VkResult vr = vk.vkCreateInstance(&ici, nullptr, &g_inst);
    if (vr != VK_SUCCESS) return PutStatus(dnr::kNoVulkan, "no Vulkan driver for the GPU (vkCreateInstance %d)", int(vr)), false;
    LoadInstanceFuncs(g_inst);
    gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(g_inst, "vkGetDeviceProcAddr"));

    // the game's adapter by its LUID
    uint32_t count = 0;
    vk.vkEnumeratePhysicalDevices(g_inst, &count, nullptr);
    std::vector<VkPhysicalDevice> pds(count);
    vk.vkEnumeratePhysicalDevices(g_inst, &count, pds.data());
    VkPhysicalDeviceProperties pr{};
    for (VkPhysicalDevice p : pds) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &id;
        vk.vkGetPhysicalDeviceProperties2(p, &p2);
        if (id.deviceLUIDValid && !memcmp(id.deviceLUID, luid, 8)) { g_pd = p, pr = p2.properties; break; }
    }
    if (!g_pd) return PutStatus(dnr::kNoVulkan, "Vulkan does not see the GPU the game draws on"), false;
    g_gpuName = pr.deviceName;
    {
        std::lock_guard<std::mutex> lk(g_statusMx);
        snprintf(g_status.gpu, sizeof g_status.gpu, "%s", pr.deviceName);
        g_status.driver[0] = pr.driverVersion >> 22, g_status.driver[1] = (pr.driverVersion >> 14) & 0xFF;   // (NVIDIA's encoding)
    }
    // A game drawn on another vendor's GPU (a CPU's own graphics beside the GeForce: a player's game ran on "AMD
    // Radeon(TM) Graphics" so) cannot hand its frame to NGX: Windows picks the GPU, the player is told how.
    const uint32_t kNvidia = 0x10DE;
    if (pr.vendorID != kNvidia) {
        std::string nv;
        for (VkPhysicalDevice p : pds) {
            VkPhysicalDeviceProperties o{};
            vk.vkGetPhysicalDeviceProperties(p, &o);
            if (o.vendorID == kNvidia) { nv = o.deviceName; break; }
        }
        if (nv.empty())
            PutStatus(dnr::kOtherGpu, "the game draws on %s, and Vulkan sees no NVIDIA GPU: DLSS needs a GeForce RTX", pr.deviceName);
        else
            PutStatus(dnr::kOtherGpu, "the game draws on %s, not on the %s: in Windows' Settings > System > Display > Graphics give "
                      "Game.exe \"High performance\" (the NVIDIA GPU), and plug the monitor into the NVIDIA card", pr.deviceName, nv.c_str());
        return false;
    }
    g_tickNs = pr.limits.timestampPeriod;
    vk.vkGetPhysicalDeviceMemoryProperties(g_pd, &g_mem);

    // DLSS-NR's requirements of this GPU (NGX says before anything is made: the driver, the adapter, the OS)
    NVSDK_NGX_FeatureDiscoveryInfo nrDi = Discovery(&g_common, g_dataDirW.c_str(), NVSDK_NGX_Feature_DLSSNR);
    NVSDK_NGX_FeatureRequirement fr{};
    NVSDK_NGX_Result rr = NVSDK_NGX_VULKAN_GetFeatureRequirements(g_inst, g_pd, &nrDi, &fr);
    {
        std::lock_guard<std::mutex> lk(g_statusMx);
        g_status.supportBits = NVSDK_NGX_FAILED(rr) ? 0xFFFFFFFFu : uint32_t(fr.FeatureSupported);
        g_status.minArch = fr.MinHWArchitecture;
    }

    // the device: a queue that computes and copies, every feature's extensions, the D3D12 resources and fences imported
    uint32_t qn = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(g_pd, &qn, qf.data());
    for (uint32_t i = 0; i < qn; i++)
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { g_family = i; break; }
    if (g_family == UINT32_MAX) return PutStatus(dnr::kNoVulkan, "no graphics queue on the GPU"), false;
    uint32_t en = 0;
    vk.vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> have(en);
    vk.vkEnumerateDeviceExtensionProperties(g_pd, nullptr, &en, have.data());
    auto has = [&](const char *e) {
        for (auto &h : have)
            if (!strcmp(h.extensionName, e)) return true;
        return false;
    };
    std::vector<std::string> dext{VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
    for (NVSDK_NGX_Feature feat : kFeatures) {
        NVSDK_NGX_FeatureDiscoveryInfo di = Discovery(&g_common, g_dataDirW.c_str(), feat);
        uint32_t n = 0;
        VkExtensionProperties *props = nullptr;
        if (!NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(g_inst, g_pd, &di, &n, &props)))
            for (uint32_t i = 0; i < n; i++)
                if (has(props[i].extensionName)) addUnique(dext, props[i].extensionName);
    }
    std::vector<const char *> dptr;
    for (auto &e : dext) dptr.push_back(e.c_str());
    float prio = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = g_family, q.queueCount = 1, q.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;   // (NGX's VK_KHR_buffer_device_address)
    f12.timelineSemaphore = VK_TRUE;     // (the shared fences)
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = uint32_t(dptr.size()), dci.ppEnabledExtensionNames = dptr.data();
    vr = vk.vkCreateDevice(g_pd, &dci, nullptr, &g_dev);
    if (vr != VK_SUCCESS) return PutStatus(dnr::kNoVulkan, "vkCreateDevice failed (%d)", int(vr)), false;
#define X(f) { auto p = reinterpret_cast<PFN_##f>(gdpa(g_dev, #f)); if (p) vk.f = p; }
    VK_FUNCS(X)
#undef X
    vk.vkGetDeviceQueue(g_dev, g_family, 0, &g_queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_family;
    if (!Check(vk.vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool), "vkCreateCommandPool"))
        return PutStatus(dnr::kNoVulkan, "%s", LastError().c_str()), false;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP, qpi.queryCount = kTimestamps;
    bool made = Check(vk.vkAllocateCommandBuffers(g_dev, &cai, &g_initCmd), "vkAllocateCommandBuffers") &&
                Check(vk.vkCreateFence(g_dev, &fci, nullptr, &g_initFence), "vkCreateFence");
    for (SlotWork &s : g_slotWork)
        made = made && Check(vk.vkAllocateCommandBuffers(g_dev, &cai, &s.cmd), "vkAllocateCommandBuffers") &&
               Check(vk.vkCreateFence(g_dev, &fci, nullptr, &s.fence), "vkCreateFence") &&
               Check(vk.vkCreateQueryPool(g_dev, &qpi, nullptr, &s.timer), "vkCreateQueryPool");
    if (!made) return PutStatus(dnr::kNoVulkan, "%s", LastError().c_str()), false;

    // NGX: the driver's runtime, the DLLs from the mod's folder, their capabilities
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", g_dataDirW.c_str(), g_inst,
                                                              g_pd, g_dev, gipa, gdpa, &g_common);
    {
        std::lock_guard<std::mutex> lk(g_statusMx);
        g_status.ngxResult = uint32_t(r);
    }
    if (NVSDK_NGX_FAILED(r)) {
        PutStatus(dnr::kNgxFailed, "NVIDIA NGX did not start: %s (0x%08X); the NVIDIA driver may be too old", NgxText(r), unsigned(r));
        return true;   // (Vulkan is there: the round trip still runs in its test mode)
    }
    g_ngx = true;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_caps)) || !g_caps) {
        PutStatus(dnr::kNgxFailed, "NGX gave no capabilities");
        return true;
    }
    int avail = 0, needs = 0, init = 0;
    unsigned major = 0, minor = 0;
    g_caps->Get(NVSDK_NGX_Parameter_DLSSNR_Available, &avail);
    g_caps->Get(NVSDK_NGX_Parameter_DLSSNR_NeedsUpdatedDriver, &needs);
    g_caps->Get(NVSDK_NGX_Parameter_DLSSNR_MinDriverVersionMajor, &major);
    g_caps->Get(NVSDK_NGX_Parameter_DLSSNR_MinDriverVersionMinor, &minor);
    g_caps->Get(NVSDK_NGX_Parameter_DLSSNR_FeatureInitResult, &init);
    int srAvail = 0, srNeeds = 0;
    g_caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &srAvail);
    g_caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &srNeeds);
    dnr::Status s;
    {
        std::lock_guard<std::mutex> lk(g_statusMx);
        g_status.available = uint32_t(avail), g_status.needsDriver = uint32_t(needs);
        g_status.minDriver[0] = major, g_status.minDriver[1] = minor;
        g_status.featureInit = uint32_t(init);
        g_srAvailable = srAvail && !srNeeds && g_status.srDll[0];
        g_status.srAvailable = g_srAvailable ? 1 : 0;
        s = g_status;
    }
    if (!s.srDll[0])
        PutSrMessage("nvngx_dlss.dll (NVIDIA's DLSS Super Resolution) is missing: put it into %s", H->mod_dir(M));
    else if (srNeeds)
        PutSrMessage("DLSS Super Resolution needs a newer NVIDIA driver");
    else if (!srAvail)
        PutSrMessage("DLSS Super Resolution does not run on %s (it needs a GeForce RTX)", s.gpu);
    else
        PutSrMessage("DLSS Super Resolution ready (nvngx_dlss.dll %s)", s.srDll);
    if (!s.dll[0])
        PutStatus(dnr::kNoDll, "nvngx_dlssnr.dll (NVIDIA's DLSS 5 NR) is missing: put it into %s", H->mod_dir(M));
    else if (needs)
        PutStatus(dnr::kOldDriver, "update the NVIDIA driver: DLSS 5 NR needs %u.%u or newer (this one: %u.%u)", major, minor, s.driver[0], s.driver[1]);
    else if (!avail || s.supportBits)
        PutStatus(dnr::kUnsupported, "DLSS 5 NR does not run on %s: NVIDIA's DLL wants a GeForce RTX 50 (Blackwell, architecture 0x%X)",
                  s.gpu, s.minArch ? s.minArch : 0x1B0u);
    else {
        g_available = true;
        PutStatus(dnr::kReady, "DLSS 5 NR ready on %s (nvngx_dlssnr.dll %s)", s.gpu, s.dll);
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&g_params))) g_params = nullptr;
    return true;
}

// ---- the render size of an upscale mode: NGX's optimal one for it (DLSS SR's own table), the frame's when off
NVSDK_NGX_PerfQuality_Value Quality(uint32_t up) {
    switch (up) {
    case dnr::kUpDlaa: return NVSDK_NGX_PerfQuality_Value_DLAA;
    case dnr::kUpQuality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case dnr::kUpBalanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case dnr::kUpPerformance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    default: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
}
void RenderSize(uint32_t outW, uint32_t outH, uint32_t up, uint32_t *rw, uint32_t *rh) {
    *rw = outW, *rh = outH;
    if (up == dnr::kUpOff || up == dnr::kUpDlaa || !g_srAvailable || !g_caps) return;
    void *cb = nullptr;   // (NGX_DLSS_GET_OPTIMAL_SETTINGS of nvsdk_ngx_helpers.h, which needs Direct3D's headers)
    NVSDK_NGX_Parameter_GetVoidPointer(g_caps, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &cb);
    if (!cb) return;
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Width, outW);
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Height, outH);
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_PerfQualityValue, Quality(up));
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_RTXValue, 0);
    if (NVSDK_NGX_FAILED(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(cb)(g_caps))) return;
    unsigned w = 0, h = 0;
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutWidth, &w);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutHeight, &h);
    if (w >= 16 && h >= 16 && w <= outW && h <= outH) *rw = w, *rh = h;
}

// ---- the frame's work (the worker)
void Barrier(VkCommandBuffer c, const std::vector<VkImageMemoryBarrier> &b, VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    if (!b.empty()) vk.vkCmdPipelineBarrier(c, src, dst, 0, 0, nullptr, 0, nullptr, uint32_t(b.size()), b.data());
}
VkImageMemoryBarrier ImgBarrier(Image &i, VkAccessFlags from, VkAccessFlags to, uint32_t srcQ = VK_QUEUE_FAMILY_IGNORED,
                                uint32_t dstQ = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = from, b.dstAccessMask = to;
    b.oldLayout = i.fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = i.fresh ? VK_QUEUE_FAMILY_IGNORED : srcQ;
    b.dstQueueFamilyIndex = i.fresh ? VK_QUEUE_FAMILY_IGNORED : dstQ;
    b.image = i.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    i.fresh = false;
    return b;
}
void Copy(VkCommandBuffer c, Image &from, Image &to) {
    VkImageCopy r{};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {std::min(from.width, to.width), std::min(from.height, to.height), 1};
    vk.vkCmdCopyImage(c, from.image, VK_IMAGE_LAYOUT_GENERAL, to.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
}

uint32_t g_wantRW, g_wantRH, g_wantFor[3];   // the render size answered last, and the frame size and upscale it is for
void Answer(const dnr::View &v, bool ok, bool signalled, uint32_t outGen, float workMs) {
    dnr::Done d{};
    d.frame = v.frame;
    d.askQpc = v.askQpc;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    d.doneQpc = t.QuadPart;
    d.ok = ok;
    d.signalled = signalled;
    d.slot = v.slot;
    d.outGen = outGen;
    d.renderW = g_wantRW, d.renderH = g_wantRH;
    d.forW = g_wantFor[0], d.forH = g_wantFor[1], d.forUpscale = g_wantFor[2];
    d.mode = v.mode;
    d.workMs = workMs;
    d.gpuMs = g_gpuMs, d.gpuNrMs = g_gpuNrMs, d.gpuSrMs = g_gpuSrMs;
    if (!ok) snprintf(d.error, sizeof d.error, "%s", LastError().c_str());
    H->channel_write(g_doneCh, &d, sizeof d);
}

// The shared fences as timeline semaphores (imported again when the mod thread made them anew).
bool ImportFence(SharedFence &f) {
    if (f.importedGen == f.gen && f.sem) return true;
    if (f.sem) vk.vkDestroySemaphore(g_dev, f.sem, nullptr), f.sem = VK_NULL_HANDLE;
    if (!f.handle) return false;
    VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    si.pNext = &ti;
    if (!Check(vk.vkCreateSemaphore(g_dev, &si, nullptr, &f.sem), "vkCreateSemaphore (timeline)")) return false;
    VkImportSemaphoreWin32HandleInfoKHR ii{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    ii.semaphore = f.sem;
    ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    ii.handle = f.handle;
    if (!vk.vkImportSemaphoreWin32HandleKHR || !Check(vk.vkImportSemaphoreWin32HandleKHR(g_dev, &ii), "vkImportSemaphoreWin32HandleKHR (a shared fence)")) {
        vk.vkDestroySemaphore(g_dev, f.sem, nullptr), f.sem = VK_NULL_HANDLE;
        return false;
    }
    f.importedGen = f.gen;
    return true;
}

// A slot's command buffer free again: its last work waited for (it is long done when the ring turns), its times read.
bool FreeSlot(uint32_t s) {
    SlotWork &w = g_slotWork[s];
    if (!w.pending) return true;
    if (vk.vkWaitForFences(g_dev, 1, &w.fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) {
        Error("the GPU did not finish a frame's DLSS in 2 s");
        return false;
    }
    vk.vkResetFences(g_dev, 1, &w.fence);
    w.pending = false;
    uint64_t ts[kTimestamps] = {};
    if (vk.vkGetQueryPoolResults(g_dev, w.timer, 0, kTimestamps, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        auto ms = [&](uint32_t a, uint32_t b) { return float(double(ts[b] - ts[a]) * g_tickNs / 1e6); };
        g_gpuMs = ms(kTsStart, kTsEnd), g_gpuNrMs = ms(kTsStart, kTsNr), g_gpuSrMs = ms(kTsNr, kTsSr);
    }
    return true;
}

// Queues a slot's recorded work: it waits for "ready" frame (the pass's inputs) and signals "done" frame.
bool Queue(uint32_t s, uint64_t frame) {
    SlotWork &w = g_slotWork[s];
    VkTimelineSemaphoreSubmitInfo tl{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tl.waitSemaphoreValueCount = 1, tl.pWaitSemaphoreValues = &frame;
    tl.signalSemaphoreValueCount = 1, tl.pSignalSemaphoreValues = &frame;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &tl;
    si.waitSemaphoreCount = 1, si.pWaitSemaphores = &g_ready.sem, si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1, si.pCommandBuffers = &w.cmd;
    si.signalSemaphoreCount = 1, si.pSignalSemaphores = &g_done.sem;
    VkResult vr = vk.vkQueueSubmit(g_queue, 1, &si, w.fence);
    if (vr != VK_SUCCESS) {
        Error("vkQueueSubmit failed (" + std::to_string(vr) + ")");
        if (vr == VK_ERROR_DEVICE_LOST) g_broken = true;
        return false;
    }
    w.pending = true;
    g_signals++;
    return true;
}

// A prepared view whose work cannot be done: its "done" signalled all the same (the pass's GPU waits for it).
void SignalOnly(const dnr::View &v) {
    uint32_t s = v.slot % dnr::kSlots;
    if (!FreeSlot(s)) { g_broken = true; return; }
    SlotWork &w = g_slotWork[s];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkResetCommandBuffer(w.cmd, 0);
    vk.vkBeginCommandBuffer(w.cmd, &bi);
    vk.vkCmdResetQueryPool(w.cmd, w.timer, 0, kTimestamps);
    for (uint32_t t = 0; t < kTimestamps; t++) vk.vkCmdWriteTimestamp(w.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, w.timer, t);
    vk.vkEndCommandBuffer(w.cmd);
    if (!Queue(s, v.frame)) g_broken = true;
}

// DLSS-NR's feature at this size (and preset), DLSS SR's for this key: made again when either changes.
bool RunInit(const char *what, NVSDK_NGX_Result (*make)(VkCommandBuffer, void *), void *user) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkResetCommandBuffer(g_initCmd, 0);
    vk.vkBeginCommandBuffer(g_initCmd, &bi);
    NVSDK_NGX_Result r = make(g_initCmd, user);
    vk.vkEndCommandBuffer(g_initCmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &g_initCmd;
    vk.vkResetFences(g_dev, 1, &g_initFence);
    if (vk.vkQueueSubmit(g_queue, 1, &si, g_initFence) == VK_SUCCESS) vk.vkWaitForFences(g_dev, 1, &g_initFence, VK_TRUE, 5000000000ull);
    if (NVSDK_NGX_FAILED(r)) {
        char b[160];
        snprintf(b, sizeof b, "%s was not made: %s (0x%08X)", what, NgxText(r), unsigned(r));
        Error(b);
        return false;
    }
    return true;
}
struct NrMake { uint32_t w, h, preset; };
NVSDK_NGX_Result MakeNr(VkCommandBuffer c, void *u) {
    auto *m = static_cast<NrMake *>(u);
    NVSDK_NGX_Parameter_SetI(g_params, NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, int(m->preset));
    NVSDK_NGX_DLSSNR_Create_Params cp{m->w, m->h};
    return NGX_VULKAN_CREATE_DLSSNR_EXT1(g_dev, c, 1, 1, &g_nrFeature, g_params, &cp);
}
bool NrFeature(uint32_t w, uint32_t h, uint32_t preset) {
    if (g_nrFeature && g_nrW == w && g_nrH == h && g_nrPreset == preset) return true;
    if (g_nrFeature) vk.vkDeviceWaitIdle(g_dev), NVSDK_NGX_VULKAN_ReleaseFeature(g_nrFeature), g_nrFeature = nullptr;
    if (!g_params) return Error("NGX's parameters were not allocated"), false;
    NrMake m{w, h, preset};
    char what[96];
    snprintf(what, sizeof what, "DLSS 5 NR's feature at %ux%u", w, h);
    if (!RunInit(what, MakeNr, &m) || !g_nrFeature) {
        g_nrFeature = nullptr;
        PutStatus(dnr::kFailed, "%s", LastError().c_str());
        g_available = false;
        return false;
    }
    g_nrW = w, g_nrH = h, g_nrPreset = preset;
    return true;
}
NVSDK_NGX_Result MakeSr(VkCommandBuffer c, void *u) {
    auto *k = static_cast<SrKey *>(u);
    // the preset hint for every quality (the one created reads its own): 0 the DLL's choice
    for (const char *p : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                          NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(g_params, p, k->preset);
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = k->rw, cp.Feature.InHeight = k->rh;
    cp.Feature.InTargetWidth = k->outW, cp.Feature.InTargetHeight = k->outH;
    cp.Feature.InPerfQualityValue = Quality(k->upscale);
    // the game's colour as it is shown (LDR, gamma: not HDR), its motion vectors at the render size without the jitter,
    // its depth 0 near 1 far
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    return NGX_VULKAN_CREATE_DLSS_EXT1(g_dev, c, 1, 1, &g_srFeature, g_params, &cp);
}
bool SrFeature(const SrKey &k) {
    if (g_srFeature && !memcmp(&g_srKey, &k, sizeof k)) return true;
    if (g_srFeature) vk.vkDeviceWaitIdle(g_dev), NVSDK_NGX_VULKAN_ReleaseFeature(g_srFeature), g_srFeature = nullptr;
    if (!g_params) return Error("NGX's parameters were not allocated"), false;
    SrKey m = k;
    char what[128];
    snprintf(what, sizeof what, "DLSS Super Resolution's feature (%ux%u to %ux%u)", k.rw, k.rh, k.outW, k.outH);
    if (!RunInit(what, MakeSr, &m) || !g_srFeature) {
        g_srFeature = nullptr;
        PutSrMessage("%s", LastError().c_str());
        g_srAvailable = false;
        {
            std::lock_guard<std::mutex> lk(g_statusMx);
            g_status.srAvailable = 0;
        }
        return false;
    }
    g_srKey = k;
    return true;
}

NVSDK_NGX_Resource_VK Res(Image &i, bool rw) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(i.view, i.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, i.format, i.width, i.height, rw);
}

bool MakeLocals(uint32_t rw, uint32_t rh, uint32_t outW, uint32_t outH, bool nr, bool sr) {
    bool remake = L.color.width != rw || L.color.height != rh || (nr && (L.nr.width != rw || L.nr.height != rh)) ||
                  (sr && (L.sr.width != outW || L.sr.height != outH));
    if (!remake) return true;
    vk.vkDeviceWaitIdle(g_dev);   // (the slots' work may still read them)
    bool ok = MakeLocal(L.color, rw, rh, VK_FORMAT_R16G16B16A16_SFLOAT) && MakeLocal(L.depth, rw, rh, VK_FORMAT_R32_SFLOAT) &&
              MakeLocal(L.mvec, rw, rh, VK_FORMAT_R16G16B16A16_SFLOAT) && MakeLocal(L.mask, rw, rh, VK_FORMAT_R8G8B8A8_UNORM);
    if (ok && nr) ok = MakeLocal(L.nr, rw, rh, VK_FORMAT_R16G16B16A16_SFLOAT);
    if (ok && sr) ok = MakeLocal(L.sr, outW, outH, VK_FORMAT_R16G16B16A16_SFLOAT);
    return ok;
}

void Render(const dnr::View &v) {
    double t0 = Now();
    const uint32_t outW = v.outW, outH = v.outH;
    const uint32_t up = g_srAvailable && v.upscale < dnr::kUpModes ? v.upscale : dnr::kUpOff;
    uint32_t rw = outW, rh = outH;
    if (outW && outH && outW <= 8192 && outH <= 8192) RenderSize(outW, outH, up, &rw, &rh);
    g_wantRW = rw, g_wantRH = rh, g_wantFor[0] = outW, g_wantFor[1] = outH, g_wantFor[2] = v.upscale;
    std::unique_lock<std::mutex> lk(g_texMx);
    bool texOk = g_texOutW == outW && g_texOutH == outH && g_texRW == rw && g_texRH == rh && g_ready.handle && g_done.handle;
    if (!texOk) {   // (the textures at these sizes: asked of the mod thread)
        std::lock_guard<std::mutex> wk(g_wantMx);
        if (outW && outH) g_want = Want{outW, outH, rw, rh};
    }
    if (!v.prepared) return Answer(v, false, false, 0, 0);   // (an ask: nothing was signalled for it)
    // a prepared view: its "done" is signalled, whatever comes of it
    if (!ImportFence(g_ready) || !ImportFence(g_done)) {
        Error("the shared fences could not be imported: the frames cannot be handed over");
        g_broken = true;
        return Answer(v, false, false, 0, 0);
    }
    const uint32_t s = v.slot % dnr::kSlots;
    for (uint32_t k = 0; texOk && k < dnr::kPerSlot; k++) {
        Shared &t = g_tex[s][k];
        if (!t.handle) { texOk = false; break; }
        if (t.importedGen != t.gen) {
            bool in = k != dnr::kColorOut;
            if (!Import(t.img, t.handle, in ? rw : outW, in ? rh : outH, VkFormatOf(dnr::kTexFormats[k]))) { texOk = false; break; }
            t.importedGen = t.gen;
        }
    }
    // (the pass drew into the generations it names: textures made since are not written yet)
    if (!texOk || v.inGen != g_tex[s][dnr::kColorIn].gen || v.outGen != g_tex[s][dnr::kColorOut].gen || v.renderW != rw || v.renderH != rh) {
        SignalOnly(v);
        return Answer(v, false, !g_broken, 0, 0);
    }
    const bool nr = v.mode == dnr::kModeNr;
    const bool sr = up != dnr::kUpOff;
    if (nr && !g_available) {
        Error("DLSS 5 NR is not available here (dlss_nr.side says why)");
        SignalOnly(v);
        return Answer(v, false, !g_broken, 0, 0);
    }
    bool ok = MakeLocals(rw, rh, outW, outH, nr, sr) && (!nr || NrFeature(rw, rh, v.preset)) &&
              (!sr || SrFeature(SrKey{rw, rh, outW, outH, up, v.srPreset}));
    if (!ok || !FreeSlot(s)) {
        SignalOnly(v);
        return Answer(v, false, !g_broken, 0, 0);
    }
    SlotWork &w = g_slotWork[s];
    VkCommandBuffer c = w.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkResetCommandBuffer(c, 0);
    vk.vkBeginCommandBuffer(c, &bi);
    vk.vkCmdResetQueryPool(c, w.timer, 0, kTimestamps);
    vk.vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, w.timer, kTsStart);
    Shared *t = g_tex[s];
    // the slot's shared textures taken from Direct3D (their content kept: GENERAL, from the external queue family),
    // the half's own images made ready to be written by the copies
    std::vector<VkImageMemoryBarrier> b;
    for (uint32_t k = 0; k < dnr::kPerSlot; k++)
        b.push_back(ImgBarrier(t[k].img, 0, k == dnr::kColorOut ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
                               VK_QUEUE_FAMILY_EXTERNAL, g_family));
    const VkAccessFlags any = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    for (Image *i : {&L.color, &L.depth, &L.mvec, &L.mask}) b.push_back(ImgBarrier(*i, any, VK_ACCESS_TRANSFER_WRITE_BIT));
    Barrier(c, b, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    Copy(c, t[dnr::kColorIn].img, L.color);
    Copy(c, t[dnr::kDepthIn].img, L.depth);
    Copy(c, t[dnr::kMvecIn].img, L.mvec);
    Copy(c, t[dnr::kMaskIn].img, L.mask);
    b.clear();
    for (Image *i : {&L.color, &L.depth, &L.mvec, &L.mask}) b.push_back(ImgBarrier(*i, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT));
    if (nr) b.push_back(ImgBarrier(L.nr, any, VK_ACCESS_SHADER_WRITE_BIT));
    if (sr) b.push_back(ImgBarrier(L.sr, any, VK_ACCESS_SHADER_WRITE_BIT));
    Barrier(c, b, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    Image *picture = &L.color;   // what is handed on: the frame (NR bypassed), DLSS-NR's, DLSS SR's
    NVSDK_NGX_Resource_VK depth = Res(L.depth, false), mvec = Res(L.mvec, false);
    if (nr) {
        NVSDK_NGX_Resource_VK color = Res(L.color, false), out = Res(L.nr, true), mask = Res(L.mask, false);
        NVSDK_NGX_VK_DLSSNR_Eval_Params e{};
        e.pInColor = &color, e.pInOutput = &out, e.pInMVec = &mvec, e.pInDepth = &depth, e.pInControlMask = &mask;
        e.InLocalToneStrength = v.tone, e.InLocalStructureStrength = v.structure;
        e.InStyle = v.style;
        e.InUseAutoMask = v.autoMask ? 1 : 0;
        e.InMaskBlendMode = v.autoMask ? NVSDK_NGX_DLSSNR_Mask_Blend_Mode_Min : NVSDK_NGX_DLSSNR_Mask_Blend_Mode_None;
        e.InSkinStructureStrength = v.autoMask ? v.skin : 0;
        e.InReset = v.reset ? 1 : 0;
        e.InDepthInverted = 0;
        e.InEnabled = 1;
        e.InIntensity = v.intensity;
        e.InMVecScaleX = e.InMVecScaleY = 1;   // (render pixels)
        e.InColorSubrectSize = e.InOutputSubrectSize = e.InMVecSubrectSize = e.InDepthSubrectSize = e.InControlMaskSubrectSize = {rw, rh};
        NVSDK_NGX_Parameter_SetF(g_params, NVSDK_NGX_Parameter_Jitter_Offset_X, v.jitter[0]);
        NVSDK_NGX_Parameter_SetF(g_params, NVSDK_NGX_Parameter_Jitter_Offset_Y, v.jitter[1]);
        NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSSNR_EXT(c, g_nrFeature, g_params, &e);
        if (NVSDK_NGX_FAILED(r)) {
            char m[160];
            snprintf(m, sizeof m, "DLSS 5 NR's evaluation failed: %s (0x%08X)", NgxText(r), unsigned(r));
            Error(m);
            ok = false;
        }
        b.clear();
        b.push_back(ImgBarrier(L.nr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT));
        Barrier(c, b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        picture = &L.nr;
    }
    vk.vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, w.timer, kTsNr);
    if (sr && ok) {
        NVSDK_NGX_Resource_VK color = Res(*picture, false), out = Res(L.sr, true);
        NVSDK_NGX_VK_DLSS_Eval_Params e{};
        e.Feature.pInColor = &color, e.Feature.pInOutput = &out;
        e.pInDepth = &depth, e.pInMotionVectors = &mvec;
        e.InJitterOffsetX = v.jitter[0], e.InJitterOffsetY = v.jitter[1];
        e.InRenderSubrectDimensions = {rw, rh};
        e.InReset = v.reset ? 1 : 0;
        e.InMVScaleX = e.InMVScaleY = 1;   // (render pixels)
        NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(c, g_srFeature, g_params, &e);
        if (NVSDK_NGX_FAILED(r)) {
            char m[160];
            snprintf(m, sizeof m, "DLSS Super Resolution's evaluation failed: %s (0x%08X)", NgxText(r), unsigned(r));
            Error(m);
            ok = false;
        }
        b.clear();
        b.push_back(ImgBarrier(L.sr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
        Barrier(c, b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        picture = &L.sr;
    }
    vk.vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, w.timer, kTsSr);
    if (ok && picture->width == outW && picture->height == outH) Copy(c, *picture, t[dnr::kColorOut].img);
    else ok = false;
    // back to Direct3D
    b.clear();
    for (uint32_t k = 0; k < dnr::kPerSlot; k++)
        b.push_back(ImgBarrier(t[k].img, k == dnr::kColorOut ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT, 0, g_family,
                               VK_QUEUE_FAMILY_EXTERNAL));
    Barrier(c, b, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vk.vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, w.timer, kTsEnd);
    vk.vkEndCommandBuffer(c);
    if (!Queue(s, v.frame)) return Answer(v, false, false, 0, 0);
    float work = float(Now() - t0);
    g_workMs = work;
    if (ok) {
        (nr ? g_evals : g_copies)++;
        if (sr) g_srEvals++;
    } else {
        g_failures++;
    }
    Answer(v, ok, true, t[dnr::kColorOut].gen, work);
}

void Work() {
    if (!g_ready_) {
        uint32_t luid[2] = {};
        if (g_tried || !H->adapter_luid(luid)) return;   // (the game's adapter is known once its graphics bridge runs)
        g_tried = true;
        g_ready_ = Start(luid);
        if (!g_ready_) return;
    }
    dnr::View v;
    uint64_t vv = 0;
    if (H->channel_read(g_viewCh, &v, sizeof v, &vv) == sizeof v && vv != g_viewSeen) {
        g_viewSeen = vv;
        Render(v);
    }
}

int GuardedWork() {
    __try {
        Work();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<int>(GetExceptionCode());
    }
}

void Worker() {
    while (!g_stop) {
        H->channel_wait(g_viewCh, g_viewSeen, 5);   // (a pass's ask wakes it at once)
        if (g_stop) break;
        if (g_broken) { Sleep(10); continue; }
        if (int code = GuardedWork()) {
            g_broken = true;
            char b[160];
            snprintf(b, sizeof b, "a fault (0x%08X) in DLSS's work: stopped (mod.reload starts again)", static_cast<unsigned>(code));
            Error(b);
            PutStatus(dnr::kFailed, "%s", b);
        }
    }
}

// ---- the mod thread: the textures at the size the worker wants, the fences once, the errors logged
void MakeFence(SharedFence &f, const char *name) {
    if (f.handle) return;
    MmlSideFence o{};
    o.size = sizeof o;
    if (!MML_SIDE_HAS(H, fence_create) || !H->fence_create(M, name, &o)) {
        Error(std::string("fence_create ") + name + " failed (a sidecar older than this mod?)");
        return;
    }
    f.handle = o.shared_handle;
    f.gen = o.generation;
}

void MakeTextures() {
    Want w;
    {
        std::lock_guard<std::mutex> wk(g_wantMx);
        w = g_want;
    }
    if (!w.outW || !w.outH || !H->texture_d3d12_device()) return;
    std::lock_guard<std::mutex> lk(g_texMx);   // (the worker is not working meanwhile: its imports can go)
    MakeFence(g_ready, dnr::kFenceReady);
    MakeFence(g_done, dnr::kFenceDone);
    if (w.outW == g_texOutW && w.outH == g_texOutH && w.rw == g_texRW && w.rh == g_texRH) return;
    if (g_dev) vk.vkDeviceWaitIdle(g_dev);
    for (auto &slot : g_tex)
        for (Shared &s : slot) Free(s.img), s.importedGen = 0, s.handle = nullptr;
    g_texOutW = g_texOutH = g_texRW = g_texRH = 0;
    for (uint32_t s = 0; s < dnr::kSlots; s++)
        for (uint32_t k = 0; k < dnr::kPerSlot; k++) {
            char name[32];
            dnr::TexName(name, sizeof name, s, k);
            bool in = k != dnr::kColorOut;
            MmlSideTexture t{};
            t.size = sizeof t;
            if (!H->texture_create(M, name, in ? w.rw : w.outW, in ? w.rh : w.outH, dnr::kTexFormats[k], &t)) {
                Error(std::string("texture_create ") + name + " failed");
                return;
            }
            g_tex[s][k].handle = t.shared_handle;
            g_tex[s][k].gen = t.generation;
        }
    g_texOutW = w.outW, g_texOutH = w.outH, g_texRW = w.rw, g_texRH = w.rh;
    H->log(M, 1, "the textures at %ux%u, the inputs at %ux%u", w.outW, w.outH, w.rw, w.rh);
}

void OnFrame(MmlSideMod *, const char *, const char *, void *) {
    if (g_ready_) {
        Want w;
        {
            std::lock_guard<std::mutex> wk(g_wantMx);
            w = g_want;
        }
        if (w.outW != g_texOutW || w.outH != g_texOutH || w.rw != g_texRW || w.rh != g_texRH || !g_ready.handle || !g_done.handle) MakeTextures();
    }
    std::string log;
    {
        std::lock_guard<std::mutex> lk(g_errMx);
        log.swap(g_toLog);
    }
    if (!log.empty()) H->log(M, 2, "%s", log.c_str());
}

const char *StageName(uint32_t s) {
    static const char *const n[] = {"starting", "no_vulkan", "ngx_failed", "no_dll", "old_driver", "unsupported", "ready", "failed",
                                    "other_gpu"};
    return s < sizeof n / sizeof n[0] ? n[s] : "?";
}

int Side(MmlSideMod *, const char *, MmlSideReply *r, void *) {
    dnr::Status s;
    {
        std::lock_guard<std::mutex> lk(g_statusMx);
        s = g_status;
    }
    char arch[16], init[16];
    snprintf(arch, sizeof arch, "0x%X", s.minArch);
    snprintf(init, sizeof init, "0x%08X", s.featureInit);
    auto r3 = [](float x) { return std::round(x * 1000) / 1000; };
    nlohmann::json j = {{"stage", StageName(s.stage)}, {"message", s.message}, {"gpu", s.gpu},
                        {"driver", std::to_string(s.driver[0]) + "." + std::to_string(s.driver[1])},
                        {"dll", s.dll}, {"dll_path", s.dllPath}, {"ngx_init", NgxText(NVSDK_NGX_Result(s.ngxResult))},
                        {"available", s.available != 0}, {"needs_driver", s.needsDriver != 0},
                        {"min_driver", std::to_string(s.minDriver[0]) + "." + std::to_string(s.minDriver[1])},
                        {"support_bits", s.supportBits}, {"min_arch", arch}, {"feature_init", init},
                        {"sr", {{"available", s.srAvailable != 0}, {"dll", s.srDll}, {"message", s.srMessage},
                                {"feature", {g_srKey.rw, g_srKey.rh, g_srKey.outW, g_srKey.outH}}, {"upscale", g_srKey.upscale},
                                {"preset", g_srKey.preset}, {"evaluations", g_srEvals.load()}}},
                        {"ready", g_ready_.load()}, {"broken", g_broken.load()}, {"evaluations", g_evals.load()},
                        {"copies", g_copies.load()}, {"failures", g_failures.load()}, {"signals", g_signals.load()},
                        {"textures", {{"frame", {g_texOutW, g_texOutH}}, {"inputs", {g_texRW, g_texRH}}}},
                        {"fences", {{"ready", g_ready.gen}, {"done", g_done.gen}}},
                        {"nr_feature", {g_nrW, g_nrH}}, {"work_ms", r3(g_workMs.load())}, {"gpu_ms", r3(g_gpuMs.load())},
                        {"gpu_nr_ms", r3(g_gpuNrMs.load())}, {"gpu_sr_ms", r3(g_gpuSrMs.load())}, {"error", LastError()}};
    std::string out = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    H->reply_append(r, out.c_str(), out.size());
    return 0;
}

void Shutdown() {
    if (g_worker.joinable()) {
        g_stop = true;
        g_worker.join();
    }
    if (g_dev) vk.vkDeviceWaitIdle(g_dev);
    if (g_nrFeature) NVSDK_NGX_VULKAN_ReleaseFeature(g_nrFeature), g_nrFeature = nullptr;
    if (g_srFeature) NVSDK_NGX_VULKAN_ReleaseFeature(g_srFeature), g_srFeature = nullptr;
    if (g_params) NVSDK_NGX_VULKAN_DestroyParameters(g_params), g_params = nullptr;
    if (g_ngx) NVSDK_NGX_VULKAN_Shutdown1(g_dev), g_ngx = false;
    if (g_dev) {
        for (Image *i : {&L.color, &L.depth, &L.mvec, &L.mask, &L.nr, &L.sr}) Free(*i);
        for (auto &slot : g_tex)
            for (Shared &s : slot) Free(s.img);
        for (SharedFence *f : {&g_ready, &g_done})
            if (f->sem) vk.vkDestroySemaphore(g_dev, f->sem, nullptr), f->sem = VK_NULL_HANDLE;
        for (SlotWork &s : g_slotWork) {
            if (s.timer) vk.vkDestroyQueryPool(g_dev, s.timer, nullptr);
            if (s.fence) vk.vkDestroyFence(g_dev, s.fence, nullptr);
            s = SlotWork{};
        }
        if (g_initFence) vk.vkDestroyFence(g_dev, g_initFence, nullptr), g_initFence = VK_NULL_HANDLE;
        if (g_pool) vk.vkDestroyCommandPool(g_dev, g_pool, nullptr), g_pool = VK_NULL_HANDLE;
        vk.vkDestroyDevice(g_dev, nullptr);
        g_dev = VK_NULL_HANDLE;
    }
    for (uint32_t s = 0; s < dnr::kSlots; s++)
        for (uint32_t k = 0; k < dnr::kPerSlot; k++) {
            if (!g_tex[s][k].handle) continue;
            char name[32];
            dnr::TexName(name, sizeof name, s, k);
            H->texture_remove(M, name);
            g_tex[s][k].handle = nullptr;
        }
    if (MML_SIDE_HAS(H, fence_remove)) {
        if (g_ready.handle) H->fence_remove(M, dnr::kFenceReady);
        if (g_done.handle) H->fence_remove(M, dnr::kFenceDone);
    }
    g_ready = SharedFence{}, g_done = SharedFence{};
    if (g_inst) vk.vkDestroyInstance(g_inst, nullptr), g_inst = VK_NULL_HANDLE;
    if (g_vkDll) FreeLibrary(g_vkDll), g_vkDll = nullptr;
}
} // namespace

MML_EXPORT uint32_t mml_side_api(void) { return MML_SIDE_API_VERSION; }

MML_EXPORT int mml_side_load(const MmlSideHost *host, MmlSideMod *mod) {
    H = host;
    M = mod;
    if (!MML_SIDE_HAS(host, channel_wait) || !MML_SIDE_HAS(host, fence_create)) return 1;   // (a sidecar older than this half)
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freqMs = f.QuadPart / 1000.0;
    g_viewCh = H->channel_open(mod, "view", sizeof(dnr::View));
    g_doneCh = H->channel_open(mod, "done", sizeof(dnr::Done));
    g_statusCh = H->channel_open(mod, "status", sizeof(dnr::Status));
    if (!g_viewCh || !g_doneCh || !g_statusCh) return 1;
    PutStatus(dnr::kStarting, "starting: waiting for the game's first frame");
    g_stop = false;
    g_worker = std::thread(Worker);
    H->subscribe(mod, "FRAME", OnFrame, nullptr);
    H->command(mod, "side", "{} -> what NGX said (stage, message, gpu, driver, dll, available, sr...), evaluations, work_ms, gpu_ms", Side, nullptr);
    return 0;
}

MML_EXPORT void mml_side_unload(MmlSideMod *) { Shutdown(); }
