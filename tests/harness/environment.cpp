#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mml/environment.h"
#include "mml/environment_light.h"
#include <wrl/client.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include "shaders/CsEnvironmentTest.h"
#include "shaders/CsEnvironmentEmitter.h"
using Microsoft::WRL::ComPtr;
namespace {
struct Placer { MmlViewFn fn;void *user; };
std::vector<Placer> placements;
unsigned removed;
uint32_t Features(MmlMod *) { return MML_VIEW_CLIP|MML_VIEW_OWN_SECTOR|MML_VIEW_SKY_ONLY; }
int Add(MmlMod *,int w,int h,MmlViewFn fn,void *user) {
    assert(w==16&&h==16);placements.push_back({fn,user});return static_cast<int>(placements.size());
}
void Remove(MmlMod *,int) { ++removed; }
void Hr(HRESULT result) { assert(SUCCEEDED(result)); }
float Decode(float x) { return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f); }
}
int main() {
    MmlHost host{};host.size=sizeof host;host.scene_view_features=Features;host.scene_view=Add;host.scene_view_remove=Remove;
    auto *mod=reinterpret_cast<MmlMod *>(1);
    mml::environment::SkyCapture sky;
    host.size=offsetof(MmlHost,scene_view_features);
    assert(!sky.Create(&host,mod,16)&&placements.empty()); // old table never reads the appended field
    host.size=sizeof host;
    assert(!sky.Create(&host,mod,17)&&placements.empty());
    assert(sky.Create(&host,mod,16)&&placements.size()==6);
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&ctx));
    ComPtr<ID3D11Texture2D> textures[6];MmlGfxView views[6]{};
    for(unsigned face=0;face<6;face++) {
        MmlViewCamera camera{};camera.world[12]=42;camera.world[13]=12;camera.world[14]=-9;
        assert(placements[face].fn(&camera,placements[face].user)==1);
        assert(camera.flags==MML_VIEW_SKY_ONLY&&camera.tan_left==1&&camera.tan_up==1&&camera.world[12]==42);
        std::vector<uint8_t> data(16*16*4);
        for(unsigned y=0;y<16;y++) for(unsigned x=0;x<16;x++) {
            float nx=(x+.5f)/8-1,ny=1-(y+.5f)/8;
            float direction[3];float length=0;
            for(unsigned axis=0;axis<3;axis++) {
                direction[axis]=camera.world[8+axis]+nx*camera.world[axis]+ny*camera.world[4+axis];length+=direction[axis]*direction[axis];
            }
            for(unsigned axis=0;axis<3;axis++) data[(y*16+x)*4+axis]=static_cast<uint8_t>(std::round((.5f+.5f*direction[axis]/std::sqrt(length))*255));
            data[(y*16+x)*4+3]=255;
        }
        D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=16;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA source{data.data(),16*4};Hr(device->CreateTexture2D(&td,&source,&textures[face]));
        views[face].handle=sky.Handle(face);views[face].fresh=1;views[face].width=views[face].height=16;views[face].color_texture=textures[face].Get();
    }
    MmlGfxFrame frame{};frame.size=sizeof frame;frame.device=device.Get();frame.context=ctx.Get();frame.generation=1;frame.views=views;frame.view_count=6;
    views[5].fresh=0;assert(!sky.Update(frame)&&!sky.View()&&sky.Captures()==0);
    views[5].fresh=1;assert(sky.Update(frame)&&sky.View()&&sky.Captures()==1);
    for(auto &p:placements) { MmlViewCamera camera{};assert(p.fn(&camera,p.user)==0); }
    sky.Invalidate();views[5].fresh=0;assert(sky.Update(frame)&&sky.Captures()==1); // coherent previous cube, no mixed faces
    views[5].fresh=1;assert(sky.Update(frame)&&sky.Captures()==2);
    ComPtr<ID3D11ComputeShader> shader;Hr(device->CreateComputeShader(kCsEnvironmentTest,sizeof kCsEnvironmentTest,nullptr,&shader));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=6*16;bd.StructureByteStride=16;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Buffer> output,read;Hr(device->CreateBuffer(&bd,nullptr,&output));
    bd.BindFlags=0;bd.MiscFlags=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(device->CreateBuffer(&bd,nullptr,&read));
    ComPtr<ID3D11UnorderedAccessView> uav;Hr(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;Hr(device->CreateSamplerState(&sd,&sampler));
    auto *view=sky.View();auto *out=uav.Get();auto *s=sampler.Get();ctx->CSSetShader(shader.Get(),nullptr,0);
    ctx->CSSetShaderResources(0,1,&view);ctx->CSSetSamplers(0,1,&s);ctx->CSSetUnorderedAccessViews(0,1,&out,nullptr);ctx->Dispatch(1,1,1);
    out=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&out,nullptr);ctx->CopyResource(read.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};Hr(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
    const float *pixels=static_cast<const float *>(mapped.pData);
    for(unsigned face=0;face<6;face++) for(unsigned axis=0;axis<3;axis++) {
        float expected=axis==face/2 ? (face%2?0:1) : Decode(.5f);
        assert(std::fabs(pixels[face*4+axis]-expected)<.012f);
    }
    ctx->Unmap(read.Get(),0);ctx->ClearState();
    frame.generation=2;views[5].fresh=0;assert(!sky.Update(frame)&&!sky.View());
    views[5].fresh=1;assert(sky.Update(frame)&&sky.View());
    sky.Active(false);for(auto &p:placements) { MmlViewCamera camera{};assert(p.fn(&camera,p.user)==0); }
    sky.Active(true);assert(sky.Pending());assert(sky.Update(frame));
    mml::environment::ImageEmitter emitter;assert(emitter.Make(device.Get(),kCsEnvironmentEmitter,sizeof kCsEnvironmentEmitter));
    const float centre[3]={0,0,1},cosineRadius=std::cos(18*3.14159265359f/180);
    auto readEmitter=[&](unsigned revision){
        assert(emitter.Update(ctx.Get(),sky.View(),revision,centre,cosineRadius));
        ComPtr<ID3D11Resource> resource;emitter.View()->GetResource(&resource);ComPtr<ID3D11Buffer> buffer;
        Hr(resource.As(&buffer));D3D11_BUFFER_DESC desc{};buffer->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;
        desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;Hr(device->CreateBuffer(&desc,nullptr,&staging));ctx->CopyResource(staging.Get(),buffer.Get());
        D3D11_MAPPED_SUBRESOURCE map{};Hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));std::vector<float> values(35*4);
        memcpy(values.data(),map.pData,values.size()*sizeof(float));ctx->Unmap(staging.Get(),0);
        for(float x:values)assert(std::isfinite(x));return values;
    };
    auto measured=readEmitter(1);assert(measured[3]==1&&measured[1]>measured[0]&&measured[4]>0);
    // A small bright cloud opening to the LEFT of the nominal sun, not a point light at the cap centre.
    auto paint=[&](bool opening,bool outside){
        for(unsigned face=0;face<6;face++){
            std::vector<uint8_t> pixels(16*16*4,80);
            for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++){
                uint8_t brightness=(opening&&face==4&&x>=5&&x<=6&&y>=7&&y<=8)||(outside&&face==0)?255:80;
                for(unsigned channel=0;channel<3;channel++)pixels[(y*16+x)*4+channel]=brightness;
                pixels[(y*16+x)*4+3]=255;
            }
            ctx->UpdateSubresource(textures[face].Get(),0,nullptr,pixels.data(),16*4,0);
        }
        sky.Invalidate();assert(sky.Update(frame));
    };
    paint(true,false);auto opening=readEmitter(2);assert(opening[3]==1);float meanX=0,varianceY=0,power[3]={};
    for(unsigned packet=0;packet<16;packet++){
        unsigned direction=(3+packet*2)*4,beam=direction+4;
        assert(opening[direction+2]>=cosineRadius-1e-5f);
        meanX+=opening[direction]/16;varianceY+=opening[direction+1]*opening[direction+1]/16;
        assert(opening[beam]>0);
        for(unsigned channel=0;channel<3;channel++)power[channel]+=opening[beam+channel]*opening[direction+2]/opening[4+channel];
    }
    assert(meanX<-.04f&&varianceY>.00001f);
    for(float channel:power)assert(std::fabs(channel-1)<.025f); // image profile integrates to the requested perpendicular irradiance
    paint(true,true);auto outside=readEmitter(3);
    for(unsigned i=0;i<outside.size();i++)assert(std::fabs(outside[i]-opening[i])<1e-5f); // bright clouds outside support add NOTHING
    paint(false,false);auto uniform=readEmitter(4);assert(uniform[3]==0);
    for(unsigned channel=0;channel<3;channel++)assert(uniform[4+channel]==0);
    emitter.Release();assert(!emitter.View());sky.Release();assert(removed==6&&!sky.View());
    std::puts("environment: world cube orientation/linear sampling, coherent faces, old host, invalidate/reset/release: ALL PASSED");
    std::puts("image emitter: actual GPU off-centre cloud profile, bounded support, uniform sky emits no extra light, finite packets/release: ALL PASSED");
}
