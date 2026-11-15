/* Original native mip rectangles in 64KiB logical pages. No atlas resize, border texels, source pointer key,
 * format conversion or implicit color-space change. Shader addressing confines every sample to its mip.
 * The coherent publisher may reuse a prefix only after proving immutable content identity. */
#pragma once
#include "mml/native_texture.h"
#include <array>
#include <utility>
#include <wrl/client.h>
namespace mml { namespace scene {
struct TextureRecord {uint32_t first,count,pad[2];};
struct MipRecord {uint32_t offset,pitch,width,height,format,pad[3];};
static_assert(sizeof(TextureRecord)==16&&sizeof(MipRecord)==32,"texture page GPU layout");
class TexturePages {
public:
    static constexpr size_t PageBytes=65536,MaxBytes=512u*1024*1024;
    std::vector<TextureRecord> textures;std::vector<MipRecord> mips;std::vector<uint32_t> words;
    size_t usefulBytes=0,packedBytes=0;bool valid=false;HRESULT error= S_OK;
    static int Format(D3DFORMAT native){switch(native){
        case D3DFMT_A8R8G8B8:return 0;case D3DFMT_A8B8G8R8:return 1;case D3DFMT_X8R8G8B8:return 2;
        case D3DFMT_R5G6B5:return 3;case D3DFMT_A1R5G5B5:return 4;case D3DFMT_A4R4G4B4:return 5;
        case D3DFMT_DXT1:return 6;case D3DFMT_DXT3:return 7;case D3DFMT_DXT5:return 8;default:return -1;}}
    bool Build(const std::vector<TextureSnapshot> &source){return BuildOwned(source);}
    bool Build(const std::vector<TextureRef> &source){return BuildOwned(source);}
    template<class T>bool BuildOwned(const std::vector<T> &source,bool pack=true){
        valid=false;error=E_INVALIDARG;TexturePages next;
        try{
            if(source.size()>UINT32_MAX)return false;size_t packed=0,mipCount=0;
            // Validate and measure before allocation. Repeated vector growth can require two large arrays
            // at once in the 32-bit game; allocate the exact padded payload once instead.
            for(const auto &item:source){
                const auto *data=TextureData(item);if(!data)return false;const auto &texture=*data;
                const int format=Format(texture.native);if(format<0||texture.mips.empty()||texture.mips.size()>14||mipCount>UINT32_MAX-texture.mips.size())return false;
                mipCount+=texture.mips.size();
                const unsigned firstW=texture.mips[0].width,firstH=texture.mips[0].height;
                if(!firstW||!firstH||firstW>8192||firstH>8192)return false;
                for(unsigned level=0;level<texture.mips.size();level++){
                    const auto &mip=texture.mips[level];unsigned block=format>=6?4:1,bytes=format>=6?(format==6?8:16):(format>=3?2:4);
                    const unsigned pitch=((mip.width+block-1)/block)*bytes,rows=(mip.height+block-1)/block;
                    if(mip.width!=(std::max)(1u,firstW>>level)||mip.height!=(std::max)(1u,firstH>>level)||mip.pitch!=pitch||mip.rows!=rows||mip.bytes.size()!=size_t(pitch)*rows)return false;
                    const size_t padded=(mip.bytes.size()+3)&~size_t(3);if(padded>MaxBytes||packed>MaxBytes-padded)return false;packed+=padded;
                }
            }
            const size_t pages=(packed+PageBytes-1)/PageBytes;if(pages>MaxBytes/PageBytes)return false;
            next.packedBytes=pages*PageBytes;
            next.textures.reserve(source.size());next.mips.reserve(mipCount);
            if(pack)next.words.resize(next.packedBytes/4);
            size_t cursor=0;
            for(const auto &item:source){const auto &texture=*TextureData(item);TextureRecord record{uint32_t(next.mips.size()),uint32_t(texture.mips.size()),{}};
                for(const auto &mip:texture.mips){next.mips.push_back({uint32_t(cursor),mip.pitch,mip.width,mip.height,uint32_t(Format(texture.native)),{}});
                    if(pack)memcpy(reinterpret_cast<uint8_t *>(next.words.data())+cursor,mip.bytes.data(),mip.bytes.size());
                    next.usefulBytes+=mip.bytes.size();cursor+=(mip.bytes.size()+3)&~size_t(3);}
                next.textures.push_back(record);
            }
        }catch(const std::bad_alloc &){error=E_OUTOFMEMORY;return false;}catch(...){error=E_FAIL;return false;}
        textures.swap(next.textures);mips.swap(next.mips);words.swap(next.words);
        usefulBytes=next.usefulBytes;packedBytes=next.packedBytes;valid=true;error=S_OK;return true;
    }
};
class GpuTexturePages {
    friend class RadianceGpu;
    struct Resource {ID3D11Buffer *buffer=nullptr;ID3D11ShaderResourceView *view=nullptr;
        ~Resource(){Clear();}void Clear(){if(view)view->Release();if(buffer)buffer->Release();view=nullptr;buffer=nullptr;}
        void Swap(Resource &other){std::swap(buffer,other.buffer);std::swap(view,other.view);}};
public:
    GpuTexturePages()=default;GpuTexturePages(const GpuTexturePages &)=delete;GpuTexturePages &operator=(const GpuTexturePages &)=delete;
    ~GpuTexturePages(){Release();}
    void Invalidate(){ready_=false;}
    void Release(){for(auto &r:resources_)r.Clear();if(device_)device_->Release();device_=nullptr;ready_=false;cycle_=0;generation_=0;}
    bool Ready(uint64_t cycle,unsigned generation)const{return ready_&&cycle==cycle_&&generation==generation_;}
    HRESULT Error()const{return error_;}
    bool Upload(ID3D11Device *device,const TexturePages &pages,uint64_t cycle,unsigned generation){
        ready_=false;error_=E_INVALIDARG;if(!device||!pages.valid||!cycle||!generation)return false;
        if(pages.words.size()*sizeof(uint32_t)!=pages.packedBytes)return false;
        std::array<Resource,3> candidate;
        error_=Make(device,pages.textures,candidate[0]);if(FAILED(error_))return false;
        error_=Make(device,pages.mips,candidate[1]);if(FAILED(error_))return false;
        error_=Make(device,pages.words,candidate[2]);if(FAILED(error_))return false;
        Publish(device,candidate,cycle,generation);return true;
    }
    template<class T>bool UploadStreamed(ID3D11Device *device,const std::vector<T> &source,uint64_t cycle,unsigned generation,size_t *pageBytes=nullptr){
        return UploadStreamedFrom(device,source,cycle,generation,nullptr,0,pageBytes,nullptr);
    }
private:
    // The coherent publisher proves the old ordered texture identities are
    // exactly the prefix of this new bank before calling this path.
    template<class T>bool UploadStreamedFrom(ID3D11Device *device,const std::vector<T> &source,uint64_t cycle,unsigned generation,
                                             const GpuTexturePages *previous,size_t first,size_t *pageBytes,size_t *uploadedBytes){
        ready_=false;error_=E_INVALIDARG;if(!device||!cycle||!generation)return false;
        TexturePages layout;
        if(!layout.BuildOwned(source,false)){error_=layout.error;return false;}
        UINT prefix=0;
        if(previous){
            if(previous==this||!first||first>=source.size()||previous->device_!=device||previous->generation_!=generation||
               !previous->ready_||!previous->resources_[2].buffer)return false;
            prefix=layout.mips[layout.textures[first].first].offset;
            D3D11_BUFFER_DESC old{};previous->resources_[2].buffer->GetDesc(&old);
            if(!prefix||prefix>old.ByteWidth)return false;
        }
        std::array<Resource,3> candidate;
        error_=Make(device,layout.textures,candidate[0]);if(FAILED(error_))return false;
        error_=Make(device,layout.mips,candidate[1]);if(FAILED(error_))return false;
        auto &payload=candidate[2];
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth=UINT((std::max)(size_t(4),layout.packedBytes));
        desc.Usage=D3D11_USAGE_DEFAULT;desc.StructureByteStride=sizeof(uint32_t);
        desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        error_=device->CreateBuffer(&desc,nullptr,&payload.buffer);if(FAILED(error_))return false;
        error_=device->CreateShaderResourceView(payload.buffer,nullptr,&payload.view);if(FAILED(error_))return false;
        try{
            // A large 32-bit address-space hole is no longer needed to stage
            // this GPU buffer. Write every padded byte in aligned chunks.
            std::vector<uint8_t> staging((std::min)(desc.ByteWidth,4u*1024u*1024u));
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
            if(prefix){
                const D3D11_BOX bytes{0,0,0,prefix,1,1};
                context->CopySubresourceRegion(payload.buffer,0,0,0,0,previous->resources_[2].buffer,0,&bytes);
            }
            size_t offset=prefix,filled=0;
            auto append=[&](const uint8_t *data,size_t bytes){
                while(bytes){
                    const size_t part=(std::min)(bytes,staging.size()-filled);
                    if(data){memcpy(staging.data()+filled,data,part);data+=part;}
                    else memset(staging.data()+filled,0,part);
                    filled+=part;bytes-=part;
                    if(filled==staging.size()){
                        const D3D11_BOX box{UINT(offset),0,0,UINT(offset+filled),1,1};
                        context->UpdateSubresource(payload.buffer,0,&box,staging.data(),0,0);
                        offset+=filled;filled=0;
                    }
                }
            };
            for(size_t i=first;i<source.size();i++)for(const auto &mip:TextureData(source[i])->mips){
                append(mip.bytes.data(),mip.bytes.size());
                append(nullptr,(4-mip.bytes.size()%4)%4);
            }
            append(nullptr,desc.ByteWidth-offset-filled);
            if(filled){
                const D3D11_BOX box{UINT(offset),0,0,UINT(offset+filled),1,1};
                context->UpdateSubresource(payload.buffer,0,&box,staging.data(),0,0);
            }
        }catch(const std::bad_alloc &){error_=E_OUTOFMEMORY;return false;}
        if(pageBytes)*pageBytes=layout.packedBytes;
        if(uploadedBytes)*uploadedBytes=desc.ByteWidth-prefix;
        Publish(device,candidate,cycle,generation);return true;
    }
public:
    bool Bind(ID3D11DeviceContext *context,uint64_t cycle,unsigned generation,bool compute)const{
        if(!context||!Ready(cycle,generation))return false;ID3D11Device *actual=nullptr;context->GetDevice(&actual);
        const bool same=actual==device_;if(actual)actual->Release();if(!same)return false;
        ID3D11ShaderResourceView *views[3]={resources_[0].view,resources_[1].view,resources_[2].view};
        if(compute)context->CSSetShaderResources(23,3,views);else context->PSSetShaderResources(23,3,views);return true;
    }
    static void Unbind(ID3D11DeviceContext *context,bool compute){ID3D11ShaderResourceView *none[3]{};if(compute)context->CSSetShaderResources(23,3,none);else context->PSSetShaderResources(23,3,none);}
private:
    void Publish(ID3D11Device *device,std::array<Resource,3> &candidate,uint64_t cycle,unsigned generation){
        for(unsigned i=0;i<3;i++)resources_[i].Swap(candidate[i]);
        if(device_!=device){if(device_)device_->Release();device_=device;device_->AddRef();}
        cycle_=cycle;generation_=generation;ready_=true;
    }
    // Only the coherent publisher calls this after comparing current verified immutable texel identities.
    bool Share(ID3D11Device *device,const GpuTexturePages &source,uint64_t cycle,unsigned generation){
        ready_=false;error_=E_INVALIDARG;
        if(&source==this||!device||device!=source.device_||generation!=source.generation_||!source.ready_||!cycle)return false;
        for(const auto &r:source.resources_)if(!r.buffer||!r.view)return false;
        Release();for(unsigned i=0;i<3;i++){resources_[i].buffer=source.resources_[i].buffer;resources_[i].view=source.resources_[i].view;
            resources_[i].buffer->AddRef();resources_[i].view->AddRef();}
        device_=device;device_->AddRef();cycle_=cycle;generation_=generation;ready_=true;error_=S_OK;return true;
    }
    template<class T>static HRESULT Make(ID3D11Device *device,const std::vector<T> &data,Resource &result){
        if(data.size()>UINT32_MAX/sizeof(T)||data.size()>(size_t(1)<<D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP))return E_INVALIDARG;
        T zero{};D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT((std::max)(size_t(1),data.size())*sizeof(T));desc.Usage=D3D11_USAGE_IMMUTABLE;
        desc.StructureByteStride=sizeof(T);desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{data.empty()?&zero:data.data(),0,0};
        HRESULT hr=device->CreateBuffer(&desc,&initial,&result.buffer);if(FAILED(hr))return hr;
        return device->CreateShaderResourceView(result.buffer,nullptr,&result.view);
    }
    std::array<Resource,3> resources_;ID3D11Device *device_=nullptr;uint64_t cycle_=0;unsigned generation_=0;bool ready_=false;HRESULT error_=S_OK;
};
} }
