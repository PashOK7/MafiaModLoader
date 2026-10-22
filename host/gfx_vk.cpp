// The graphics bridge on DXVK: Direct3D 9 on Vulkan (see gfx_vk.h).
//
// Found 2026-10-19 in a player's game: DXVK's d3d9.dll beside Game.exe (dxwrapper loads the game folder's d3d9.dll
// before the system's: LoadLibraryA by its bare name), for ReShade on Vulkan. DXVK makes a Direct3D 9 shared texture
// from Vulkan memory exported as VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT (d3d9_common_texture.cpp), which
// only another Vulkan device of the same driver can import: Direct3D 11's OpenSharedResource said E_INVALIDARG, and
// the bridge was off. The way round, through DXVK's own interop interfaces (d3d9_interfaces.h, DXVK 2.1 and later):
//   - ID3D9VkInteropDevice gives DXVK's VkInstance, VkPhysicalDevice, VkDevice and its rendering queue, flushes its
//     command stream and locks the queue for a submission of ours;
//   - ID3D9VkInteropTexture gives a Direct3D 9 texture's VkImage and the layout DXVK keeps it in.
// Each shared texture is then two: the Direct3D 9 one (DXVK's image) and a Direct3D 11 one made shared by Direct3D 11
// (a KMT handle, or an NT handle where the driver imports only those), imported into DXVK's device
// (VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE(_KMT)_BIT, a dedicated allocation). At each hand-over a command buffer
// of ours on DXVK's queue copies one into the other: DXVK's image taken from its layout and given back to it, the
// imported image taken from and given back to VK_QUEUE_FAMILY_EXTERNAL in GENERAL. The bridge already waits for each
// device on the CPU at the hand-overs (event queries), so each copy is waited for by a fence: nothing of DXVK's runs
// between (the game thread is the device's only user, and the event query it waited for was DXVK's last work).
#include "host.h"
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <vector>
#include "gfx_vk.h"

namespace host {
namespace gfxvk {
namespace {

// DXVK's interop interfaces (src/d3d9/d3d9_interfaces.h, zlib): only the methods used, in their order.
struct __declspec(uuid("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")) ID3D9VkInteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance *instance, VkPhysicalDevice *physDev, VkDevice *device) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue *queue, uint32_t *queueIndex, uint32_t *queueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(IUnknown *texture, const VkImageSubresourceRange *subresources,
                                                           VkImageLayout oldLayout, VkImageLayout newLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};
struct __declspec(uuid("d56344f5-8d35-46fd-806d-94c351b472c1")) ID3D9VkInteropTexture : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage *handle, VkImageLayout *layout, VkImageCreateInfo *info) = 0;
};

#define VK_FUNCS(X)                                                                                                    \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)             \
    X(vkBindImageMemory) X(vkGetMemoryWin32HandlePropertiesKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool)         \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer)                 \
    X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences)   \
    X(vkResetFences)
struct Vk {
#define X(f) PFN_##f f = nullptr;
    VK_FUNCS(X)
#undef X
    PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 vkGetPhysicalDeviceImageFormatProperties2 = nullptr;
} vk;

uint64_t g_waitNs = 250ull * 1000 * 1000;   // a copy not done in this time: the frame goes on without it (Busy)

ID3D9VkInteropDevice *g_interop;
VkInstance g_inst;
VkPhysicalDevice g_pd;
VkDevice g_dev;
VkQueue g_queue;
uint32_t g_family;
VkPhysicalDeviceMemoryProperties g_mem;
VkExternalMemoryHandleTypeFlagBits g_handleType;
VkCommandPool g_pool;
VkCommandBuffer g_cmd;
VkFence g_fence;
bool g_inFlight;   // a copy submitted and not seen done (a late one's): its command buffer and fence not reused before
int g_links;
unsigned g_copies;
double g_ms;
LARGE_INTEGER g_freq;

double NowMs() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return 1000.0 * t.QuadPart / g_freq.QuadPart; }

