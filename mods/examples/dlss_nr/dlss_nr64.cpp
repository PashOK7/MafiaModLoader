// dlss_nr's 64-bit half (docs/MODDING.md "64-bit halves"): NVIDIA DLSS 5 Neural Rendering ("3D-guided neural
// rendering", NGX feature DLSSNR, nvngx_dlssnr.dll) over the game's own frame. NGX and its features exist only for
// 64 bits and DLSS-NR only for Vulkan, so it runs here, in the sidecar, on a Vulkan device of its own on the adapter
// the game draws on (the sidecar's adapter_luid, matched to Vulkan's deviceLUID).
//
// The frame: the 32-bit half draws it (colour, depth, motion vectors, a control mask) into four shared textures
// (Direct3D 12, made here: texture_create; imported into Vulkan as D3D12 resources), waits for its GPU to have them
// and writes "view". The worker copies them into images of its own (DLSS-NR reads and writes images made for it:
// storage, sampled; the shared ones are render targets), evaluates DLSS-NR, copies its picture into "color_out",
// waits for the GPU (a fence) and answers in "done"; the 32-bit half draws "color_out" as the frame.
//
// Whether this GPU can run it is NGX's to say (the driver and the DLL: DLSS-NR checks the GPU's architecture
// itself; it needs an RTX 50, Blackwell). What NGX said is in "status" and `dlss_nr.side`; the half is useful without
// it too: View::mode kModeCopy sends the frame round through the same textures and copies, untouched (the round trip
// tested on any GPU).
//
// nvngx_dlssnr.dll is not part of this mod (NVIDIA's, under its license): it is looked for in the mod's folder
// (<game>\mml\mods\dlss_nr\), where the player puts it. NGX keeps its log in the mod's data folder (nvngx.log).
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
    X(vkWaitForFences) X(vkResetFences) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)             \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView)                \
    X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkGetMemoryWin32HandlePropertiesKHR) X(vkCreateQueryPool)              \
    X(vkDestroyQueryPool) X(vkCmdWriteTimestamp) X(vkCmdResetQueryPool) X(vkGetQueryPoolResults) X(vkDeviceWaitIdle)
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
VkCommandBuffer g_cmd;
VkFence g_fence;
VkQueryPool g_timer;
float g_tickNs = 1;
VkPhysicalDeviceMemoryProperties g_mem;

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

// ---- images: the shared textures imported, and the half's own ones DLSS-NR works on
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

// One of the half's own images: what DLSS-NR reads (sampled) and writes (storage).
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

// ---- the textures: made on the mod thread (texture_create), imported by the worker
struct Shared {
    HANDLE handle = nullptr;
    uint32_t gen = 0;          // the sidecar's generation of it
    uint32_t importedGen = 0;  // the generation `img` holds
    Image img;
};
std::mutex g_texMx;            // the worker holds it for a frame's work; MakeTextures for its change
Shared g_tex[dnr::kTextures];
uint32_t g_texW, g_texH;
std::atomic<uint32_t> g_wantW{0}, g_wantH{0};
Image g_local[dnr::kTextures];   // the half's own: kColorIn.. kMaskIn read by DLSS-NR, kColorOut written by it

// ---- NGX
NVSDK_NGX_Parameter *g_caps, *g_params;
NVSDK_NGX_Handle *g_feature;
uint32_t g_featW, g_featH, g_featPreset = UINT32_MAX;
bool g_ngx, g_available;
std::atomic<bool> g_ready{false}, g_stop{false}, g_broken{false};
bool g_tried;
std::thread g_worker;
uint64_t g_viewSeen;
std::atomic<uint64_t> g_evals{0}, g_copies{0}, g_failures{0};
std::atomic<float> g_workMs{0}, g_gpuMs{0};
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

