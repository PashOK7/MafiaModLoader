/* Coherent D3D11 publication of ray_scene.h. The caller owns its world render-cycle and device generation.
 * Upload failure invalidates readiness even when old allocation bytes are retained. Bind never makes an old
 * frame current. No source pointers, native textures, readback or game addresses are retained here. */
#pragma once
#include <d3d11.h>
#include <array>
#include <utility>
#include "mml/ray_scene.h"
#include "mml/gpu_immutable_buffer.h"

namespace mml { namespace scene {class RadianceGpu;} namespace ray {
struct GpuConstants { uint32_t counts[4],more[4];float lo[4],hi[4]; };
static_assert(sizeof(GpuConstants)==64,"ray snapshot cbuffer layout");
class GpuScene {
    using Resource=gfx::ImmutableBuffer;
public:
    GpuScene()=default;GpuScene(const GpuScene &)=delete;GpuScene &operator=(const GpuScene &)=delete;
    ~GpuScene(){Release();}
    void Invalidate(){ready_=false;}
    void Release(){for(auto &r:data_)r.Clear();constant_.Clear();if(device_)device_->Release();device_=nullptr;ready_=false;cycle_=sourceFrame_=0;generation_=0;}
    bool Ready(uint64_t cycle,unsigned generation)const{return ready_&&cycle_==cycle&&generation_==generation;}
    uint64_t SourceFrame()const{return sourceFrame_;}
    HRESULT Error()const{return error_;}
    unsigned ReusedBuffers()const{return reusedBuffers_;}
    unsigned AppendedBuffers()const{return appendedBuffers_;}
    size_t UploadedBytes()const{return uploadedBytes_;}
    bool Upload(ID3D11Device *device,const Scene &scene,const Bounds &coverage,uint64_t cycle,unsigned generation){
        try{return UploadOwned(device,std::make_shared<const Scene>(scene),coverage,cycle,generation);}
        catch(const std::bad_alloc &){ready_=false;error_=E_OUTOFMEMORY;return false;}catch(...){ready_=false;error_=E_FAIL;return false;}
    }
    bool UploadOwned(ID3D11Device *device,const std::shared_ptr<const Scene> &owned,const Bounds &coverage,uint64_t cycle,unsigned generation,const GpuScene *previous=nullptr){
        return UploadPacked(device,owned,coverage,cycle,generation,previous,nullptr,0,0,false,false);
    }
private:
    friend class mml::scene::RadianceGpu;
    bool UploadPacked(ID3D11Device *device,const std::shared_ptr<const Scene> &owned,const Bounds &coverage,uint64_t cycle,unsigned generation,const GpuScene *previous,
                      const Scene *packedFrom,size_t nodePrefix,size_t trianglePrefix,bool compactAttributes,bool appendPacked){
        ready_=false;error_=E_INVALIDARG;
        if(!owned)return false;const auto &scene=*owned;if(!previous)previous=this;
        const auto *reuse=previous->device_==device&&previous->generation_==generation?previous:nullptr;
        const auto nodes=scene.Nodes();const auto triangles=scene.Triangles();
        const bool proven=packedFrom&&reuse&&reuse->owned_.lock().get()==packedFrom&&
            nodePrefix<=nodes.size()&&nodePrefix<=packedFrom->Nodes().size()&&
            trianglePrefix<=triangles.size()&&trianglePrefix<=packedFrom->Triangles().size();
        if(!proven)nodePrefix=trianglePrefix=0;
        if(!device||!cycle||!generation||!coverage.Valid()||!Valid(scene,nodePrefix))return false;
        std::array<Resource,6> candidate;Resource constant;
        if(!MakeView(device,nodes,candidate[0],reuse?&reuse->data_[0]:nullptr,owned,nodePrefix,appendPacked)||
           !MakeView(device,triangles,candidate[1],reuse?&reuse->data_[1]:nullptr,owned,trianglePrefix,appendPacked)||
           !Make(device,scene.topNodes,candidate[2],reuse?&reuse->data_[2]:nullptr,owned)||!Make(device,scene.instances,candidate[3],reuse?&reuse->data_[3]:nullptr,owned)||
           !Make(device,scene.instanceOrder,candidate[4],reuse?&reuse->data_[4]:nullptr,owned)||!Make(device,scene.unknown,candidate[5],reuse?&reuse->data_[5]:nullptr,owned))return false;
        GpuConstants values{{uint32_t(nodes.size()),uint32_t(triangles.size()),uint32_t(scene.topNodes.size()),uint32_t(scene.instances.size())},
            {uint32_t(scene.instanceOrder.size()),uint32_t(scene.unknown.size()),uint32_t(scene.frameId),uint32_t(scene.frameId>>32)},
            {coverage.lo.x,coverage.lo.y,coverage.lo.z,compactAttributes?1.f:0.f},{coverage.hi.x,coverage.hi.y,coverage.hi.z,0}};
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof values;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{&values,0,0};error_=device->CreateBuffer(&desc,&initial,&constant.buffer);if(FAILED(error_))return false;
        for(unsigned i=0;i<6;i++)data_[i].Swap(candidate[i]);constant_.Swap(constant);
        reusedBuffers_=appendedBuffers_=0;uploadedBytes_=sizeof values;
        for(const auto &r:data_){reusedBuffers_+=r.reused?1u:0u;appendedBuffers_+=r.appended?1u:0u;uploadedBytes_+=r.uploadedBytes;}
        if(device_!=device){if(device_)device_->Release();device_=device;device_->AddRef();}
        owned_=owned;cycle_=cycle;generation_=generation;sourceFrame_=scene.frameId;ready_=true;error_=S_OK;return true;
    }
public:
    bool BindCS(ID3D11DeviceContext *context,uint64_t cycle,unsigned generation)const{return Bind(context,cycle,generation,true);}
    bool BindPS(ID3D11DeviceContext *context,uint64_t cycle,unsigned generation)const{return Bind(context,cycle,generation,false);}
    static void UnbindCS(ID3D11DeviceContext *context){ID3D11ShaderResourceView *empty[6]{};ID3D11Buffer *none=nullptr;context->CSSetShaderResources(16,6,empty);context->CSSetConstantBuffers(3,1,&none);}
    static void UnbindPS(ID3D11DeviceContext *context){ID3D11ShaderResourceView *empty[6]{};ID3D11Buffer *none=nullptr;context->PSSetShaderResources(16,6,empty);context->PSSetConstantBuffers(3,1,&none);}
private:
    template<class T>bool Make(ID3D11Device *device,const std::vector<T> &source,Resource &out,const Resource *previous,const std::shared_ptr<const Scene> &owner,size_t prefix=0){
        error_=out.MakeWithPrefix(device,gfx::BufferLease(owner,source),previous,prefix);return SUCCEEDED(error_);
    }
    template<class T>bool MakeView(ID3D11Device *device,std::span<const T> source,Resource &out,const Resource *previous,const std::shared_ptr<const Scene> &owner,size_t prefix,bool append){
        error_=out.MakeView(device,source,owner,previous,prefix,append);return SUCCEEDED(error_);
    }
    static bool NodesValid(std::span<const Node> nodes,size_t elements,size_t first=0){
        if(nodes.size()>UINT32_MAX)return false;
        for(size_t i=first;i<nodes.size();i++){
            const auto &n=nodes[i];if(!Bounds{n.lo,n.hi}.Valid()||n.escape<=i||n.escape>nodes.size())return false;
            if(n.count){if(size_t(n.first)+n.count>elements)return false;}
            else if(n.first<=i||n.first>=n.escape)return false;
        }
        return true;
    }
    static bool Valid(const Scene &s,size_t nodePrefix){
        const auto nodes=s.Nodes();
        if(!NodesValid(nodes,s.Triangles().size(),nodePrefix)||!NodesValid(s.topNodes,s.instanceOrder.size())||s.instances.size()>UINT32_MAX||s.unknown.size()>UINT32_MAX)return false;
        for(uint32_t i:s.instanceOrder)if(i>=s.instances.size())return false;
        for(const auto &i:s.instances){if(i.root>=nodes.size()||!Bounds{i.lo,i.hi}.Valid()||(i.flags&~15u)||(i.flags&6u)==6u)return false;for(const auto &row:i.local)for(float x:row)if(!Finite(x))return false;}
        for(const auto &u:s.unknown)if(!Bounds{u.lo,u.hi}.Valid())return false;
        return true;
    }
    bool Bind(ID3D11DeviceContext *context,uint64_t cycle,unsigned generation,bool compute)const{
        if(!context||!Ready(cycle,generation))return false;
        ID3D11Device *actual=nullptr;context->GetDevice(&actual);const bool matches=actual==device_;if(actual)actual->Release();if(!matches)return false;
        ID3D11ShaderResourceView *views[6];for(unsigned i=0;i<6;i++)views[i]=data_[i].view;
        ID3D11Buffer *constant=constant_.buffer;
        if(compute){context->CSSetShaderResources(16,6,views);context->CSSetConstantBuffers(3,1,&constant);}
        else{context->PSSetShaderResources(16,6,views);context->PSSetConstantBuffers(3,1,&constant);}
        return true;
    }
    std::array<Resource,6> data_;Resource constant_;ID3D11Device *device_=nullptr;
    std::weak_ptr<const Scene> owned_;
    uint64_t cycle_=0,sourceFrame_=0;unsigned generation_=0;bool ready_=false;HRESULT error_=S_OK;
    unsigned reusedBuffers_=0,appendedBuffers_=0;size_t uploadedBytes_=0;
};
} }
