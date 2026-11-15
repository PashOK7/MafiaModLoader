/* Immutable D3D11 structured-buffer payload reuse. Exact CURRENT owned bytes prove sharing, not a hash,
 * native pointer, age or a frame tag. A reused GPU buffer takes the CURRENT CPU lease, preventing retention
 * of a different whole scene for each independently changing resource. Frame publication is the caller's. */
#pragma once
#include <d3d11.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <vector>
#include <type_traits>
namespace mml { namespace ray {class GpuScene;} namespace scene {class RadianceGpu;} namespace gfx {
class ImmutableBuffer {
public:
    ID3D11Buffer *buffer=nullptr;ID3D11ShaderResourceView *view=nullptr;
    bool reused=false,appended=false;size_t uploadedBytes=0;
    ImmutableBuffer()=default;ImmutableBuffer(const ImmutableBuffer &)=delete;ImmutableBuffer &operator=(const ImmutableBuffer &)=delete;
    ~ImmutableBuffer(){Clear();}
    void Clear(){if(view)view->Release();if(buffer)buffer->Release();if(device_)device_->Release();
        view=nullptr;buffer=nullptr;device_=nullptr;owner_.reset();bytes_=nullptr;count_=stride_=capacity_=0;reused=appended=false;uploadedBytes=0;}
    void Swap(ImmutableBuffer &other){std::swap(buffer,other.buffer);std::swap(view,other.view);std::swap(device_,other.device_);
        owner_.swap(other.owner_);std::swap(bytes_,other.bytes_);std::swap(count_,other.count_);std::swap(stride_,other.stride_);
        std::swap(capacity_,other.capacity_);std::swap(reused,other.reused);std::swap(appended,other.appended);std::swap(uploadedBytes,other.uploadedBytes);}
    template<class T>HRESULT Make(ID3D11Device *device,const std::vector<T> &data,const ImmutableBuffer *previous=nullptr){
        try{return MakeOwned(device,std::make_shared<const std::vector<T>>(data),previous);}
        catch(const std::bad_alloc &){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
    }
    template<class T>HRESULT MakeOwned(ID3D11Device *device,std::shared_ptr<const std::vector<T>> data,const ImmutableBuffer *previous=nullptr){
        return MakeWithPrefix(device,std::move(data),previous,0);
    }
private:
    friend class mml::ray::GpuScene;
    friend class mml::scene::RadianceGpu;
    // The scene publisher has already matched the previous immutable owner.
    template<class T>HRESULT MakeWithPrefix(ID3D11Device *device,std::shared_ptr<const std::vector<T>> data,const ImmutableBuffer *previous,size_t prefix){
        if(!data)return E_INVALIDARG;
        return MakeView(device,std::span<const T>(*data),data,previous,prefix);
    }
    HRESULT MakeLogicalView(ID3D11Device *device,size_t count){
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;
        desc.Buffer.NumElements=UINT(count);
        return device->CreateShaderResourceView(buffer,&desc,&view);
    }
    template<class T>HRESULT MakeView(ID3D11Device *device,std::span<const T> data,std::shared_ptr<const void> owner,const ImmutableBuffer *previous,size_t prefix,bool reserveAppend=false){
        static_assert(std::is_trivially_copyable<T>::value,"GPU payload must be trivially copyable");
        constexpr size_t limit=(std::min)(size_t(UINT32_MAX/sizeof(T)),size_t(1)<<D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP);
        if(previous==this||!device||!owner||data.size()>limit)return E_INVALIDARG;
        Clear();const size_t bytes=data.size_bytes();
        if(prefix&&(!previous||previous->count_<prefix||prefix>data.size()))return E_INVALIDARG;
        const bool compatible=previous&&previous->device_==device&&previous->buffer&&previous->view&&previous->owner_&&previous->stride_==sizeof(T);
        if(compatible&&previous->count_==data.size()&&
           (prefix==data.size()||previous->bytes_==data.data()||
            memcmp(static_cast<const uint8_t *>(previous->bytes_)+prefix*sizeof(T),data.data()+prefix,bytes-prefix*sizeof(T))==0)){
            buffer=previous->buffer;view=previous->view;buffer->AddRef();view->AddRef();capacity_=previous->capacity_;reused=true;
        }else if(reserveAppend&&compatible&&prefix&&previous->count_==prefix&&previous->capacity_>=data.size()){
            // Older in-flight draws keep their exact-sized SRV and only address the proven prefix.
            buffer=previous->buffer;buffer->AddRef();capacity_=previous->capacity_;
            const HRESULT hr=MakeLogicalView(device,data.size());if(FAILED(hr)){Clear();return hr;}
            ID3D11DeviceContext *context=nullptr;device->GetImmediateContext(&context);
            const D3D11_BOX tail{UINT(prefix*sizeof(T)),0,0,UINT(bytes),1,1};
            context->UpdateSubresource(buffer,0,&tail,data.data()+prefix,0,0);context->Release();
            uploadedBytes=bytes-tail.left;reused=appended=true;
        }else{
            // Clone the unchanged bytes on the device and upload only dirty
            // regions. The previous publication keeps its own resource.
            const auto changes=FindChanges(data.data(),data.size(),previous,device,prefix);
            const size_t spare=reserveAppend?(std::min)(data.size()/8,size_t(2u*1024*1024)/sizeof(T)):0;
            capacity_=(std::max)(size_t(1),(std::min)(limit,data.size()+spare));
            const bool hasSpare=spare&&capacity_>data.size();
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(capacity_*sizeof(T));desc.Usage=D3D11_USAGE_IMMUTABLE;
            desc.StructureByteStride=sizeof(T);desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            T zero{};D3D11_SUBRESOURCE_DATA initial{data.empty()?&zero:data.data(),0,0};
            if(changes.copyBytes||hasSpare)desc.Usage=D3D11_USAGE_DEFAULT;
            HRESULT hr=device->CreateBuffer(&desc,desc.Usage==D3D11_USAGE_DEFAULT?nullptr:&initial,&buffer);if(FAILED(hr))return hr;
            hr=hasSpare?MakeLogicalView(device,data.size()):device->CreateShaderResourceView(buffer,nullptr,&view);if(FAILED(hr)){Clear();return hr;}
            uploadedBytes=(std::max)(sizeof(T),bytes);
            if(desc.Usage==D3D11_USAGE_DEFAULT){
                ID3D11DeviceContext *context=nullptr;
                device->GetImmediateContext(&context);
                if(changes.copyBytes){
                    const D3D11_BOX common{0,0,0,changes.copyBytes,1,1};
                    context->CopySubresourceRegion(buffer,0,0,0,0,previous->buffer,0,&common);
                    const auto *source=reinterpret_cast<const uint8_t *>(data.data());
                    uploadedBytes=0;
                    for(size_t i=0;i<changes.count;i++){
                        const D3D11_BOX &range=changes.dirty[i];
                        context->UpdateSubresource(buffer,0,&range,source+range.left,0,0);
                        uploadedBytes+=range.right-range.left;
                    }
                }else{
                    const D3D11_BOX initialRange{0,0,0,UINT(bytes),1,1};
                    context->UpdateSubresource(buffer,0,&initialRange,data.data(),0,0);
                }
                context->Release();
            }
        }
        device_=device;device_->AddRef();bytes_=data.data();count_=data.size();stride_=sizeof(T);owner_=std::move(owner);return S_OK;
    }
    struct Changes {
        std::array<D3D11_BOX,32> dirty{};
        size_t count=0;
        UINT copyBytes=0;
    };
    template<class T>static Changes FindChanges(const T *source,size_t count,const ImmutableBuffer *previous,ID3D11Device *device,size_t prefix){
        if(count*sizeof(T)<512u*1024u||!previous||previous->device_!=device||!previous->buffer||
           !previous->owner_||previous->stride_!=sizeof(T))return {};
        Changes changes;
        const auto *now=reinterpret_cast<const uint8_t *>(source),*before=static_cast<const uint8_t *>(previous->bytes_);
        const size_t common=(std::min)(count,previous->count_);
        const size_t block=(std::max)(size_t(1),size_t(64u*1024u)/sizeof(T));
        size_t dirtyBytes=0;
        for(size_t first=prefix;first<count;){
            const size_t last=(std::min)(count,first+block);
            const UINT left=UINT(first*sizeof(T)),right=UINT(last*sizeof(T));
            if(last>common||memcmp(now+left,before+left,right-left)!=0){
                dirtyBytes+=right-left;
                if(changes.count&&changes.dirty[changes.count-1].right==left)changes.dirty[changes.count-1].right=right;
                else{
                    if(changes.count==changes.dirty.size())return {};
                    changes.dirty[changes.count++]={left,0,0,right,1,1};
                }
            }
            first=last;
        }
        if(dirtyBytes>count*sizeof(T)/2)return {};
        changes.copyBytes=UINT(common*sizeof(T));
        return changes;
    }
    ID3D11Device *device_=nullptr;std::shared_ptr<const void> owner_;const void *bytes_=nullptr;size_t count_=0,stride_=0,capacity_=0;
};
template<class Owner,class T>std::shared_ptr<const std::vector<T>> BufferLease(const std::shared_ptr<const Owner> &owner,const std::vector<T> &data){
    return std::shared_ptr<const std::vector<T>>(owner,&data);
}
} }