NVSDK_NGX_FeatureDiscoveryInfo Discovery(const NVSDK_NGX_FeatureCommonInfo *ci, const wchar_t *dataPath) {
    NVSDK_NGX_FeatureDiscoveryInfo di{};
    di.SDKVersion = NVSDK_NGX_Version_API;
    di.FeatureID = NVSDK_NGX_Feature_DLSSNR;
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

// Vulkan on the game's adapter, with what NGX's DLSS-NR asks of the instance and the device, and NGX on it.
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
    NVSDK_NGX_FeatureDiscoveryInfo di = Discovery(&g_common, g_dataDirW.c_str());

    {   // the DLL, before anything: its version for the player
        std::wstring dll = g_modDirW + L"\\nvngx_dlssnr.dll";
        std::string ver = FileVersion(dll);
        std::lock_guard<std::mutex> lk(g_statusMx);
        snprintf(g_status.dll, sizeof g_status.dll, "%s", ver.c_str());
        snprintf(g_status.dllPath, sizeof g_status.dllPath, "%s\\nvngx_dlssnr.dll", modDir.c_str());
    }

    std::vector<const char *> iext;
    uint32_t n = 0;
    VkExtensionProperties *props = nullptr;
    if (!NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&di, &n, &props)))
        for (uint32_t i = 0; i < n; i++) iext.push_back(props[i].extensionName);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MafiaModLoader dlss_nr";
    app.pEngineName = "LS3D";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(iext.size());
    ici.ppEnabledExtensionNames = iext.data();
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
            PutStatus(dnr::kOtherGpu, "the game draws on %s, and Vulkan sees no NVIDIA GPU: DLSS 5 NR needs a GeForce RTX 50",
                      pr.deviceName);
        else
            PutStatus(dnr::kOtherGpu, "the game draws on %s, not on the %s: in Windows' Settings > System > Display > Graphics give "
                      "Game.exe \"High performance\" (the NVIDIA GPU), and plug the monitor into the NVIDIA card", pr.deviceName, nv.c_str());
        return false;
    }
    g_tickNs = pr.limits.timestampPeriod;
    vk.vkGetPhysicalDeviceMemoryProperties(g_pd, &g_mem);

    // DLSS-NR's requirements of this GPU (NGX says before anything is made: the driver, the adapter, the OS)
    NVSDK_NGX_FeatureRequirement fr{};
    NVSDK_NGX_Result rr = NVSDK_NGX_VULKAN_GetFeatureRequirements(g_inst, g_pd, &di, &fr);
    {
        std::lock_guard<std::mutex> lk(g_statusMx);
        g_status.supportBits = NVSDK_NGX_FAILED(rr) ? 0xFFFFFFFFu : uint32_t(fr.FeatureSupported);
        g_status.minArch = fr.MinHWArchitecture;
    }

    // the device: a queue that computes and copies, NGX's extensions for DLSS-NR, the D3D12 resources imported
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
    std::vector<std::string> dext{VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME};
    n = 0, props = nullptr;
    if (!NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(g_inst, g_pd, &di, &n, &props)))
        for (uint32_t i = 0; i < n; i++)
            if (has(props[i].extensionName)) dext.push_back(props[i].extensionName);
    std::vector<const char *> dptr;
    for (auto &e : dext) dptr.push_back(e.c_str());
    float prio = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = g_family, q.queueCount = 1, q.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;   // (NGX's VK_KHR_buffer_device_address)
    f12.timelineSemaphore = VK_TRUE;
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
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cai.commandBufferCount = 1;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP, qpi.queryCount = 2;
    if (!Check(vk.vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool), "vkCreateCommandPool") ||
        !(cai.commandPool = g_pool, Check(vk.vkAllocateCommandBuffers(g_dev, &cai, &g_cmd), "vkAllocateCommandBuffers")) ||
        !Check(vk.vkCreateFence(g_dev, &fci, nullptr, &g_fence), "vkCreateFence") ||
        !Check(vk.vkCreateQueryPool(g_dev, &qpi, nullptr, &g_timer), "vkCreateQueryPool"))
        return PutStatus(dnr::kNoVulkan, "%s", LastError().c_str()), false;

    // NGX: the driver's runtime, the DLL from the mod's folder, its capabilities for DLSS-NR
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
    dnr::Status s;
    {
        std::lock_guard<std::mutex> lk(g_statusMx);
        g_status.available = uint32_t(avail), g_status.needsDriver = uint32_t(needs);
        g_status.minDriver[0] = major, g_status.minDriver[1] = minor;
        g_status.featureInit = uint32_t(init);
        s = g_status;
    }
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

void Answer(const dnr::View &v, bool ok, uint32_t outGen, float workMs, float gpuMs) {
    dnr::Done d{};
    d.frame = v.frame;
    d.askQpc = v.askQpc;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    d.doneQpc = t.QuadPart;
    d.ok = ok;
    d.outGeneration = outGen;
    d.mode = v.mode;
    d.workMs = workMs, d.gpuMs = gpuMs;
    if (!ok) snprintf(d.error, sizeof d.error, "%s", LastError().c_str());
    H->channel_write(g_doneCh, &d, sizeof d);
}