std::string VkErr(const char *what, VkResult r) {
    char b[160];
    snprintf(b, sizeof b, "%s failed (VkResult %d)", what, static_cast<int>(r));
    return b;
}

DXGI_FORMAT DxgiOf(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The driver imports this kind of handle for an image of this format, as the copies use it.
bool Importable(VkExternalMemoryHandleTypeFlagBits type, VkFormat format) {
    VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    ext.handleType = type;
    VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    info.pNext = &ext;
    info.format = format;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkExternalImageFormatProperties extOut{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 out{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    out.pNext = &extOut;
    return vk.vkGetPhysicalDeviceImageFormatProperties2(g_pd, &info, &out) == VK_SUCCESS &&
           (extOut.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}

// The copy in flight waited for up to ns: false while it runs (a late copy's command buffer and fence stay as they are
// until it is done: neither is reset while the GPU holds them).
bool Settle(uint64_t ns, std::string *why) {
    if (!g_inFlight) return true;
    VkResult r = vk.vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, ns);
    if (r == VK_TIMEOUT) { *why = "the copy on DXVK's queue did not finish in time"; return false; }
    g_inFlight = false;
    vk.vkResetFences(g_dev, 1, &g_fence);
    if (r != VK_SUCCESS) { *why = VkErr("vkWaitForFences", r); return false; }
    return true;
}

// One command buffer on DXVK's queue, waited for. record() fills it.
template <class F> bool Submit(F record, std::string *why) {
    if (!Settle(0, why)) return false;   // (the caller waits for a late copy first: Busy)
    vk.vkResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult r = vk.vkBeginCommandBuffer(g_cmd, &bi);
    if (r != VK_SUCCESS) { *why = VkErr("vkBeginCommandBuffer", r); return false; }
    record(g_cmd);
    if ((r = vk.vkEndCommandBuffer(g_cmd)) != VK_SUCCESS) { *why = VkErr("vkEndCommandBuffer", r); return false; }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    g_interop->LockSubmissionQueue();   // (no Direct3D 9 call while it is held)
    r = vk.vkQueueSubmit(g_queue, 1, &si, g_fence);
    g_interop->ReleaseSubmissionQueue();
    if (r != VK_SUCCESS) { *why = VkErr("vkQueueSubmit on DXVK's queue", r); return false; }
    g_inFlight = true;
    return Settle(g_waitNs, why);
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                             uint32_t qfFrom = VK_QUEUE_FAMILY_IGNORED, uint32_t qfTo = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = qfFrom;
    b.dstQueueFamilyIndex = qfTo;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}
} // namespace

struct Link {
    ID3D9VkInteropTexture *tex9 = nullptr;
    VkImage image9 = VK_NULL_HANDLE;   // DXVK's
    ID3D11Texture2D *t11 = nullptr;    // (a reference: freed after the memory imported from it)
    VkImage image = VK_NULL_HANDLE;    // the Direct3D 11 texture's memory, imported
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkExtent3D extent{};
    bool counted = false;              // (in g_links)
};

bool IsDxvk(IDirect3DDevice9 *dev9) {
    ID3D9VkInteropDevice *i = nullptr;
    if (!dev9 || FAILED(dev9->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void **>(&i))) || !i) return false;
    i->Release();
    return true;
}

bool On() { return g_interop != nullptr; }

bool Start(IDirect3DDevice9 *dev9, std::string *why) {
    why->clear();
    if (g_interop) return true;
    if (!dev9 || FAILED(dev9->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void **>(&g_interop))) || !g_interop) {
        g_interop = nullptr;
        return false;   // not DXVK
    }
    QueryPerformanceFrequency(&g_freq);
    auto fail = [&](const std::string &w) { *why = "Direct3D 9 is DXVK's: " + w; Stop(); return false; };
    HMODULE vkDll = GetModuleHandleW(L"vulkan-1.dll");   // (DXVK's own: loaded by it)
    auto gipa = vkDll ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vkDll, "vkGetInstanceProcAddr")) : nullptr;
    if (!gipa) return fail("no vulkan-1.dll with vkGetInstanceProcAddr in the game");
    uint32_t index = 0;
    g_interop->GetVulkanHandles(&g_inst, &g_pd, &g_dev);
    g_interop->GetSubmissionQueue(&g_queue, &index, &g_family);
    if (!g_inst || !g_pd || !g_dev || !g_queue) return fail("its interop interface gave no Vulkan device or queue");
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(g_inst, "vkGetDeviceProcAddr"));
    vk.vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(g_inst, "vkGetPhysicalDeviceMemoryProperties"));
    vk.vkGetPhysicalDeviceImageFormatProperties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(gipa(g_inst, "vkGetPhysicalDeviceImageFormatProperties2"));
    if (!gdpa || !vk.vkGetPhysicalDeviceMemoryProperties || !vk.vkGetPhysicalDeviceImageFormatProperties2)
        return fail("its Vulkan instance lacks a function the copies need");
    std::string missing;
