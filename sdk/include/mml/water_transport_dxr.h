/* Persistent x64 backend for the shared native water-transport resource layout.
 * Artistic transport bytecode and its retained sample count are caller inputs.
 * Inputs arrive through TransferConsumer. Geometry, materials, texture pages,
 * waves, full shadow extent and all seed samples keep their native precision.
 * Submit acknowledges only an actually queued output signal. No CPU texels or
 * geometry are read back, and no previous-frame output becomes current.
 */
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
#include <span>
#include "mml/gpu_transfer_d3d12.h"
#include "mml/ray_scene_dxr.h"
#include "mml/ray_scene_facets.h"

namespace mml::water {
class DxrTransport {
public:
    struct GpuTiming {double inputs=0,bounds=0,trace=0,pack=0;};
private:
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device5> device_;
    Ptr<ID3D12CommandQueue> queue_;
    Ptr<ID3D12CommandAllocator> allocator_;
    Ptr<ID3D12GraphicsCommandList4> command_;
    Ptr<ID3D12RootSignature> signature_,packSignature_;
    Ptr<ID3D12PipelineState> pipeline_,facetPipeline_,packPipeline_;
    std::array<Ptr<ID3D12PipelineState>,4> queuePrograms_;
    Ptr<ID3D12PipelineState> firstHitPipeline_,opaquePipeline_,layeredDispatch_;
    Ptr<ID3D12CommandSignature> queueDispatch_;
    Ptr<ID3D12Resource> queuePixels_,queueCounts_,queueArgs_;
    Ptr<ID3D12Resource> firstKinds_,firstOpaque_;
    uint64_t queueCapacity_=0,queueFailedPixels_=0;
    uint64_t stageCapacity_=0,stageFailedPixels_=0;
    Ptr<ID3D12DescriptorHeap> heap_,samplers_,packHeap_;
    Ptr<ID3D12Resource> output_,empty_;
    Ptr<ID3D12Fence> drainFence_;
    Ptr<ID3D12QueryHeap> timingHeap_;
    Ptr<ID3D12Resource> timingRead_;
    uint64_t timingFrequency_=0;
    GpuTiming timing_{};
    uint64_t drainValue_=0;
    gfx::TransferConsumer inputs_;
    ray::DxrScene scene_;
    ray::DxrFacetScene facetScene_;
    struct FacetVersion {
        uint64_t owner=0;
        uint32_t pid=0,generation=0,triangles=0,billboards=0;
        std::array<uint64_t,6> buffers{}; // allocation id and exact content revision for each input
        bool operator==(const FacetVersion &)const=default;
    } facetVersion_;
    HRESULT facetError_=E_NOTIMPL;
    bool facetReused_=false;
    UINT width_=0,height_=0,step_=0,samples_=0;
    HRESULT error_=S_OK;
    bool faulted_=false,drainQueued_=false;
    void ReadTiming(){
        if(!timingRead_||!drainValue_||drainFence_->GetCompletedValue()<drainValue_)return;
        const D3D12_RANGE read{0,5*sizeof(uint64_t)};
        void *mapped=nullptr;
        if(FAILED(timingRead_->Map(0,&read,&mapped)))return;
        uint64_t ticks[5];std::memcpy(ticks,mapped,sizeof ticks);
        const D3D12_RANGE written{0,0};timingRead_->Unmap(0,&written);
        for(unsigned i=1;i<5;i++)if(ticks[i]<ticks[i-1])return;
        const double scale=1000.0/double(timingFrequency_);
        timing_={(ticks[1]-ticks[0])*scale,(ticks[2]-ticks[1])*scale,
                 (ticks[3]-ticks[2])*scale,(ticks[4]-ticks[3])*scale};
    }
    void Timestamp(UINT index){
        if(timingHeap_)command_->EndQuery(timingHeap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,index);
    }
    bool Buffer(Ptr<ID3D12Resource> &out,uint64_t bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state){
        D3D12_HEAP_PROPERTIES properties{};properties.Type=heap;D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;
        desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
        error_=device_->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&out));return SUCCEEDED(error_);
    }
    static void Transition(ID3D12GraphicsCommandList *command,ID3D12Resource *resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};command->ResourceBarrier(1,&barrier);
    }
    bool Program(Ptr<ID3D12PipelineState> &out,ID3D12RootSignature *signature,std::span<const uint8_t> bytes){
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=signature;ps.CS={bytes.data(),bytes.size()};
        error_=device_->CreateComputePipelineState(&ps,IID_PPV_ARGS(&out));return SUCCEEDED(error_);
    }
    bool Signature(Ptr<ID3D12RootSignature> &out,const D3D12_ROOT_SIGNATURE_DESC &description){
        Ptr<ID3DBlob> bytes,error;error_=D3D12SerializeRootSignature(&description,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&error);
        if(FAILED(error_))return false;
        error_=device_->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&out));return SUCCEEDED(error_);
    }
    bool Heap(Ptr<ID3D12DescriptorHeap> &out,D3D12_DESCRIPTOR_HEAP_TYPE type,UINT count){
        D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=type;desc.NumDescriptors=count;desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        error_=device_->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&out));return SUCCEEDED(error_);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE At(UINT slot)const{
        auto handle=heap_->GetCPUDescriptorHandleForHeapStart();handle.ptr+=size_t(slot)*step_;return handle;
    }
    bool MakeQueues(uint64_t pixels){
        constexpr auto flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        constexpr auto heap=D3D12_HEAP_TYPE_DEFAULT;
        constexpr auto uav=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if(queueCapacity_<pixels){
            Ptr<ID3D12Resource> storage;
            if(!Buffer(storage,pixels*(layeredDispatch_?12:8),heap,flags,uav))return false;
            queuePixels_=std::move(storage);queueCapacity_=pixels;
        }
        if(!queueCounts_&&!Buffer(queueCounts_,12,heap,flags,uav))return false;
        return queueArgs_||Buffer(queueArgs_,36,heap,flags,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    }
    bool MakeStageBuffers(uint64_t pixels){
        if(!layeredDispatch_||pixels==stageFailedPixels_)return false;
        if(stageCapacity_>=pixels)return true;
        constexpr auto flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        constexpr auto heap=D3D12_HEAP_TYPE_DEFAULT;
        constexpr auto uav=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Ptr<ID3D12Resource> kinds,opaque;
        if(!Buffer(kinds,pixels*4,heap,flags,uav)||!Buffer(opaque,pixels*20,heap,flags,uav)){
            stageFailedPixels_=pixels;
            return false;
        }
        firstKinds_=std::move(kinds);
        firstOpaque_=std::move(opaque);
        stageCapacity_=pixels;
        stageFailedPixels_=0;
        return true;
    }
    void DescribeQueues(UINT pixels,bool staged){
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
        view.Buffer.NumElements=(std::max)(1u,pixels*(layeredDispatch_?3u:2u));
        view.Buffer.StructureByteStride=4;
        device_->CreateUnorderedAccessView(queuePixels_.Get(),nullptr,&view,At(48));
        view.Format=DXGI_FORMAT_R32_TYPELESS;view.Buffer.StructureByteStride=0;view.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        view.Buffer.NumElements=3;device_->CreateUnorderedAccessView(queueCounts_.Get(),nullptr,&view,At(49));
        view.Buffer.NumElements=9;device_->CreateUnorderedAccessView(queueArgs_.Get(),nullptr,&view,At(50));
        view.Format=DXGI_FORMAT_UNKNOWN;
        view.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_NONE;
        view.Buffer.NumElements=staged?pixels:1;
        view.Buffer.StructureByteStride=4;
        device_->CreateUnorderedAccessView(staged?firstKinds_.Get():nullptr,nullptr,&view,At(51));
        view.Buffer.StructureByteStride=20;
        device_->CreateUnorderedAccessView(staged?firstOpaque_.Get():nullptr,nullptr,&view,At(52));
    }
    bool PrepareQueues(uint64_t pixels,bool &staged){
        // A queue dispatch is one dimensional with 64 lanes per group.
        if(!queueDispatch_||pixels>65535ull*64||pixels==queueFailedPixels_)return false;
        if(!MakeQueues(pixels)){queueFailedPixels_=pixels;return false;}
        queueFailedPixels_=0;
        staged=MakeStageBuffers(pixels);
        DescribeQueues(UINT(pixels),staged);
        return true;
    }
    void UavBarrier(ID3D12Resource *resource=nullptr){
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource=resource;command_->ResourceBarrier(1,&barrier);
    }
    void DispatchQueues(const gfx::TransferRequest &request,UINT sample,bool staged){
        constexpr auto uav=D3D12_RESOURCE_STATE_UNORDERED_ACCESS,args=D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
        command_->SetPipelineState(queuePrograms_[0].Get());command_->Dispatch(1,1,1);UavBarrier(queueCounts_.Get());
        command_->SetPipelineState(queuePrograms_[1].Get());command_->Dispatch((request.width+7)/8,(request.height+7)/8,1);UavBarrier();
        Transition(command_.Get(),queueArgs_.Get(),args,uav);
        command_->SetPipelineState(queuePrograms_[2].Get());command_->Dispatch(1,1,1);
        Transition(command_.Get(),queueArgs_.Get(),uav,args);
        if(staged){
            // Resolve escape and opaque upward rays in a shader without the
            // layered traversal. The first hit puts uncertain rays in queue 2.
            command_->SetPipelineState(firstHitPipeline_.Get());
            command_->ExecuteIndirect(queueDispatch_.Get(),1,queueArgs_.Get(),0,nullptr,0);
            UavBarrier();
            command_->SetPipelineState(opaquePipeline_.Get());
            command_->ExecuteIndirect(queueDispatch_.Get(),1,queueArgs_.Get(),0,nullptr,0);
            UavBarrier(output_.Get());
            Transition(command_.Get(),queueArgs_.Get(),args,uav);
            command_->SetPipelineState(layeredDispatch_.Get());
            command_->Dispatch(1,1,1);
            Transition(command_.Get(),queueArgs_.Get(),uav,args);
        }
        command_->SetPipelineState(queuePrograms_[3].Get());
        // The remaining queues write disjoint pixels. Keep sample order.
        for(UINT arm=0;arm<2;arm++){
            const UINT queue=staged&&arm==0?2:arm;
            const uint32_t values[]{request.width,request.height,sample,queue};command_->SetComputeRoot32BitConstants(1,4,values,0);
            command_->ExecuteIndirect(queueDispatch_.Get(),1,queueArgs_.Get(),queue*12,nullptr,0);
        }
        UavBarrier();
    }
    bool Descriptors(const gfx::TransferRequest &request,UINT facets){
        const unsigned words[]{12,16,17,18,19,20,21,23,24,25,26,27,28,29,30,31,32,100,101,103,104};
        const unsigned images[]{2,6,7,8,11,33,34,35};
        for(unsigned binding:words)if(!inputs_.Resource(binding,gfx::TransferKind::Words)){error_=E_INVALIDARG;return false;}
        for(unsigned binding:images)if(!inputs_.Resource(binding,gfx::TransferKind::Image)){error_=E_INVALIDARG;return false;}
        auto *shared=inputs_.Resource(0,gfx::TransferKind::Output);
        if(!shared||shared->GetDesc().Width!=request.width||shared->GetDesc().Height!=request.height||shared->GetDesc().DepthOrArraySize!=3){error_=E_INVALIDARG;return false;}
        for(unsigned binding=0;binding<42;binding++){
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Format=DXGI_FORMAT_R32_FLOAT;
            sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;
            device_->CreateShaderResourceView(nullptr,&sv,At(binding));
        }
        for(unsigned index=0;index<request.count;index++){
            const auto &wire=request.resources[index];
            if(wire.kind==uint32_t(gfx::TransferKind::Words)){
                auto *resource=inputs_.Resource(wire.binding,gfx::TransferKind::Words);
                if(wire.binding>=100){
                    if(wire.binding>104)continue;
                    const D3D12_CONSTANT_BUFFER_VIEW_DESC cb{resource->GetGPUVirtualAddress(),UINT(resource->GetDesc().Width)};
                    device_->CreateConstantBufferView(&cb,At(43+wire.binding-100));
                }else{
                    if(wire.binding>=40){error_=E_INVALIDARG;return false;}
                    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    sv.Buffer.StructureByteStride=wire.stride;sv.Buffer.NumElements=wire.elements;device_->CreateShaderResourceView(resource,&sv,At(wire.binding));
                }
            }else if(wire.kind==uint32_t(gfx::TransferKind::Image)){
                if(wire.binding>=40){error_=E_INVALIDARG;return false;}
                D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT(wire.viewFormat);sv.ViewDimension=D3D12_SRV_DIMENSION(wire.viewDimension);
                sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if(sv.ViewDimension==D3D12_SRV_DIMENSION_TEXTURECUBE)sv.TextureCube.MipLevels=wire.mips;
                else if(sv.ViewDimension==D3D12_SRV_DIMENSION_TEXTURE2DARRAY){sv.Texture2DArray.MipLevels=wire.mips;sv.Texture2DArray.ArraySize=wire.layers;}
                else sv.Texture2D.MipLevels=wire.mips;
                device_->CreateShaderResourceView(inputs_.Resource(wire.binding,gfx::TransferKind::Image),&sv,At(wire.binding));
            }
        }
        const D3D12_CONSTANT_BUFFER_VIEW_DESC empty{empty_->GetGPUVirtualAddress(),256};device_->CreateConstantBufferView(&empty,At(45));
        D3D12_SHADER_RESOURCE_VIEW_DESC dummy{};dummy.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;dummy.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dummy.Buffer.NumElements=facets?facets:1;dummy.Buffer.StructureByteStride=8;
        device_->CreateShaderResourceView(facets?facetScene_.Mapping():empty_.Get(),&dummy,At(41));
        D3D12_SHADER_RESOURCE_VIEW_DESC as{};as.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        as.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        as.RaytracingAccelerationStructure.Location=(facets?facetScene_.Resource():scene_.Resource())->GetGPUVirtualAddress();
        device_->CreateShaderResourceView(nullptr,&as,At(40));
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uv.Buffer.NumElements=request.width*request.height*3;uv.Buffer.StructureByteStride=16;
        device_->CreateUnorderedAccessView(output_.Get(),nullptr,&uv,At(42));
        const auto samplerStep=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        for(unsigned binding=2;binding<=3;binding++){
            const gfx::TransferSampler *source=nullptr;
            for(unsigned index=0;index<request.samplerCount;index++)if(request.samplers[index].binding==binding)source=&request.samplers[index];
            if(!source){error_=E_INVALIDARG;return false;}
            D3D12_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER(source->filter);sampler.AddressU=D3D12_TEXTURE_ADDRESS_MODE(source->addressU);
            sampler.AddressV=D3D12_TEXTURE_ADDRESS_MODE(source->addressV);sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE(source->addressW);
            sampler.MaxAnisotropy=source->maxAnisotropy;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC(source->comparison);
            sampler.MipLODBias=source->mipBias;sampler.MinLOD=source->minLod;sampler.MaxLOD=source->maxLod;std::copy_n(source->border,4,sampler.BorderColor);
            auto cpu=samplers_->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=size_t(binding-2)*samplerStep;device_->CreateSampler(&sampler,cpu);
        }
        return true;
    }
    void Pack(const gfx::TransferRequest &request){
        auto *shared=inputs_.Resource(0,gfx::TransferKind::Output);
        auto cpu=packHeap_->GetCPUDescriptorHandleForHeapStart();D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.StructureByteStride=16;sv.Buffer.NumElements=request.width*request.height*3;
        device_->CreateShaderResourceView(output_.Get(),&sv,cpu);cpu.ptr+=step_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;uv.Texture2DArray.ArraySize=3;
        device_->CreateUnorderedAccessView(shared,nullptr,&uv,cpu);
        Transition(command_.Get(),output_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(command_.Get(),shared,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ID3D12DescriptorHeap *heaps[]{packHeap_.Get()};command_->SetDescriptorHeaps(1,heaps);command_->SetComputeRootSignature(packSignature_.Get());command_->SetPipelineState(packPipeline_.Get());
        command_->SetComputeRootDescriptorTable(0,packHeap_->GetGPUDescriptorHandleForHeapStart());
        const uint32_t size[]{request.width,request.height,3,0};command_->SetComputeRoot32BitConstants(1,4,size,0);command_->Dispatch((request.width+15)/16,(request.height+15)/16,1);
        Transition(command_.Get(),shared,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
        Transition(command_.Get(),output_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
public:
    DxrTransport()=default;
    DxrTransport(const DxrTransport &)=delete;
    DxrTransport &operator=(const DxrTransport &)=delete;
    ~DxrTransport(){while(!Drain(2000)){} }
    bool Drain(DWORD timeout){
        if(!queue_||!drainFence_||!drainValue_)return true;
        if(FAILED(device_->GetDeviceRemovedReason()))return true;
        if(!drainQueued_){
            drainQueued_=SUCCEEDED(queue_->Signal(drainFence_.Get(),drainValue_));
            if(!drainQueued_){Sleep(1);return false;}
        }
        if(drainFence_->GetCompletedValue()>=drainValue_)return true;
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return false;
        const HRESULT result=drainFence_->SetEventOnCompletion(drainValue_,event);
        const bool finished=SUCCEEDED(result)&&WaitForSingleObject(event,timeout)==WAIT_OBJECT_0;
        CloseHandle(event);return finished;
    }
    bool Initialize(ID3D12Device *device,std::span<const uint8_t> transport,std::span<const uint8_t> unpack,
                    std::span<const uint8_t> bounds,std::span<const uint8_t> pack,UINT samples,
                    std::span<const uint8_t> facetTransport={},std::span<const uint8_t> facetBounds={},
                    const std::array<std::span<const uint8_t>,4> &queuePrograms={},
                    const std::array<std::span<const uint8_t>,3> &stagedPrograms={}){
        if(device_||!device||!samples||samples>16)return false;
        error_=device->QueryInterface(IID_PPV_ARGS(&device_));if(FAILED(error_))return false;
        if(!inputs_.Initialize(device,unpack)){error_=inputs_.Error();return false;}
        if(!scene_.Initialize(device,bounds)){error_=scene_.Error();return false;}
        D3D12_COMMAND_QUEUE_DESC qd{};error_=device_->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue_));if(FAILED(error_))return false;
        D3D12_QUERY_HEAP_DESC queries{};queries.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;queries.Count=5;
        if(SUCCEEDED(queue_->GetTimestampFrequency(&timingFrequency_))&&timingFrequency_&&
           SUCCEEDED(device_->CreateQueryHeap(&queries,IID_PPV_ARGS(&timingHeap_)))){
            if(!Buffer(timingRead_,5*sizeof(uint64_t),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST))timingHeap_.Reset();
        }
        error_=device_->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&drainFence_));if(FAILED(error_))return false;
        error_=device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator_));if(FAILED(error_))return false;
        error_=device_->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator_.Get(),nullptr,IID_PPV_ARGS(&command_));if(FAILED(error_))return false;
        error_=command_->Close();if(FAILED(error_))return false;
        D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,42,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,42},
            {D3D12_DESCRIPTOR_RANGE_TYPE_CBV,5,0,0,43},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,5,1,0,48}};
        D3D12_DESCRIPTOR_RANGE samplerRange{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER,2,2,0,0};
        D3D12_ROOT_PARAMETER roots[3]{};roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;roots[0].DescriptorTable={4,ranges};
        roots[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;roots[1].Constants={5,0,4};
        roots[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;roots[2].DescriptorTable={1,&samplerRange};
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=3;rs.pParameters=roots;
        if(!Signature(signature_,rs)||!Program(pipeline_,signature_.Get(),transport))return false;
        if(!facetTransport.empty()){
            if(!Program(facetPipeline_,signature_.Get(),facetTransport))facetError_=error_;
            else if(!facetScene_.Initialize(device_.Get(),facetBounds)){
                facetError_=facetScene_.Error();facetPipeline_.Reset();
            }
        }
        if(facetPipeline_&&!queuePrograms.front().empty()){
            bool ready=true;
            for(size_t i=0;i<queuePrograms.size()&&ready;i++)
                ready=!queuePrograms[i].empty()&&Program(queuePrograms_[i],signature_.Get(),queuePrograms[i]);
            if(ready){
                D3D12_INDIRECT_ARGUMENT_DESC argument{};argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
                const D3D12_COMMAND_SIGNATURE_DESC description{12,1,&argument,0};
                device_->CreateCommandSignature(&description,nullptr,IID_PPV_ARGS(&queueDispatch_));
            }
        }
        if(queueDispatch_&&!stagedPrograms[0].empty()&&!stagedPrograms[1].empty()&&!stagedPrograms[2].empty()&&
           Program(firstHitPipeline_,signature_.Get(),stagedPrograms[0])&&
           Program(opaquePipeline_,signature_.Get(),stagedPrograms[1])){
            Program(layeredDispatch_,signature_.Get(),stagedPrograms[2]);
        }
        D3D12_DESCRIPTOR_RANGE packRanges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
        roots[0].DescriptorTable={2,packRanges};roots[1].Constants={0,0,4};rs.NumParameters=2;
        if(!Signature(packSignature_,rs)||!Program(packPipeline_,packSignature_.Get(),pack))return false;
        if(!Heap(heap_,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,53)||!Heap(samplers_,D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,2)||!Heap(packHeap_,D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2))return false;
        step_=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        DescribeQueues(0,false);
        if(!Buffer(empty_,256,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ))return false;
        void *mapped=nullptr;error_=empty_->Map(0,nullptr,&mapped);if(FAILED(error_))return false;
        std::memset(mapped,0,256);empty_->Unmap(0,nullptr);samples_=samples;return true;
    }
    HRESULT Error()const{return error_;}
    const GpuTiming &Timing()const{return timing_;}
    bool FacetReused()const{return facetReused_;}
    bool Submit(const gfx::TransferRequest &request,gfx::TransferReply &reply,UINT triangles=0,UINT billboards=0){
        facetReused_=false;
        reply={};reply.owner=request.owner;reply.cycle=request.cycle;reply.frame=request.frame;
        reply.generation=request.generation;reply.outputReady=request.outputReady;
        if(faulted_||!samples_){reply.error=error_;return false;}
        if(!inputs_.Prepare(request)){error_=inputs_.Error();reply.error=error_;return false;}
        ReadTiming();
        if(!request.instances||(!triangles&&billboards)||request.width>16384||request.height>16384||
           uint64_t(request.width)*request.height*48>UINT32_MAX){error_=E_INVALIDARG;reply.error=error_;return false;}
        UINT sourceFacets=0,sourceRanges=0;
        FacetVersion version{request.owner,request.producerPid,request.generation,triangles,billboards};
        if(triangles){
            if(!facetPipeline_){reply.error=facetError_;return false;}
            unsigned present=0,immutable=0;
            for(UINT i=0;i<request.count;i++){
                const auto &item=request.resources[i];
                if(item.kind!=uint32_t(gfx::TransferKind::Words))continue;
                unsigned slot;
                if(item.binding==17&&item.stride==48){sourceFacets=item.elements;slot=0;}
                else if(item.binding==19&&item.stride==96&&item.elements==request.instances)slot=1;
                else if(item.binding==38&&item.stride==sizeof(ray::DxrFacetRange)){sourceRanges=item.elements;slot=2;}
                else continue;
                present|=1u<<slot;
                if(item.flags&gfx::TransferImmutable){
                    version.buffers[slot*2]=item.id;version.buffers[slot*2+1]=item.revision;
                    immutable|=1u<<slot;
                }
            }
            if(immutable!=7)version.owner=0;
            if(present!=7||!sourceFacets||sourceRanges!=request.instances||billboards>sourceRanges||
               uint64_t(triangles)+billboards>ray::DxrFacetLimit){error_=E_INVALIDARG;reply.error=error_;return false;}
        }
        if(!output_||width_!=request.width||height_!=request.height){
            output_.Reset();if(!Buffer(output_,uint64_t(request.width)*request.height*48,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)){reply.error=error_;return false;}
            width_=request.width;height_=request.height;
        }
        bool staged=false;
        const bool compact=triangles&&PrepareQueues(uint64_t(request.width)*request.height,staged);
        error_=allocator_->Reset();if(FAILED(error_)){reply.error=error_;return false;}
        error_=command_->Reset(allocator_.Get(),nullptr);if(FAILED(error_)){reply.error=error_;return false;}
        Timestamp(0);
        const bool inputsReady=inputs_.RecordInputs(command_.Get());Timestamp(1);
        constexpr auto words=gfx::TransferKind::Words;
        auto *instances=inputs_.Resource(19,words);
        const bool reuse=triangles&&version.owner&&version==facetVersion_;
        // A failed rebuild may have replaced the allocation before submission.
        if(triangles&&!reuse)facetVersion_={};
        const bool sceneReady=inputsReady&&(triangles?
            (reuse||facetScene_.Record(command_.Get(),inputs_.Resource(17,words),sourceFacets,instances,inputs_.Resource(38,words),sourceRanges,triangles,billboards)):
            scene_.Record(command_.Get(),instances,request.instances));
        Timestamp(2);
        if(!sceneReady||!Descriptors(request,triangles+billboards)){
            const HRESULT sceneError=triangles?facetScene_.Error():scene_.Error();
            if(FAILED(sceneError))error_=sceneError;else if(FAILED(inputs_.Error()))error_=inputs_.Error();
            command_->Close();if(!triangles)scene_.Discard();reply.error=error_;return false;
        }
        ID3D12DescriptorHeap *heaps[]{heap_.Get(),samplers_.Get()};command_->SetDescriptorHeaps(2,heaps);command_->SetComputeRootSignature(signature_.Get());
        command_->SetPipelineState(triangles?facetPipeline_.Get():pipeline_.Get());
        command_->SetComputeRootDescriptorTable(0,heap_->GetGPUDescriptorHandleForHeapStart());command_->SetComputeRootDescriptorTable(2,samplers_->GetGPUDescriptorHandleForHeapStart());
        for(UINT sample=0;sample<samples_;sample++){
            const uint32_t values[]{request.width,request.height,sample,0};command_->SetComputeRoot32BitConstants(1,4,values,0);
            if(compact)DispatchQueues(request,sample,staged);
            else {command_->Dispatch((request.width+7)/8,(request.height+7)/8,1);UavBarrier(output_.Get());}
        }
        Timestamp(3);Pack(request);inputs_.RecordRelease(command_.Get());Timestamp(4);
        if(timingHeap_)command_->ResolveQueryData(timingHeap_.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,5,timingRead_.Get(),0);
        error_=command_->Close();
        if(FAILED(error_)){if(!triangles)scene_.Discard();reply.error=error_;return false;}
        error_=queue_->Wait(inputs_.Fence(),request.inputReady);if(FAILED(error_)){if(!triangles)scene_.Discard();reply.error=error_;return false;}
        ID3D12CommandList *lists[]{command_.Get()};queue_->ExecuteCommandLists(1,lists);
        reply.status=uint32_t(gfx::TransferStatus::Faulted);
        const HRESULT outputSignal=queue_->Signal(inputs_.Fence(),request.outputReady);
        const HRESULT drained=queue_->Signal(drainFence_.Get(),++drainValue_);drainQueued_=SUCCEEDED(drained);
        if(FAILED(outputSignal)||FAILED(drained)){
            faulted_=true;error_=FAILED(outputSignal)?outputSignal:drained;reply.error=error_;return false;
        }
        if(triangles){
            facetScene_.Submitted();facetVersion_=version;facetReused_=reuse;
        }else scene_.Submitted();
        inputs_.Submitted();reply.status=uint32_t(gfx::TransferStatus::Submitted);reply.error=0;return true;
    }
};
}