// DLSS-NR's feature at this size (and preset): made again when either changes.
bool Feature(uint32_t w, uint32_t h, uint32_t preset) {
    if (g_feature && g_featW == w && g_featH == h && g_featPreset == preset) return true;
    if (g_feature) {
        vk.vkDeviceWaitIdle(g_dev);
        NVSDK_NGX_VULKAN_ReleaseFeature(g_feature);
        g_feature = nullptr;
    }
    if (!g_params) return Error("NGX's parameters were not allocated"), false;
    NVSDK_NGX_Parameter_SetI(g_params, NVSDK_NGX_Parameter_DLSSNR_Hint_Render_Preset, int(preset));
    NVSDK_NGX_DLSSNR_Create_Params cp{w, h};
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.vkResetCommandBuffer(g_cmd, 0);
    vk.vkBeginCommandBuffer(g_cmd, &bi);
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSSNR_EXT1(g_dev, g_cmd, 1, 1, &g_feature, g_params, &cp);
    vk.vkEndCommandBuffer(g_cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &g_cmd;
    vk.vkResetFences(g_dev, 1, &g_fence);
    if (vk.vkQueueSubmit(g_queue, 1, &si, g_fence) == VK_SUCCESS) vk.vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, 5000000000ull);
    if (NVSDK_NGX_FAILED(r) || !g_feature) {
        g_feature = nullptr;
        char b[160];
        snprintf(b, sizeof b, "DLSS 5 NR's feature was not made at %ux%u: %s (0x%08X)", w, h, NgxText(r), unsigned(r));
        Error(b);
        PutStatus(dnr::kFailed, "%s", b);
        g_available = false;
        return false;
    }
    g_featW = w, g_featH = h, g_featPreset = preset;
    return true;
}

NVSDK_NGX_Resource_VK Res(Image &i, bool rw) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(i.view, i.image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, i.format, i.width, i.height, rw);
}