#define X(f) vk.f = reinterpret_cast<PFN_##f>(gdpa(g_dev, #f)); if (!vk.f) missing += std::string(missing.empty() ? "" : ", ") + #f;
    VK_FUNCS(X)
#undef X
    if (!missing.empty())
        return fail("its Vulkan device lacks " + missing + (vk.vkGetMemoryWin32HandlePropertiesKHR ? "" :
                    " (VK_KHR_external_memory_win32 not enabled: a DXVK older than 2.0, or a driver without it)"));
    vk.vkGetPhysicalDeviceMemoryProperties(g_pd, &g_mem);
    // the kind of handle the driver imports a Direct3D 11 texture by: the legacy (KMT) one, else an NT handle
    if (Importable(VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, VK_FORMAT_B8G8R8A8_UNORM))
        g_handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    else if (Importable(VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, VK_FORMAT_B8G8R8A8_UNORM))
        g_handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    else
        return fail("its Vulkan driver imports no Direct3D 11 texture");
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_family;
    VkResult r = vk.vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool);
    if (r != VK_SUCCESS) return fail(VkErr("vkCreateCommandPool", r));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if ((r = vk.vkAllocateCommandBuffers(g_dev, &cai, &g_cmd)) != VK_SUCCESS) return fail(VkErr("vkAllocateCommandBuffers", r));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((r = vk.vkCreateFence(g_dev, &fci, nullptr, &g_fence)) != VK_SUCCESS) return fail(VkErr("vkCreateFence", r));
    g_copies = 0;
    g_ms = 0;
    return true;
}

void Stop() {
    std::string why;
    if (g_dev && !Settle(2000ull * 1000 * 1000, &why) && g_inFlight) {
        HWARN("gfx: a copy on DXVK's queue still runs after 2 s: its command buffer and fence are left to it");
        g_fence = VK_NULL_HANDLE;
        g_pool = VK_NULL_HANDLE;
    }
    g_inFlight = false;
    if (g_dev) {
        if (g_fence) vk.vkDestroyFence(g_dev, g_fence, nullptr);
        if (g_pool) vk.vkDestroyCommandPool(g_dev, g_pool, nullptr);   // (its command buffer with it)
    }
    g_fence = VK_NULL_HANDLE;
    g_pool = VK_NULL_HANDLE;
    g_cmd = VK_NULL_HANDLE;
    g_inst = VK_NULL_HANDLE;
    g_pd = VK_NULL_HANDLE;
    g_dev = VK_NULL_HANDLE;
    g_queue = VK_NULL_HANDLE;
    if (g_interop) g_interop->Release();
    g_interop = nullptr;
}

