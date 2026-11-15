// Water's x64 policy adapter. Shared resource lifetime and DXR work live in SDK.
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include "mml/side.h"
#include "mml/water_transport_dxr.h"
#include "river_transport.h"
#include "shaders/CsWaterTransportDxr.h"
#include "shaders/CsWaterTransportFacets.h"
#include "shaders/CsTransferUnpack.h"
#include "shaders/CsSceneBounds.h"
#include "shaders/CsSceneFacets.h"
#include "shaders/CsTransferPackOutput.h"
#include "shaders/CsWaterQueueReset.h"
#include "shaders/CsWaterQueueBuild.h"
#include "shaders/CsWaterQueueDispatch.h"
#include "shaders/CsWaterQueueTrace.h"
#include "shaders/CsWaterFirstHit.h"
#include "shaders/CsWaterQueueOpaque.h"
#include "shaders/CsWaterQueueLayeredDispatch.h"

namespace {
const MmlSideHost *H;
MmlSideMod *M;
MmlSideChannel *g_request,*g_reply,*g_status;
std::atomic<bool> g_stop=false;
std::thread g_thread;
std::mutex g_deviceMutex;
Microsoft::WRL::ComPtr<ID3D12Device> g_device;

void OnFrame(MmlSideMod *,const char *,const char *,void *){
    std::lock_guard<std::mutex> lock(g_deviceMutex);
    if(!g_device)g_device=static_cast<ID3D12Device *>(H->texture_d3d12_device());
}
void Worker(){
    LARGE_INTEGER frequency{},start{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    river::HardwareStatus status{};status.worker=uint64_t(start.QuadPart)^uint64_t(GetCurrentProcessId());if(!status.worker)status.worker=1;
    std::unique_ptr<mml::water::DxrTransport> backend;
    uint64_t seen=0;
    while(!g_stop.load()){
        if(!backend){
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            {std::lock_guard<std::mutex> lock(g_deviceMutex);device=g_device;}
            if(!device){Sleep(5);continue;}
            auto candidate=std::make_unique<mml::water::DxrTransport>();
            if(!candidate->Initialize(device.Get(),kCsWaterTransportDxr,kCsTransferUnpack,kCsSceneBounds,kCsTransferPackOutput,
                                      river::TransportSamples,kCsWaterTransportFacets,kCsSceneFacets,
                                      {kCsWaterQueueReset,kCsWaterQueueBuild,kCsWaterQueueDispatch,kCsWaterQueueTrace},
                                      {kCsWaterFirstHit,kCsWaterQueueOpaque,kCsWaterQueueLayeredDispatch})){
                status.error=candidate->Error();H->channel_write(g_status,&status,sizeof status);return;
            }
            backend=std::move(candidate);status.ready=1;H->channel_write(g_status,&status,sizeof status);
        }
        H->channel_wait(g_request,seen,5);if(g_stop.load())break;
        river::HardwareRequest work{};uint64_t version=0;
        if(H->channel_read(g_request,&work,sizeof work,&version)!=sizeof work||version<=seen)continue;
        seen=version;
        const auto &request=work.transfer;
        if(request.version==mml::gfx::TransferVersion&&request.size==sizeof request&&request.operation==uint32_t(mml::gfx::TransferOperation::Release)){
            status.ready=0;H->channel_write(g_status,&status,sizeof status);backend.reset();continue;
        }
        mml::gfx::TransferReply reply{};
        LARGE_INTEGER begin{},end{};QueryPerformanceCounter(&begin);
        const bool submitted=backend->Submit(request,reply,work.triangles,work.billboards);QueryPerformanceCounter(&end);
        status.enqueueMs=double(end.QuadPart-begin.QuadPart)*1000/double(frequency.QuadPart);
        const auto &timing=backend->Timing();status.gpuInputsMs=timing.inputs;status.gpuBoundsMs=timing.bounds;
        status.gpuTraceMs=timing.trace;status.gpuPackMs=timing.pack;
        if(submitted){
            status.submitted++;
            if(backend->FacetReused())status.facetReuses++;
        }else status.rejected++;
        status.error=reply.error;
        H->channel_write(g_reply,&reply,sizeof reply);
        H->channel_write(g_status,&status,sizeof status);
        if(reply.status==uint32_t(mml::gfx::TransferStatus::Faulted))break;
    }
    status.ready=0;H->channel_write(g_status,&status,sizeof status);
    backend.reset();
}
}
MML_EXPORT uint32_t mml_side_api(){return MML_SIDE_API_VERSION;}
MML_EXPORT int mml_side_load(const MmlSideHost *host,MmlSideMod *mod){
    H=host;M=mod;
    if(!MML_SIDE_HAS(host,channel_wait)||!MML_SIDE_HAS(host,texture_d3d12_device))return 1;
    g_request=H->channel_open(M,"hardware_request",sizeof(river::HardwareRequest));
    g_reply=H->channel_open(M,"hardware_reply",sizeof(mml::gfx::TransferReply));
    g_status=H->channel_open(M,"hardware_status",sizeof(river::HardwareStatus));
    if(!g_request||!g_reply||!g_status)return 2;
    river::HardwareStatus initial{};H->channel_write(g_status,&initial,sizeof initial);
    g_stop=false;g_thread=std::thread(Worker);H->subscribe(M,"FRAME",OnFrame,nullptr);return 0;
}
MML_EXPORT void mml_side_unload(MmlSideMod *){
    g_stop=true;if(g_thread.joinable())g_thread.join();
    std::lock_guard<std::mutex> lock(g_deviceMutex);g_device.Reset();
}
