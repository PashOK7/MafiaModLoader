/* D3D12 consumer of gpu_transfer.h. One owner/frame is recorded at a time.
 * Prepare requires the preceding submitted D3D12 work to have completed.
 * RecordInputs records GPU unpack and resource transitions. The caller queues
 * Wait(inputReady), ExecuteCommandLists and Signal(outputReady), in that order,
 * and publishes Submitted only after all three calls succeed. Before signaling,
 * RecordRelease returns shared allocations to COMMON. Rejected certifies that
 * no consumer work for that request was submitted. All objects are owner scoped.
 */
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <set>
#include <span>
#include <vector>
#include <cstring>
#include <iterator>
#include <utility>
#include "mml/gpu_transfer_protocol.h"

namespace mml::gfx {
class TransferConsumer {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Item {
        TransferResource wire{};
        Ptr<ID3D12Resource> shared,buffer;
        bool unpacked=false;
        uint64_t unpackedRevision=0,lastSeen=0;
    };
    struct Handle {
        HANDLE value=nullptr;
        ~Handle(){if(value)CloseHandle(value);}
    };
    Ptr<ID3D12Device> device_;
    Ptr<ID3D12RootSignature> signature_;
    Ptr<ID3D12PipelineState> pipeline_;
    Ptr<ID3D12DescriptorHeap> heap_;
    Ptr<ID3D12Fence> inputFence_,submittedFence_;
    uint64_t submittedValue_=0,owner_=0;
    uint32_t generation_=0,pid_=0;
    HANDLE process_=nullptr;
    UINT step_=0;
    HRESULT error_=S_OK;
    std::map<uint64_t,Item> items_;
    TransferRequest request_{};
    bool prepared_=false;
    bool Duplicate(uint64_t remote,Handle &local){
        if(!remote||remote>UINTPTR_MAX){error_=E_INVALIDARG;return false;}
        if(!DuplicateHandle(process_,reinterpret_cast<HANDLE>(uintptr_t(remote)),GetCurrentProcess(),&local.value,0,FALSE,DUPLICATE_SAME_ACCESS)){
            error_=HRESULT_FROM_WIN32(GetLastError());return false;
        }
        return true;
    }
    static bool Valid(const TransferResource &r){
        if(!r.id||!r.handle||!r.width||!r.height||!r.layers||!r.mips||r.width>16384||r.height>16384||r.layers>2048||r.mips>15)return false;
        if(r.kind==uint32_t(TransferKind::Words)){
            const uint64_t bytes=uint64_t(r.stride)*r.elements;
            return r.binding<128&&r.stride&&r.elements&&bytes<=256ull*1024*1024&&bytes%4==0&&
                r.resourceFormat==DXGI_FORMAT_R32_UINT&&r.layers==1&&r.mips==1&&
                bytes/4<=uint64_t(r.width)*r.height&&(r.flags&~TransferImmutable)==0;
        }
        if(r.kind==uint32_t(TransferKind::Output))return r.resourceFormat==DXGI_FORMAT_R32G32B32A32_FLOAT&&r.mips==1&&r.layers<=16;
        if(r.kind!=uint32_t(TransferKind::Image)||r.binding>=128||(r.flags&~TransferIntegerLoad))return false;
        if((r.flags&TransferIntegerLoad)&&(r.viewDimension!=D3D12_SRV_DIMENSION_TEXTURE2D||r.layers!=1||r.mips!=1))return false;
        return r.viewDimension==D3D12_SRV_DIMENSION_TEXTURE2D||r.viewDimension==D3D12_SRV_DIMENSION_TEXTURE2DARRAY||
            (r.viewDimension==D3D12_SRV_DIMENSION_TEXTURECUBE&&r.layers==6);
    }
    bool Open(Item &item,const TransferResource &wire){
        Handle handle;if(!Duplicate(wire.handle,handle))return false;
        error_=device_->OpenSharedHandle(handle.value,IID_PPV_ARGS(&item.shared));if(FAILED(error_))return false;
        const auto desc=item.shared->GetDesc();
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||desc.Width!=wire.width||desc.Height!=wire.height||
           desc.DepthOrArraySize!=wire.layers||desc.MipLevels!=wire.mips||desc.Format!=DXGI_FORMAT(wire.resourceFormat)||desc.SampleDesc.Count!=1){error_=E_INVALIDARG;return false;}
        item.wire=wire;
        if(wire.kind!=uint32_t(TransferKind::Words))return true;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width=(uint64_t(wire.width)*wire.height*4+255)&~uint64_t(255);
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;
        buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;buffer.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        error_=device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&item.buffer));
        return SUCCEEDED(error_);
    }
    static void Transition(ID3D12GraphicsCommandList *command,ID3D12Resource *resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};command->ResourceBarrier(1,&barrier);
    }