Link *Make(IDirect3DTexture9 *t9, ID3D11Device *dev11, ID3D11Texture2D **t11, std::string *why) {
    *t11 = nullptr;
    if (!g_interop || !t9 || !dev11) { *why = "the Vulkan path is not started"; return nullptr; }
    Link *l = new Link;
    auto fail = [&](const std::string &w) { *why = w; Free(l); return nullptr; };
    if (FAILED(t9->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void **>(&l->tex9))) || !l->tex9)
        return fail("DXVK's texture has no interop interface");
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (FAILED(l->tex9->GetVulkanImageInfo(&l->image9, &layout, &info)) || !l->image9) return fail("DXVK gave no image for the texture");
    const VkImageUsageFlags copies = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((info.usage & copies) != copies) return fail("DXVK's image cannot be copied (no transfer usage)");
    DXGI_FORMAT f11 = DxgiOf(info.format);
    if (f11 == DXGI_FORMAT_UNKNOWN) return fail("DXVK's image has a format the bridge does not share (VkFormat " + std::to_string(info.format) + ")");
    l->extent = {info.extent.width, info.extent.height, 1};
    // the Direct3D 11 half, shared by Direct3D 11
    const bool nt = g_handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = info.extent.width;
    td.Height = info.extent.height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = f11;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED : D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = dev11->CreateTexture2D(&td, nullptr, &l->t11);
    if (FAILED(hr)) { char b[96]; snprintf(b, sizeof b, "Direct3D 11 made no shared texture (0x%08X)", static_cast<unsigned>(hr)); return fail(b); }
    HANDLE handle = nullptr;
    if (nt) {
        IDXGIResource1 *res = nullptr;
        hr = l->t11->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&res));
        if (SUCCEEDED(hr)) hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
        if (res) res->Release();
    } else {
        IDXGIResource *res = nullptr;
        hr = l->t11->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void **>(&res));
        if (SUCCEEDED(hr)) hr = res->GetSharedHandle(&handle);
        if (res) res->Release();
    }
    if (FAILED(hr) || !handle) { char b[96]; snprintf(b, sizeof b, "the shared texture gave no handle (0x%08X)", static_cast<unsigned>(hr)); return fail(b); }
    // ... imported into DXVK's device
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes = g_handleType;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = info.format;
    ici.extent = l->extent;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = copies;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vk.vkCreateImage(g_dev, &ici, nullptr, &l->image);
    if (r != VK_SUCCESS) { if (nt) CloseHandle(handle); return fail(VkErr("vkCreateImage (the imported texture)", r)); }
    VkMemoryRequirements req{};
    vk.vkGetImageMemoryRequirements(g_dev, l->image, &req);
    VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    uint32_t bits = req.memoryTypeBits;
    if (vk.vkGetMemoryWin32HandlePropertiesKHR(g_dev, g_handleType, handle, &hp) == VK_SUCCESS && (bits & hp.memoryTypeBits))
        bits &= hp.memoryTypeBits;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < g_mem.memoryTypeCount && type == UINT32_MAX; i++)
        if ((bits & (1u << i)) && (g_mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = i;
    for (uint32_t i = 0; i < g_mem.memoryTypeCount && type == UINT32_MAX; i++)
        if (bits & (1u << i)) type = i;
    if (type == UINT32_MAX) { if (nt) CloseHandle(handle); return fail("no memory type takes the Direct3D 11 texture"); }
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = l->image;
    VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    import.pNext = &dedicated;
    import.handleType = g_handleType;
    import.handle = handle;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &import;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    r = vk.vkAllocateMemory(g_dev, &ai, nullptr, &l->memory);
    if (nt) CloseHandle(handle);   // (an import does not take the handle: Vulkan keeps its own reference)
    if (r != VK_SUCCESS) return fail(VkErr("vkAllocateMemory (importing the Direct3D 11 texture)", r));
    if ((r = vk.vkBindImageMemory(g_dev, l->image, l->memory, 0)) != VK_SUCCESS) return fail(VkErr("vkBindImageMemory", r));
    // its first layout: GENERAL, handed to Direct3D 11 (every copy takes it from there and gives it back so)
    std::string err;
    VkImage img = l->image;
    uint32_t family = g_family;
    if (!Submit([&](VkCommandBuffer c) {
            VkImageMemoryBarrier b = Barrier(img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, 0, family, VK_QUEUE_FAMILY_EXTERNAL);
            vk.vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }, &err))
        return fail(err);
    *t11 = l->t11;
    (*t11)->AddRef();   // (the caller's; the link keeps its own)
    g_links++;
    l->counted = true;
    return l;
}

void Free(Link *&l) {
    if (!l) return;
    if (g_dev) {
        if (l->image) vk.vkDestroyImage(g_dev, l->image, nullptr);
        if (l->memory) vk.vkFreeMemory(g_dev, l->memory, nullptr);
    }
    if (l->t11) l->t11->Release();
    if (l->tex9) l->tex9->Release();
    if (l->counted) g_links--;
    delete l;
    l = nullptr;
}

namespace {
// The copies of one hand-over: to Direct3D 11 (DXVK's image read, the imported one written) or back.
bool Copy(bool to11, Link *const *links, int n, std::string *why) {
    if (!g_interop) { *why = "the Vulkan path is not started"; return false; }
    double t0 = NowMs();
    g_interop->FlushRenderingCommands();   // (DXVK's command stream recorded and handed to its queue: its layouts final)
    struct Item { Link *l; VkImageLayout layout; };
    std::vector<Item> items;
    for (int i = 0; i < n; i++) {
        Link *l = links[i];
        if (!l) continue;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (FAILED(l->tex9->GetVulkanImageInfo(nullptr, &layout, nullptr))) { *why = "DXVK did not say its image's layout"; return false; }
        if (layout == VK_IMAGE_LAYOUT_UNDEFINED) continue;   // (never drawn into: nothing to hand over)
        items.push_back({l, layout});
    }
    if (items.empty()) return true;
    const VkImageLayout mine = to11 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageLayout theirs = to11 ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkAccessFlags mineAccess = to11 ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkAccessFlags theirsAccess = to11 ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
    const VkAccessFlags any = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    const uint32_t family = g_family;
    bool ok = Submit([&](VkCommandBuffer c) {
        std::vector<VkImageMemoryBarrier> pre, post;
        for (const Item &it : items) {
            // DXVK's image from the layout it keeps it in; the imported one taken from Direct3D 11
            pre.push_back(Barrier(it.l->image9, it.layout, mine, any, mineAccess));
            pre.push_back(Barrier(it.l->image, VK_IMAGE_LAYOUT_GENERAL, theirs, 0, theirsAccess, VK_QUEUE_FAMILY_EXTERNAL, family));
            post.push_back(Barrier(it.l->image9, mine, it.layout, mineAccess, any));
            post.push_back(Barrier(it.l->image, theirs, VK_IMAGE_LAYOUT_GENERAL, theirsAccess, 0, family, VK_QUEUE_FAMILY_EXTERNAL));
        }
        vk.vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                                static_cast<uint32_t>(pre.size()), pre.data());
        for (const Item &it : items) {
            VkImageCopy region{};
            region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = it.l->extent;
            if (to11) vk.vkCmdCopyImage(c, it.l->image9, mine, it.l->image, theirs, 1, &region);
            else vk.vkCmdCopyImage(c, it.l->image, theirs, it.l->image9, mine, 1, &region);
        }
        vk.vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                                static_cast<uint32_t>(post.size()), post.data());
    }, why);
    double ms = NowMs() - t0;
    g_copies++;
    g_ms = g_copies == 1 ? ms : g_ms * 0.95 + ms * 0.05;
    return ok;
}
} // namespace

bool To11(Link *const *links, int n, std::string *why) { return Copy(true, links, n, why); }
bool To9(Link *const *links, int n, std::string *why) { return Copy(false, links, n, why); }

void WaitLimit(double ms) { g_waitNs = static_cast<uint64_t>(ms * 1000000.0); }

bool Busy() {
    std::string why;
    return g_interop && !Settle(0, &why) && g_inFlight;
}

std::string Describe() {
    if (!g_interop) return "";
    char b[256];
    snprintf(b, sizeof b, "DXVK (Direct3D 9 on Vulkan): %d textures imported from Direct3D 11 (%s handles), %u hand-overs copied, %.3f ms each",
             g_links, g_handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT ? "NT" : "KMT", g_copies, g_ms);
    return b;
}

} // namespace gfxvk
} // namespace host