void Render(const dnr::View &v) {
    const uint32_t w = v.width, h = v.height;
    if (!w || !h || w > 8192 || h > 8192) return Answer(v, false, 0, 0, 0);
    std::unique_lock<std::mutex> lk(g_texMx);
    if (g_texW != w || g_texH != h) {   // (the textures at this size: asked of the mod thread, this frame goes without)
        g_wantW = w, g_wantH = h;
        return Answer(v, false, 0, 0, 0);
    }
    for (uint32_t i = 0; i < dnr::kTextures; i++) {
        Shared &s = g_tex[i];
        if (!s.handle) return Answer(v, false, 0, 0, 0);
        if (s.importedGen != s.gen) {
            if (!Import(s.img, s.handle, w, h, VkFormatOf(dnr::kTexFormats[i]))) return Answer(v, false, 0, 0, 0);
            s.importedGen = s.gen;
        }
    }
    for (uint32_t i = 0; i < 4; i++)   // (the 32-bit half drew into the generations it names: one made since is not written yet)
        if (v.inGeneration[i] != g_tex[i].gen) return Answer(v, false, g_tex[dnr::kColorOut].gen, 0, 0);
    const bool nr = v.mode == dnr::kModeNr;
    if (nr && !g_available) return Error("DLSS 5 NR is not available here (dlss_nr.side says why)"), Answer(v, false, 0, 0, 0);
    if (nr) {
        for (uint32_t i = 0; i < dnr::kTextures; i++)
            if (g_local[i].width != w || g_local[i].height != h)
                if (!MakeLocal(g_local[i], w, h, VkFormatOf(dnr::kTexFormats[i]))) return Answer(v, false, 0, 0, 0);
        if (!Feature(w, h, v.preset)) return Answer(v, false, 0, 0, 0);
    }
    double t0 = Now();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkCommandBuffer c = g_cmd;
    vk.vkResetCommandBuffer(c, 0);
    vk.vkBeginCommandBuffer(c, &bi);
    vk.vkCmdResetQueryPool(c, g_timer, 0, 2);
    vk.vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_timer, 0);
    // the shared textures taken from Direct3D (their content kept: GENERAL, from the external queue family)
    std::vector<VkImageMemoryBarrier> b;
    for (uint32_t i = 0; i < dnr::kTextures; i++)
        b.push_back(ImgBarrier(g_tex[i].img, 0, i == dnr::kColorOut ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
                               VK_QUEUE_FAMILY_EXTERNAL, g_family));
    if (nr)
        for (uint32_t i = 0; i < dnr::kTextures; i++)
            b.push_back(ImgBarrier(g_local[i], VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                   i == dnr::kColorOut ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT));
    Barrier(c, b, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    bool ok = true;
    if (!nr) {
        Copy(c, g_tex[dnr::kColorIn].img, g_tex[dnr::kColorOut].img);   // (the test: the frame back as it came)
    } else {
        for (uint32_t i = 0; i < 4; i++) Copy(c, g_tex[i].img, g_local[i]);
        b.clear();
        for (uint32_t i = 0; i < dnr::kTextures; i++)
            b.push_back(ImgBarrier(g_local[i], VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                                   i == dnr::kColorOut ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT));
        Barrier(c, b, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        NVSDK_NGX_Resource_VK color = Res(g_local[dnr::kColorIn], false), out = Res(g_local[dnr::kColorOut], true),
                              mvec = Res(g_local[dnr::kMvecIn], false), depth = Res(g_local[dnr::kDepthIn], false),
                              mask = Res(g_local[dnr::kMaskIn], false);
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
        e.InMVecScaleX = e.InMVecScaleY = 1;   // (pixels)
        e.InColorSubrectSize = e.InOutputSubrectSize = e.InMVecSubrectSize = e.InDepthSubrectSize = e.InControlMaskSubrectSize = {w, h};
        NVSDK_NGX_Parameter_SetF(g_params, NVSDK_NGX_Parameter_Jitter_Offset_X, 0);
        NVSDK_NGX_Parameter_SetF(g_params, NVSDK_NGX_Parameter_Jitter_Offset_Y, 0);
        NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSSNR_EXT(c, g_feature, g_params, &e);
        if (NVSDK_NGX_FAILED(r)) {
            char m[160];
            snprintf(m, sizeof m, "DLSS 5 NR's evaluation failed: %s (0x%08X)", NgxText(r), unsigned(r));
            Error(m);
            ok = false;
        }
        b.clear();
        b.push_back(ImgBarrier(g_local[dnr::kColorOut], VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
        Barrier(c, b, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (ok) Copy(c, g_local[dnr::kColorOut], g_tex[dnr::kColorOut].img);
    }
    // back to Direct3D
    b.clear();
    for (uint32_t i = 0; i < dnr::kTextures; i++)
        b.push_back(ImgBarrier(g_tex[i].img, i == dnr::kColorOut ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT, 0, g_family,
                               VK_QUEUE_FAMILY_EXTERNAL));
    Barrier(c, b, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vk.vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_timer, 1);
    vk.vkEndCommandBuffer(c);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &c;
    vk.vkResetFences(g_dev, 1, &g_fence);
    VkResult vr = vk.vkQueueSubmit(g_queue, 1, &si, g_fence);
    if (vr != VK_SUCCESS || vk.vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) {
        Error("the GPU did not finish the frame (vkQueueSubmit " + std::to_string(vr) + ")");
        if (vr == VK_ERROR_DEVICE_LOST) g_broken = true;
        return Answer(v, false, 0, 0, 0);
    }
    uint64_t ts[2] = {};
    float gpu = 0;
    if (vk.vkGetQueryPoolResults(g_dev, g_timer, 0, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        gpu = float(double(ts[1] - ts[0]) * g_tickNs / 1e6);
    float work = float(Now() - t0);
    g_workMs = work, g_gpuMs = gpu;
    if (ok) (nr ? g_evals : g_copies)++;
    else g_failures++;
    Answer(v, ok, g_tex[dnr::kColorOut].gen, work, gpu);
}

void Work() {
    if (!g_ready) {
        uint32_t luid[2] = {};
        if (g_tried || !H->adapter_luid(luid)) return;   // (the game's adapter is known once its graphics bridge runs)
        g_tried = true;
        g_ready = Start(luid);
        if (!g_ready) return;
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
            snprintf(b, sizeof b, "a fault (0x%08X) in DLSS 5 NR's work: stopped (mod.reload starts again)", static_cast<unsigned>(code));
            Error(b);
            PutStatus(dnr::kFailed, "%s", b);
        }
    }
}

// ---- the mod thread: the textures at the size the worker wants, the errors logged
void MakeTextures() {
    uint32_t w = g_wantW, h = g_wantH;
    if (!w || !h || !H->texture_d3d12_device()) return;
    std::lock_guard<std::mutex> lk(g_texMx);   // (the worker is not working meanwhile: its imports can go)
    if (w == g_texW && h == g_texH) return;
    if (g_dev) vk.vkDeviceWaitIdle(g_dev);
    for (Shared &s : g_tex) Free(s.img), s.importedGen = 0, s.handle = nullptr;
    g_texW = g_texH = 0;
    for (uint32_t i = 0; i < dnr::kTextures; i++) {
        MmlSideTexture t{};
        t.size = sizeof t;
        if (!H->texture_create(M, dnr::kTexNames[i], w, h, dnr::kTexFormats[i], &t)) {
            Error(std::string("texture_create ") + dnr::kTexNames[i] + " failed");
            return;
        }
        g_tex[i].handle = t.shared_handle;
        g_tex[i].gen = t.generation;
    }
    g_texW = w, g_texH = h;
}

void OnFrame(MmlSideMod *, const char *, const char *, void *) {
    if (g_ready && (g_wantW != g_texW || g_wantH != g_texH)) MakeTextures();
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
    nlohmann::json j = {{"stage", StageName(s.stage)}, {"message", s.message}, {"gpu", s.gpu},
                        {"driver", std::to_string(s.driver[0]) + "." + std::to_string(s.driver[1])},
                        {"dll", s.dll}, {"dll_path", s.dllPath}, {"ngx_init", NgxText(NVSDK_NGX_Result(s.ngxResult))},
                        {"available", s.available != 0}, {"needs_driver", s.needsDriver != 0},
                        {"min_driver", std::to_string(s.minDriver[0]) + "." + std::to_string(s.minDriver[1])},
                        {"support_bits", s.supportBits}, {"min_arch", arch}, {"feature_init", init},
                        {"ready", g_ready.load()}, {"broken", g_broken.load()}, {"evaluations", g_evals.load()},
                        {"copies", g_copies.load()}, {"failures", g_failures.load()}, {"textures", {g_texW, g_texH}},
                        {"feature", {g_featW, g_featH}}, {"work_ms", std::round(g_workMs.load() * 1000) / 1000},
                        {"gpu_ms", std::round(g_gpuMs.load() * 1000) / 1000}, {"error", LastError()}};
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
    if (g_feature) NVSDK_NGX_VULKAN_ReleaseFeature(g_feature), g_feature = nullptr;
    if (g_params) NVSDK_NGX_VULKAN_DestroyParameters(g_params), g_params = nullptr;
    if (g_ngx) NVSDK_NGX_VULKAN_Shutdown1(g_dev), g_ngx = false;
    if (g_dev) {
        for (Image &i : g_local) Free(i);
        for (Shared &s : g_tex) Free(s.img);
        if (g_timer) vk.vkDestroyQueryPool(g_dev, g_timer, nullptr);
        if (g_fence) vk.vkDestroyFence(g_dev, g_fence, nullptr);
        if (g_pool) vk.vkDestroyCommandPool(g_dev, g_pool, nullptr);
        vk.vkDestroyDevice(g_dev, nullptr);
        g_dev = VK_NULL_HANDLE;
    }
    for (uint32_t i = 0; i < dnr::kTextures; i++)
        if (g_tex[i].handle) H->texture_remove(M, dnr::kTexNames[i]), g_tex[i].handle = nullptr;
    if (g_inst) vk.vkDestroyInstance(g_inst, nullptr), g_inst = VK_NULL_HANDLE;
    if (g_vkDll) FreeLibrary(g_vkDll), g_vkDll = nullptr;
}
} // namespace

MML_EXPORT uint32_t mml_side_api(void) { return MML_SIDE_API_VERSION; }

MML_EXPORT int mml_side_load(const MmlSideHost *host, MmlSideMod *mod) {
    H = host;
    M = mod;
    if (!MML_SIDE_HAS(host, channel_wait)) return 1;   // (a sidecar older than this half)
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
    H->command(mod, "side", "{} -> what NGX said (stage, message, gpu, driver, dll, available, min_arch...), evaluations, work_ms, gpu_ms", Side, nullptr);
    return 0;
}

MML_EXPORT void mml_side_unload(MmlSideMod *) { Shutdown(); }
