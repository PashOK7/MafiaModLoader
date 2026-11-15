/* Owner-scoped D3D11 GPU publication for an external D3D12 worker.
 * Call on the owning immediate-context thread. The channel policy and timeout
 * belong to the caller. No buffer/texture contents are mapped or read by CPU.
 * Only caller-proven immutable SRVs may reuse a packed payload. Keeping that
 * exact SRV alive also prevents pointer ABA and distinguishes appended views.
 * Packing and unpacking replace CS shader, SRV0, UAV0 and CB0. The caller
 * restores those bindings before its software fallback or other compute work.
 */
#pragma once
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include "mml/gpu_transfer_protocol.h"

namespace mml::gfx {
class TransferProducer {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Item {
        TransferResource wire{};
        Ptr<ID3D11ShaderResourceView> sourceView,packedView;
        Ptr<ID3D11Texture2D> texture;
        Ptr<ID3D11UnorderedAccessView> packedOutput;
        Ptr<IDXGIKeyedMutex> ownership;
        bool acquired=false;
        HANDLE handle=nullptr;
        uint64_t seen=0;
        ~Item(){Clear();}
        void Clear(){
            if(acquired&&ownership)ownership->ReleaseSync(0);acquired=false;
            if(handle)CloseHandle(handle);
            handle=nullptr;wire={};sourceView.Reset();packedView.Reset();
            texture.Reset();packedOutput.Reset();ownership.Reset();seen=0;
        }
    };
    Ptr<ID3D11Device5> device_;
    Ptr<ID3D11DeviceContext4> context_;
    Ptr<ID3D11Fence> fence_;
    Ptr<ID3D11ComputeShader> pack_,unpack_;
    Ptr<ID3D11Buffer> unpackConstants_,packConstants_,raw_;
    Ptr<ID3D11ShaderResourceView> rawView_;
    UINT rawCapacity_=0;
    HANDLE fenceHandle_=nullptr;
    std::array<Item,TransferMaxResources> items_;
    TransferRequest request_{};
    uint64_t owner_=0,nextId_=0,timeline_=0,required_=0;
    uint32_t generation_=0;
    bool pending_=false,building_=false,needsFlush_=false;
    HRESULT error_=S_OK;
    Item *Slot(uint32_t binding,TransferKind kind){
        for(auto &item:items_)if(item.wire.id&&item.wire.binding==binding&&item.wire.kind==uint32_t(kind))return &item;
        for(auto &item:items_)if(!item.wire.id)return &item;
        error_=E_OUTOFMEMORY;return nullptr;
    }
    bool Shared(Item &item,D3D11_TEXTURE2D_DESC description){
        item.wire.id=++nextId_;
        description.Usage=D3D11_USAGE_DEFAULT;description.CPUAccessFlags=0;
        description.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        description.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
        if(description.Format==DXGI_FORMAT_R32_UINT||description.Format==DXGI_FORMAT_R32G32B32A32_FLOAT)
            description.BindFlags|=D3D11_BIND_UNORDERED_ACCESS;
        error_=device_->CreateTexture2D(&description,nullptr,&item.texture);if(FAILED(error_))return false;
        Ptr<IDXGIResource1> resource;error_=item.texture.As(&resource);if(FAILED(error_))return false;
        error_=resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&item.handle);if(FAILED(error_))return false;
        error_=item.texture.As(&item.ownership);if(FAILED(error_))return false;
        error_=item.ownership->AcquireSync(0,0);if(error_!=S_OK)return false;item.acquired=true;
        item.wire.handle=uint64_t(reinterpret_cast<uintptr_t>(item.handle));
        item.wire.width=description.Width;item.wire.height=description.Height;
        item.wire.layers=description.ArraySize;item.wire.mips=description.MipLevels;
        item.wire.resourceFormat=description.Format;
        return true;
    }
    bool Add(Item &item,uint32_t binding,TransferKind kind){
        if(request_.count>=TransferMaxResources){error_=E_OUTOFMEMORY;return false;}
        item.wire.binding=binding;item.wire.kind=uint32_t(kind);item.seen=request_.cycle;
        request_.resources[request_.count++]=item.wire;return true;
    }
    bool Raw(UINT bytes){
        if(rawCapacity_>=bytes)return true;
        const UINT capacity=(bytes+std::min(bytes/8,2u*1024*1024)+15)&~15u;
        Ptr<ID3D11Buffer> buffer;Ptr<ID3D11ShaderResourceView> view;
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=capacity;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        error_=device_->CreateBuffer(&desc,nullptr,&buffer);if(FAILED(error_))return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32_TYPELESS;sv.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        sv.BufferEx.NumElements=capacity/4;sv.BufferEx.Flags=D3D11_BUFFEREX_SRV_FLAG_RAW;
        error_=device_->CreateShaderResourceView(buffer.Get(),&sv,&view);if(FAILED(error_))return false;
        raw_=std::move(buffer);rawView_=std::move(view);rawCapacity_=capacity;return true;
    }
    bool Words(uint32_t binding,ID3D11Buffer *buffer,ID3D11ShaderResourceView *view,uint32_t offset,uint32_t bytes,uint32_t stride,bool immutable){
        auto *item=Slot(binding,TransferKind::Words);if(!item)return false;
        if(immutable&&view&&item->sourceView.Get()==view&&item->wire.stride==stride&&item->wire.elements==bytes/stride)return Add(*item,binding,TransferKind::Words);
        const uint32_t words=bytes/4;
        if(!bytes||bytes>256u*1024*1024||bytes%4||!stride||bytes%stride){error_=E_INVALIDARG;return false;}
        const bool reallocate=!item->wire.id||uint64_t(item->wire.width)*item->wire.height<words;
        if(reallocate){
            const UINT capacity=std::min(words+std::min(words/8,512u*1024),4096u*D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION);
            const UINT width=std::min(4096u,std::max(16u,words)),height=(capacity+width-1)/width;
            if(height>D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION){error_=E_INVALIDARG;return false;}
            item->Clear();D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.ArraySize=td.MipLevels=1;
            td.Format=DXGI_FORMAT_R32_UINT;td.SampleDesc.Count=1;if(!Shared(*item,td))return false;
            error_=device_->CreateUnorderedAccessView(item->texture.Get(),nullptr,&item->packedOutput);if(FAILED(error_))return false;
        }
        if(!Raw(bytes))return false;
        D3D11_BUFFER_DESC source{};buffer->GetDesc(&source);
        if(uint64_t(offset)+bytes>source.ByteWidth){error_=E_INVALIDARG;return false;}
        const D3D11_BOX box{offset,0,0,offset+bytes,1,1};
        context_->CopySubresourceRegion(raw_.Get(),0,0,0,0,buffer,0,!offset&&bytes==source.ByteWidth?nullptr:&box);
        const UINT width=item->wire.width,height=(words+width-1)/width;const uint32_t values[]{words,width,0,0};
        context_->UpdateSubresource(packConstants_.Get(),0,nullptr,values,0,0);
        ID3D11ShaderResourceView *input=rawView_.Get();ID3D11UnorderedAccessView *output=item->packedOutput.Get();ID3D11Buffer *cb=packConstants_.Get();
        context_->CSSetShader(pack_.Get(),nullptr,0);context_->CSSetShaderResources(0,1,&input);context_->CSSetUnorderedAccessViews(0,1,&output,nullptr);context_->CSSetConstantBuffers(0,1,&cb);
        context_->Dispatch((width+15)/16,(height+15)/16,1);input=nullptr;output=nullptr;
        context_->CSSetShaderResources(0,1,&input);context_->CSSetUnorderedAccessViews(0,1,&output,nullptr);
        item->wire.revision++;
        item->sourceView=immutable?view:nullptr;item->wire.stride=stride;item->wire.elements=bytes/stride;
        item->wire.flags=immutable?TransferImmutable:0;
        return Add(*item,binding,TransferKind::Words);
    }
public:
    TransferProducer()=default;
    TransferProducer(const TransferProducer &)=delete;
    TransferProducer &operator=(const TransferProducer &)=delete;
    ~TransferProducer(){Release();}
    void Release(){
        FlushInputs();
        for(auto &item:items_)item.Clear();
        if(fenceHandle_)CloseHandle(fenceHandle_);fenceHandle_=nullptr;
        pack_.Reset();unpack_.Reset();unpackConstants_.Reset();packConstants_.Reset();raw_.Reset();rawView_.Reset();rawCapacity_=0;
        fence_.Reset();context_.Reset();device_.Reset();
        owner_=nextId_=timeline_=required_=0;generation_=0;pending_=building_=needsFlush_=false;request_={};
    }
    bool Initialize(ID3D11Device *device,uint32_t generation,std::span<const uint8_t> pack,std::span<const uint8_t> unpack){
        Release();if(!device||!generation)return false;
        error_=device->QueryInterface(IID_PPV_ARGS(&device_));if(FAILED(error_))return false;
        Ptr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
        error_=context.As(&context_);if(FAILED(error_))return false;
        error_=device_->CreateComputeShader(pack.data(),pack.size(),nullptr,&pack_);if(FAILED(error_))return false;
        error_=device_->CreateComputeShader(unpack.data(),unpack.size(),nullptr,&unpack_);if(FAILED(error_))return false;
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=16;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        error_=device_->CreateBuffer(&cb,nullptr,&packConstants_);if(FAILED(error_))return false;
        error_=device_->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence_));if(FAILED(error_))return false;
        error_=fence_->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&fenceHandle_);if(FAILED(error_))return false;
        LARGE_INTEGER clock{};QueryPerformanceCounter(&clock);
        owner_=uint64_t(clock.QuadPart)^(uint64_t(GetCurrentProcessId())<<32)^uint64_t(reinterpret_cast<uintptr_t>(this));
        if(!owner_)owner_=1;generation_=generation;return true;
    }
    HRESULT Error()const{return error_;}
    uint32_t Generation()const{return generation_;}
    bool Begin(uint64_t cycle,uint64_t frame,uint32_t width,uint32_t height,uint32_t instances,DWORD completionTimeout=0){
        if(!device_||building_||!cycle||!width||!height)return false;
        if(pending_){
            const ULONGLONG deadline=GetTickCount64()+completionTimeout;
            auto completed=fence_->GetCompletedValue();
            while(completed!=UINT64_MAX&&completed<required_&&GetTickCount64()<deadline){
                Sleep(1);completed=fence_->GetCompletedValue();
            }
            if(completed==UINT64_MAX||completed<required_)return false;
            pending_=false;
        }
        request_={};request_.producerPid=GetCurrentProcessId();request_.owner=owner_;request_.cycle=cycle;request_.frame=frame;
        request_.generation=generation_;request_.width=width;request_.height=height;request_.instances=instances;
        request_.fenceHandle=uint64_t(reinterpret_cast<uintptr_t>(fenceHandle_));
        request_.inputReady=timeline_+1;request_.outputReady=timeline_+2;request_.consumed=timeline_+3;
        building_=true;return true;
    }
    bool Buffer(uint32_t binding,ID3D11ShaderResourceView *view,bool immutable){
        if(!building_||!view)return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};view->GetDesc(&desc);
        if(desc.ViewDimension!=D3D11_SRV_DIMENSION_BUFFER){error_=E_INVALIDARG;return false;}
        Ptr<ID3D11Resource> resource;view->GetResource(&resource);Ptr<ID3D11Buffer> buffer;
        error_=resource.As(&buffer);if(FAILED(error_))return false;
        D3D11_BUFFER_DESC bd{};buffer->GetDesc(&bd);const auto stride=bd.StructureByteStride;
        if(!stride||uint64_t(desc.Buffer.FirstElement)+desc.Buffer.NumElements>bd.ByteWidth/stride)return false;
        return Words(binding,buffer.Get(),view,desc.Buffer.FirstElement*stride,desc.Buffer.NumElements*stride,stride,immutable);
    }
    bool Constant(uint32_t binding,ID3D11Buffer *buffer){
        if(!building_||!buffer)return false;D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);
        return Words(binding,buffer,nullptr,0,desc.ByteWidth,4,false);
    }
    bool Sampler(uint32_t binding,ID3D11SamplerState *sampler){
        if(!building_||!sampler||binding>=D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT||request_.samplerCount>=TransferMaxSamplers)return false;
        D3D11_SAMPLER_DESC desc{};sampler->GetDesc(&desc);auto &wire=request_.samplers[request_.samplerCount++];
        wire.binding=binding;wire.filter=desc.Filter;wire.addressU=desc.AddressU;wire.addressV=desc.AddressV;wire.addressW=desc.AddressW;
        wire.maxAnisotropy=desc.MaxAnisotropy;wire.comparison=desc.ComparisonFunc;
        wire.mipBias=desc.MipLODBias;wire.minLod=desc.MinLOD;wire.maxLod=desc.MaxLOD;
        std::copy_n(desc.BorderColor,4,wire.border);return true;
    }
    // integerLoad retains a growing single-mip 2D allocation. Its consumer must
    // use original integer texel coordinates and original logical-size constants,
    // without normalized sampling or GetDimensions on that transfer allocation.
    bool Image(uint32_t binding,ID3D11ShaderResourceView *view,bool integerLoad=false){
        if(!building_||!view)return false;
        Ptr<ID3D11Resource> resource;view->GetResource(&resource);Ptr<ID3D11Texture2D> texture;
        error_=resource.As(&texture);if(FAILED(error_))return false;
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);D3D11_SHADER_RESOURCE_VIEW_DESC sv{};view->GetDesc(&sv);
        if(desc.SampleDesc.Count!=1){error_=E_INVALIDARG;return false;}
        const bool full2d=sv.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D&&sv.Texture2D.MostDetailedMip==0&&
            (sv.Texture2D.MipLevels==desc.MipLevels||sv.Texture2D.MipLevels==UINT32_MAX);
        const bool fullArray=sv.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY&&sv.Texture2DArray.MostDetailedMip==0&&
            sv.Texture2DArray.FirstArraySlice==0&&sv.Texture2DArray.ArraySize==desc.ArraySize&&
            (sv.Texture2DArray.MipLevels==desc.MipLevels||sv.Texture2DArray.MipLevels==UINT32_MAX);
        const bool fullCube=sv.ViewDimension==D3D11_SRV_DIMENSION_TEXTURECUBE&&desc.ArraySize==6&&sv.TextureCube.MostDetailedMip==0&&
            (sv.TextureCube.MipLevels==desc.MipLevels||sv.TextureCube.MipLevels==UINT32_MAX);
        if(!full2d&&!fullArray&&!fullCube){error_=E_INVALIDARG;return false;}
        if(integerLoad&&(!full2d||desc.MipLevels!=1||desc.ArraySize!=1)){error_=E_INVALIDARG;return false;}
        Item *item=nullptr,*oldest=nullptr;unsigned variants=0;
        for(auto &candidate:items_)if(candidate.wire.kind==uint32_t(TransferKind::Image)&&candidate.wire.binding==binding){
            variants++;if(!oldest||candidate.seen<oldest->seen)oldest=&candidate;
            const auto &wire=candidate.wire;
            const bool dimensions=integerLoad?wire.width>=desc.Width&&wire.height>=desc.Height:wire.width==desc.Width&&wire.height==desc.Height;
            if(dimensions&&wire.layers==desc.ArraySize&&wire.mips==desc.MipLevels&&wire.resourceFormat==uint32_t(desc.Format))item=&candidate;
        }
        if(!item){
            if(variants>=(integerLoad?1u:2u))item=oldest;
            else for(auto &candidate:items_)if(!candidate.wire.id){item=&candidate;break;}
            if(!item){error_=E_OUTOFMEMORY;return false;}
        }
        const bool dimensions=integerLoad?item->wire.width>=desc.Width&&item->wire.height>=desc.Height:item->wire.width==desc.Width&&item->wire.height==desc.Height;
        if(!dimensions||item->wire.layers!=desc.ArraySize||item->wire.mips!=desc.MipLevels||item->wire.resourceFormat!=uint32_t(desc.Format)){
            item->Clear();if(!Shared(*item,desc))return false;
        }
        if(integerLoad)context_->CopySubresourceRegion(item->texture.Get(),0,0,0,0,texture.Get(),0,nullptr);
        else context_->CopyResource(item->texture.Get(),texture.Get());
        item->wire.revision++;
        item->wire.viewFormat=sv.Format;item->wire.viewDimension=sv.ViewDimension;
        item->wire.flags=integerLoad?TransferIntegerLoad:0;
        return Add(*item,binding,TransferKind::Image);
    }
    bool Output(uint32_t width,uint32_t height,uint32_t layers){
        if(!building_||!layers)return false;auto *item=Slot(0,TransferKind::Output);if(!item)return false;
        if(item->wire.width!=width||item->wire.height!=height||item->wire.layers!=layers){
            item->Clear();D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.ArraySize=layers;desc.MipLevels=1;
            desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;if(!Shared(*item,desc))return false;
            error_=device_->CreateShaderResourceView(item->texture.Get(),nullptr,&item->packedView);if(FAILED(error_))return false;
            const uint32_t values[]{width,height,layers,0};D3D11_BUFFER_DESC bd{};bd.ByteWidth=sizeof values;bd.Usage=D3D11_USAGE_IMMUTABLE;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            const D3D11_SUBRESOURCE_DATA initial{values};unpackConstants_.Reset();error_=device_->CreateBuffer(&bd,&initial,&unpackConstants_);if(FAILED(error_))return false;
        }
        return Add(*item,0,TransferKind::Output);
    }
    bool Publish(TransferRequest &out,bool flush=true){
        if(!building_||!request_.count)return false;
        error_=context_->Signal(fence_.Get(),request_.inputReady);if(FAILED(error_)){building_=false;return false;}
        needsFlush_=true;if(flush)FlushInputs();
        out=request_;timeline_=request_.consumed;required_=request_.outputReady;pending_=true;building_=false;
        for(auto &item:items_)if(item.wire.id&&item.seen!=request_.cycle&&item.wire.kind!=uint32_t(TransferKind::Image))item.Clear();
        return true;
    }
    void Cancel(){building_=false;}
    // Deferring the input flush is safe when the caller flushes either through
    // Read or this method before returning from its pass, including fallback.
    void FlushInputs(){if(needsFlush_&&context_){context_->Flush();needsFlush_=false;}}
    void NotDelivered(){if(pending_)required_=request_.inputReady;}
    bool Rejected(const TransferReply &reply){
        if(!pending_||!TransferMatches(request_,reply)||reply.status!=uint32_t(TransferStatus::Rejected))return false;
        // Rejected certifies that the consumer submitted no GPU work.
        required_=request_.inputReady;return true;
    }
    bool Read(const TransferReply &reply,ID3D11UnorderedAccessView *target){
        if(!pending_||!target||!TransferMatches(request_,reply)||reply.status!=uint32_t(TransferStatus::Submitted))return false;
        auto *item=Slot(0,TransferKind::Output);if(!item||!item->packedView)return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};target->GetDesc(&view);
        Ptr<ID3D11Resource> resource;target->GetResource(&resource);Ptr<ID3D11Buffer> buffer;
        error_=resource.As(&buffer);if(FAILED(error_))return false;
        D3D11_BUFFER_DESC description{};buffer->GetDesc(&description);
        if(view.ViewDimension!=D3D11_UAV_DIMENSION_BUFFER||view.Buffer.FirstElement||view.Buffer.Flags||
           description.StructureByteStride!=16||view.Buffer.NumElements<uint64_t(item->wire.width)*item->wire.height*item->wire.layers){error_=E_INVALIDARG;return false;}
        // Submitted means ExecuteCommandLists AND the output Signal succeeded.
        error_=context_->Wait(fence_.Get(),request_.outputReady);if(FAILED(error_))return false;
        ID3D11ShaderResourceView *input=item->packedView.Get();ID3D11Buffer *cb=unpackConstants_.Get();
        context_->CSSetShader(unpack_.Get(),nullptr,0);context_->CSSetShaderResources(0,1,&input);
        context_->CSSetUnorderedAccessViews(0,1,&target,nullptr);context_->CSSetConstantBuffers(0,1,&cb);
        context_->Dispatch((item->wire.width+15)/16,(item->wire.height+15)/16,1);
        input=nullptr;target=nullptr;context_->CSSetShaderResources(0,1,&input);context_->CSSetUnorderedAccessViews(0,1,&target,nullptr);
        error_=context_->Signal(fence_.Get(),request_.consumed);if(FAILED(error_))return false;
        context_->Flush();needsFlush_=false;required_=request_.consumed;return true;
    }
};
}