public:
    TransferConsumer()=default;
    TransferConsumer(const TransferConsumer &)=delete;
    TransferConsumer &operator=(const TransferConsumer &)=delete;
    ~TransferConsumer(){Release();}
    void Release(){
        items_.clear();inputFence_.Reset();submittedFence_.Reset();heap_.Reset();pipeline_.Reset();signature_.Reset();device_.Reset();
        if(process_)CloseHandle(process_);process_=nullptr;owner_=submittedValue_=0;generation_=pid_=0;prepared_=false;
    }
    bool Initialize(ID3D12Device *device,std::span<const uint8_t> shader){
        Release();if(!device||shader.empty())return false;device_=device;
        D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
        D3D12_ROOT_PARAMETER roots[2]{};roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;roots[0].DescriptorTable={2,ranges};
        roots[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;roots[1].Constants={0,0,4};
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=roots;Ptr<ID3DBlob> bytes,error;
        error_=D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&error);if(FAILED(error_))return false;
        error_=device_->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&signature_));if(FAILED(error_))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=signature_.Get();ps.CS={shader.data(),shader.size()};
        error_=device_->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline_));if(FAILED(error_))return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=TransferMaxResources*2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        error_=device_->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap_));step_=device_->GetDescriptorHandleIncrementSize(hd.Type);
        return SUCCEEDED(error_);
    }
    HRESULT Error()const{return error_;}
    ID3D12Fence *Fence()const{return prepared_?inputFence_.Get():nullptr;}
    const TransferRequest &Request()const{return request_;}
    ID3D12Resource *Resource(uint32_t binding,TransferKind kind)const{
        if(!prepared_)return nullptr;
        for(uint32_t index=0;index<request_.count;index++){
            const auto &wire=request_.resources[index];if(wire.binding!=binding||wire.kind!=uint32_t(kind))continue;
            const auto found=items_.find(wire.id);if(found==items_.end())return nullptr;
            return kind==TransferKind::Words?found->second.buffer.Get():found->second.shared.Get();
        }
        return nullptr;
    }
    bool Prepare(const TransferRequest &request){
        prepared_=false;
        if(!device_||request.operation!=uint32_t(TransferOperation::Submit)||request.version!=TransferVersion||request.size!=sizeof request||!request.owner||!request.producerPid||!request.cycle||
           !request.frame||!request.generation||!request.count||request.count>TransferMaxResources||request.samplerCount>TransferMaxSamplers||!request.width||!request.height||
           !request.fenceHandle||!request.inputReady||request.inputReady>UINT64_MAX-2||
           request.outputReady!=request.inputReady+1||request.consumed!=request.inputReady+2){error_=E_INVALIDARG;return false;}
        if(submittedFence_){const auto completed=submittedFence_->GetCompletedValue();if(completed==UINT64_MAX||completed<submittedValue_){error_=E_PENDING;return false;}}
        if(owner_!=request.owner||generation_!=request.generation||pid_!=request.producerPid){
            items_.clear();inputFence_.Reset();if(process_)CloseHandle(process_);
            owner_=0;generation_=pid_=0;
            process_=OpenProcess(PROCESS_DUP_HANDLE,FALSE,request.producerPid);
            if(!process_){error_=HRESULT_FROM_WIN32(GetLastError());return false;}
            Handle handle;if(!Duplicate(request.fenceHandle,handle))return false;
            error_=device_->OpenSharedHandle(handle.value,IID_PPV_ARGS(&inputFence_));if(FAILED(error_))return false;
            owner_=request.owner;generation_=request.generation;pid_=request.producerPid;
        }
        std::set<uint64_t> ids;std::set<std::pair<uint32_t,uint32_t>> bindings;
        std::set<uint32_t> samplerBindings;
        for(uint32_t index=0;index<request.samplerCount;index++){
            if(request.samplers[index].binding>=16||!samplerBindings.insert(request.samplers[index].binding).second){error_=E_INVALIDARG;return false;}
        }
        for(uint32_t index=0;index<request.count;index++){
            const auto &wire=request.resources[index];
            if(!Valid(wire)||!ids.insert(wire.id).second||!bindings.emplace(wire.kind,wire.binding).second){error_=E_INVALIDARG;return false;}
            auto found=items_.find(wire.id);
            if(found==items_.end()){
                Item item;if(!Open(item,wire))return false;items_.emplace(wire.id,std::move(item));
            }else{
                auto identity=found->second.wire;
                identity.revision=wire.revision;identity.stride=wire.stride;identity.elements=wire.elements;
                identity.flags=wire.flags;identity.viewFormat=wire.viewFormat;identity.viewDimension=wire.viewDimension;
                if(memcmp(&identity,&wire,sizeof wire)!=0||wire.revision<found->second.wire.revision){error_=E_INVALIDARG;return false;}
                found->second.wire=wire;
            }
            items_.at(wire.id).lastSeen=request.cycle;
        }
        for(auto item=items_.begin();item!=items_.end();){
            const bool retain=ids.count(item->first)||item->second.wire.kind==uint32_t(TransferKind::Image);
            item=retain?std::next(item):items_.erase(item);
        }
        std::map<uint32_t,std::vector<uint64_t>> variants;
        for(const auto &[id,item]:items_)if(item.wire.kind==uint32_t(TransferKind::Image))variants[item.wire.binding].push_back(id);
        for(auto &[binding,group]:variants){
            std::sort(group.begin(),group.end(),[&](uint64_t a,uint64_t b){return items_.at(a).lastSeen>items_.at(b).lastSeen;});
            const size_t retained=(items_.at(group.front()).wire.flags&TransferIntegerLoad)?1:2;
            for(size_t index=retained;index<group.size();index++)items_.erase(group[index]);
        }
        request_=request;prepared_=true;error_=S_OK;return true;
    }
    bool RecordInputs(ID3D12GraphicsCommandList *command){
        if(!prepared_||!command)return false;
        constexpr auto readable=D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        for(uint32_t index=0;index<request_.count;index++){
            auto &item=items_.at(request_.resources[index].id);const auto &wire=item.wire;
            if(wire.kind==uint32_t(TransferKind::Output))continue;
            if(wire.kind==uint32_t(TransferKind::Image)){
                Transition(command,item.shared.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);continue;
            }
            if(item.unpacked&&(wire.flags&TransferImmutable)&&item.unpackedRevision==wire.revision)continue;
            if(item.unpacked)Transition(command,item.buffer.Get(),readable,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Transition(command,item.shared.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            auto cpu=heap_->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=size_t(index)*2*step_;
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32_UINT;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;device_->CreateShaderResourceView(item.shared.Get(),&sv,cpu);cpu.ptr+=step_;
            const UINT words=wire.stride*wire.elements/4;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_R32_TYPELESS;uv.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
            uv.Buffer.NumElements=words;uv.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;device_->CreateUnorderedAccessView(item.buffer.Get(),nullptr,&uv,cpu);
            ID3D12DescriptorHeap *heaps[]{heap_.Get()};command->SetDescriptorHeaps(1,heaps);command->SetComputeRootSignature(signature_.Get());command->SetPipelineState(pipeline_.Get());
            auto gpu=heap_->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=size_t(index)*2*step_;command->SetComputeRootDescriptorTable(0,gpu);
            const uint32_t values[]{words,wire.width,0,0};command->SetComputeRoot32BitConstants(1,4,values,0);
            const UINT height=(words+wire.width-1)/wire.width;
            command->Dispatch((wire.width+15)/16,(height+15)/16,1);
            Transition(command,item.buffer.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,readable);
            Transition(command,item.shared.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        }
        return true;
    }
    void RecordRelease(ID3D12GraphicsCommandList *command){
        if(!prepared_||!command)return;
        for(uint32_t index=0;index<request_.count;index++){
            const auto &item=items_.at(request_.resources[index].id);
            if(item.wire.kind==uint32_t(TransferKind::Image))
                Transition(command,item.shared.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
        }
    }
    void Submitted(){
        if(!prepared_)return;
        submittedFence_=inputFence_;submittedValue_=request_.outputReady;
        for(uint32_t index=0;index<request_.count;index++){
            auto &item=items_.at(request_.resources[index].id);
            if(item.wire.kind==uint32_t(TransferKind::Words)){item.unpacked=true;item.unpackedRevision=item.wire.revision;}
        }
    }
};
}
