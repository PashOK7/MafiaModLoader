"""patch.py: MafiaModLoader's changes to NVIDIA Flow 2's sources (third_party/_deps/flow), applied before Flow is built
(CMakeLists.txt runs it after tools/deps.py flow). Each change replaces one exact piece of Flow's text; one already
made is left as it is, so the script may run at every configure. Flow's license (BSD 3-Clause) allows them.

    python third_party/flow/patch.py [flow folder]

1. A Vulkan pipeline cache kept in a file (Flow makes its several hundred compute pipelines with none: the driver's
   own disk cache is kept by the executable's path, and every game folder has its own mml-sidecar64.exe, rebuilt with
   the loader: each such start had the driver compile them all again, 1.5 to 5 minutes). When the environment
   variable NV_FLOW_VK_PIPELINE_CACHE names a file, the device starts its cache from it (if the file was made by the
   same GPU and driver: the header's vendor, device and pipelineCacheUUID; else it starts empty and the driver
   checks nothing it did not make), gives it to every pipeline, and writes it back (a .tmp file renamed into its
   place) 2 s after the last new pipeline, from its queue's flush, and when the device is destroyed: the sidecar
   ends with the game (a job object kills it), so the file must not wait for an orderly end.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FLOW = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'third_party', '_deps', 'flow')
VK = os.path.join(FLOW, 'source', 'nvflowext', 'vulkan')

EDITS = [
    # the loader's pointers: the three pipeline cache functions
    ('LoaderVulkan.h', '    NV_FLOW_VK_LOADER_PTR(vkCreateComputePipelines);\n',
     '    NV_FLOW_VK_LOADER_PTR(vkCreateComputePipelines);\n'
     '    NV_FLOW_VK_LOADER_PTR(vkCreatePipelineCache);       // (MML: the pipeline cache, third_party/flow/patch.py)\n'
     '    NV_FLOW_VK_LOADER_PTR(vkDestroyPipelineCache);\n'
     '    NV_FLOW_VK_LOADER_PTR(vkGetPipelineCacheData);\n'),
    ('LoaderVulkan.h', '    NV_FLOW_VK_LOADER_DEVICE(vkCreateComputePipelines);\n',
     '    NV_FLOW_VK_LOADER_DEVICE(vkCreateComputePipelines);\n'
     '    NV_FLOW_VK_LOADER_DEVICE(vkCreatePipelineCache);    // (MML)\n'
     '    NV_FLOW_VK_LOADER_DEVICE(vkDestroyPipelineCache);\n'
     '    NV_FLOW_VK_LOADER_DEVICE(vkGetPipelineCacheData);\n'),
    ('DeviceVulkan.cpp', '#include "CommonVulkan.h"\n',
     '#include "CommonVulkan.h"\n#include <stdio.h>   // (MML: the pipeline cache\'s file)\n#include <wchar.h>\n'),
    # the device: its cache, the file, what is new since the last write
    ('DeviceVulkan.h', '        NvFlowDeviceMemoryStats memoryStats = {};\n    };\n',
     '        NvFlowDeviceMemoryStats memoryStats = {};\n'
     '\n'
     '        // (MML: the pipeline cache kept in a file, third_party/flow/patch.py)\n'
     '        VkPipelineCache pipelineCache = VK_NULL_HANDLE;\n'
     '        wchar_t pipelineCachePath[1024] = {};\n'
     '        NvFlowUint64 pipelinesUnsaved = 0u;\n'
     '        NvFlowUint64 lastPipelineMs = 0u;\n'
     '        NvFlowUint64 pipelineCacheBytes = 0u;   // what the file holds (read or last written)\n'
     '    };\n'
     '\n'
     '    void device_pipelineCacheCreate(Device* device);                    // (MML)\n'
     '    void device_pipelineCacheSave(Device* device, NvFlowBool32 force);   // (MML)\n'),
    ('DeviceVulkan.cpp', '    ptr->deviceQueue = deviceQueue_create(ptr);\n\n    return cast(ptr);\n}\n',
     '    ptr->deviceQueue = deviceQueue_create(ptr);\n'
     '\n'
     '    device_pipelineCacheCreate(ptr);   // (MML)\n'
     '\n'
     '    return cast(ptr);\n'
     '}\n'
     '\n'
     '// (MML: the pipeline cache kept in a file; third_party/flow/patch.py says why)\n'
     'void device_pipelineCacheCreate(Device* ptr)\n'
     '{\n'
     '#if defined(_WIN32)\n'
     '    DWORD length = GetEnvironmentVariableW(L"NV_FLOW_VK_PIPELINE_CACHE", ptr->pipelineCachePath, 1024u);\n'
     '    if (!length || length >= 1024u)\n'
     '    {\n'
     '        ptr->pipelineCachePath[0] = 0;\n'
     '        return;\n'
     '    }\n'
     '    NvFlowArray<unsigned char> data;\n'
     '    FILE* file = nullptr;\n'
     '    _wfopen_s(&file, ptr->pipelineCachePath, L"rb");\n'
     '    if (file)\n'
     '    {\n'
     '        fseek(file, 0, SEEK_END);\n'
     '        long size = ftell(file);\n'
     '        fseek(file, 0, SEEK_SET);\n'
     '        if (size > 32)\n'
     '        {\n'
     '            data.reserve((NvFlowUint64)size);\n'
     '            data.size = (NvFlowUint64)size;\n'
     '            if (fread(data.data, 1u, (size_t)size, file) != (size_t)size)\n'
     '            {\n'
     '                data.size = 0u;\n'
     '            }\n'
     '        }\n'
     '        fclose(file);\n'
     '    }\n'
     '    // the header (VkPipelineCacheHeaderVersionOne): the same GPU and driver, or nothing of it\n'
     '    bool other = false;\n'
     '    if (data.size > 32u)\n'
     '    {\n'
     '        uint32_t header[4];\n'
     '        memcpy(header, data.data, sizeof(header));\n'
     '        const VkPhysicalDeviceProperties* p = &ptr->physicalDeviceProperties;\n'
     '        if (header[0] < 32u || header[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || header[2] != p->vendorID ||\n'
     '            header[3] != p->deviceID || memcmp(data.data + 16, p->pipelineCacheUUID, VK_UUID_SIZE) != 0)\n'
     '        {\n'
     '            other = true;\n'
     '            data.size = 0u;\n'
     '        }\n'
     '    }\n'
     '    VkPipelineCacheCreateInfo info = {};\n'
     '    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;\n'
     '    info.initialDataSize = (size_t)data.size;\n'
     '    info.pInitialData = data.size ? data.data : nullptr;\n'
     '    if (ptr->loader.vkCreatePipelineCache(ptr->vulkanDevice, &info, nullptr, &ptr->pipelineCache) != VK_SUCCESS)\n'
     '    {\n'
     '        ptr->pipelineCache = VK_NULL_HANDLE;\n'
     '    }\n'
     '    ptr->pipelineCacheBytes = data.size;\n'
     '    ptr->logPrint(eNvFlowLogLevel_info, other ? "pipeline cache: another GPU\'s or driver\'s file, started empty" :\n'
     '                  "pipeline cache: %llu bytes read", (unsigned long long)data.size);\n'
     '#endif\n'
     '}\n'
     '\n'
     'void device_pipelineCacheSave(Device* ptr, NvFlowBool32 force)\n'
     '{\n'
     '#if defined(_WIN32)\n'
     '    if (!ptr->pipelineCache || !ptr->pipelinesUnsaved)\n'
     '    {\n'
     '        return;\n'
     '    }\n'
     '    if (!force && GetTickCount64() - ptr->lastPipelineMs < 2000u)\n'
     '    {\n'
     '        return;   // (pipelines are still being made: once they stop)\n'
     '    }\n'
     '    ptr->pipelinesUnsaved = 0u;\n'
     '    size_t size = 0u;\n'
     '    if (ptr->loader.vkGetPipelineCacheData(ptr->vulkanDevice, ptr->pipelineCache, &size, nullptr) != VK_SUCCESS || !size ||\n'
     '        size == ptr->pipelineCacheBytes)\n'
     '    {\n'
     '        return;   // (nothing, or nothing new: every pipeline came from the file)\n'
     '    }\n'
     '    NvFlowArray<unsigned char> data;\n'
     '    data.reserve(size);\n'
     '    data.size = size;\n'
     '    if (ptr->loader.vkGetPipelineCacheData(ptr->vulkanDevice, ptr->pipelineCache, &size, data.data) != VK_SUCCESS)\n'
     '    {\n'
     '        return;\n'
     '    }\n'
     '    wchar_t tmp[1040];\n'
     '    swprintf_s(tmp, L"%ls.tmp", ptr->pipelineCachePath);\n'
     '    FILE* file = nullptr;\n'
     '    _wfopen_s(&file, tmp, L"wb");\n'
     '    if (!file)\n'
     '    {\n'
     '        return;\n'
     '    }\n'
     '    bool whole = fwrite(data.data, 1u, size, file) == size;\n'
     '    whole = fclose(file) == 0 && whole;\n'
     '    if (whole && MoveFileExW(tmp, ptr->pipelineCachePath, MOVEFILE_REPLACE_EXISTING))\n'
     '    {\n'
     '        ptr->pipelineCacheBytes = size;\n'
     '        ptr->logPrint(eNvFlowLogLevel_info, "pipeline cache: %llu bytes written", (unsigned long long)size);\n'
     '    }\n'
     '    else\n'
     '    {\n'
     '        DeleteFileW(tmp);\n'
     '    }\n'
     '#endif\n'
     '}\n'),
    ('DeviceVulkan.cpp', '    deviceQueue_destroy(ptr->deviceQueue);\n\n    ptr->loader.vkDestroyDevice(ptr->vulkanDevice, nullptr);\n',
     '    deviceQueue_destroy(ptr->deviceQueue);\n'
     '\n'
     '    if (ptr->pipelineCache)   // (MML)\n'
     '    {\n'
     '        device_pipelineCacheSave(ptr, NV_FLOW_TRUE);\n'
     '        ptr->loader.vkDestroyPipelineCache(ptr->vulkanDevice, ptr->pipelineCache, nullptr);\n'
     '    }\n'
     '\n'
     '    ptr->loader.vkDestroyDevice(ptr->vulkanDevice, nullptr);\n'),
    ('DeviceVulkan.cpp', '    int ret = flushStepA(ptr, waitSemaphore, signalSemaphore);\n    flushStepB(ptr);\n',
     '    int ret = flushStepA(ptr, waitSemaphore, signalSemaphore);\n'
     '    flushStepB(ptr);\n'
     '    device_pipelineCacheSave(ptr->device, NV_FLOW_FALSE);   // (MML: once new pipelines stop coming)\n'),
    # every pipeline made through the cache
    ('ComputePipelineVulkan.cpp', '        loader->vkCreateComputePipelines(\n            vulkanDevice,\n            VK_NULL_HANDLE,\n',
     '        auto device = context->deviceQueue->device;   // (MML: through the pipeline cache)\n'
     '        device->pipelinesUnsaved++;\n'
     '#if defined(_WIN32)\n'
     '        device->lastPipelineMs = GetTickCount64();\n'
     '#endif\n'
     '        loader->vkCreateComputePipelines(\n'
     '            vulkanDevice,\n'
     '            device->pipelineCache,\n'),
]


def main():
    # Each file is made again from Flow's own text, kept beside it as <file>.mml-orig at the first patch (so a newer
    # patch.py replaces an older one's changes), and written only when the result differs from what is there.
    written = 0
    names = sorted({e[0] for e in EDITS})
    for name in names:
        path = os.path.join(VK, name)
        orig = path + '.mml-orig'
        if not os.path.exists(orig):
            text = open(path, encoding='utf-8', newline='').read()
            if '(MML' in text:
                sys.exit('flow patch: %s holds an older patch and no %s: python tools/deps.py --force flow' % (name, orig))
            open(orig, 'w', encoding='utf-8', newline='').write(text)
        text = open(orig, encoding='utf-8', newline='').read()
        crlf = '\r\n' in text
        t = text.replace('\r\n', '\n')
        for file, old, new in EDITS:
            if file != name:
                continue
            if t.count(old) != 1:
                sys.exit('flow patch: %s: the piece to change is not there once (%d): Flow changed?' % (name, t.count(old)))
            t = t.replace(old, new)
        t = t.replace('\n', '\r\n') if crlf else t
        if open(path, encoding='utf-8', newline='').read() != t:
            open(path, 'w', encoding='utf-8', newline='').write(t)
            written += 1
    print('flow patch: %d of %d files written (%d changes)' % (written, len(names), len(EDITS)))


main()
