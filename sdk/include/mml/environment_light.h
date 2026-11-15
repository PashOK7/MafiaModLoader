/* GPU-only image emitter calibration: one coherent directional image, bounded angular search support and
 * an explicit incident-power convention. No frame readback, screen coordinates, global cloud boost or point
 * sun is inferred. Caches image statistics and 16 importance-sampled medium-light packets until source changes. */
#pragma once
#include <d3d11.h>
#include <cmath>
#include <cstring>
namespace mml { namespace environment {
class ImageEmitter {
public:
    ImageEmitter()=default;ImageEmitter(const ImageEmitter&)=delete;ImageEmitter&operator=(const ImageEmitter&)=delete;
    ~ImageEmitter(){Release();}
    bool Make(ID3D11Device *device,const void *code,size_t bytes){
        Release();D3D11_BUFFER_DESC bd{};bd.ByteWidth=35*16;bd.StructureByteStride=16;
        bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
        if(FAILED(device->CreateBuffer(&bd,nullptr,&stats_))||FAILED(device->CreateShaderResourceView(stats_,nullptr,&view_))||
           FAILED(device->CreateUnorderedAccessView(stats_,nullptr,&output_))||FAILED(device->CreateComputeShader(code,bytes,nullptr,&shader_))){Release();return false;}
        bd={};bd.ByteWidth=16;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        if(FAILED(device->CreateBuffer(&bd,nullptr,&settings_))||FAILED(device->CreateSamplerState(&sd,&linear_))){Release();return false;}return true;
    }
    bool Update(ID3D11DeviceContext *context,ID3D11ShaderResourceView *cube,unsigned revision,const float *centre,float cosineRadius){
        if(!shader_||!cube||!centre||!std::isfinite(cosineRadius)||cosineRadius<=0||cosineRadius>=1)return false;
        float key[4]={centre[0],centre[1],centre[2],cosineRadius};float length=0;
        for(unsigned i=0;i<3;i++){if(!std::isfinite(key[i]))return false;length+=key[i]*key[i];}if(length<1e-12)return false;
        if(ready_&&revision_==revision&&!memcmp(key,key_,sizeof key))return true;
        context->UpdateSubresource(settings_,0,nullptr,key,0,0);context->CSSetConstantBuffers(4,1,&settings_);
        context->CSSetSamplers(3,1,&linear_);
        context->CSSetShaderResources(11,1,&cube);context->CSSetShader(shader_,nullptr,0);context->CSSetUnorderedAccessViews(1,1,&output_,nullptr);
        context->Dispatch(1,1,1);ID3D11UnorderedAccessView *none=nullptr;context->CSSetUnorderedAccessViews(1,1,&none,nullptr);
        cube=nullptr;context->CSSetShaderResources(11,1,&cube);context->CSSetShader(nullptr,nullptr,0);
        memcpy(key_,key,sizeof key);revision_=revision;ready_=true;return true;
    }
    ID3D11ShaderResourceView *View()const{return ready_?view_:nullptr;}
    void Release(){Drop(shader_);Drop(output_);Drop(view_);Drop(stats_);Drop(settings_);Drop(linear_);ready_=false;revision_=0;}
private:
    template<class T>static void Drop(T*&p){if(p){p->Release();p=nullptr;}}
    ID3D11ComputeShader *shader_=nullptr;ID3D11Buffer *stats_=nullptr,*settings_=nullptr;
    ID3D11ShaderResourceView *view_=nullptr;ID3D11UnorderedAccessView *output_=nullptr;
    ID3D11SamplerState *linear_=nullptr;
    bool ready_=false;unsigned revision_=0;float key_[4]={};
};
} }
