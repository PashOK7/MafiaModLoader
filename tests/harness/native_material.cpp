#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include "shaders/CsNativeMaterial.h"
using Microsoft::WRL::ComPtr;
static void Hr(HRESULT h){if(FAILED(h)){std::printf("D3D failure %08X\n",unsigned(h));fflush(stdout);}assert(SUCCEEDED(h));}
struct Probe {float tex0[4],tex1[4],diffuse[4],factor[4];unsigned ops[4],info[4];};
static DWORD Argb(const float *c){return DWORD(std::lround(c[3]*255))<<24|DWORD(std::lround(c[0]*255))<<16|DWORD(std::lround(c[1]*255))<<8|DWORD(std::lround(c[2]*255));}
#include "native_texture_pages_test.h"
#include "native_visibility_test.h"
#include "scene_snapshot_test.h"
namespace texture_slot_test {
struct Capture {
    IDirect3DTexture9 *textures[2];
    unsigned order[3];
    bool mutate=false;
};
static void Paint(IDirect3DTexture9 *texture,DWORD color,uint64_t revision){
    D3DLOCKED_RECT data{};Hr(texture->LockRect(0,&data,nullptr,0));
    *static_cast<DWORD *>(data.pBits)=color;Hr(texture->UnlockRect(0));
    Hr(texture->SetPrivateData(mml::scene::NativeTextureRevision,&revision,sizeof revision,0));
}
static int Collect(MmlMod *mod,const float *,const float *,MmlSceneCollection *out){
    const auto &capture=*reinterpret_cast<Capture *>(mod);
    for(unsigned i=0;i<3;i++){
        auto *texture=capture.textures[capture.order[i]];
        if(capture.mutate&&i==2)Paint(texture,0xff00ff00,3);
        snapshot_test::vertices[0]={{0,0,0},0xffffffff,{0,0},{0,0}};
        snapshot_test::vertices[1]={{1,0,0},0xffffffff,{1,0},{0,0}};
        snapshot_test::vertices[2]={{0,1,0},0xffffffff,{0,1},{0,0}};
        snapshot_test::vertices[3]={{1,1,0},0xffffffff,{1,1},{0,0}};
        const uint32_t index[]{0,1,2,2,1,3};std::copy_n(index,6,snapshot_test::indices);
        MmlSceneTextureStage stages[2]{};stages[0].texture=texture;stages[0].color_op=D3DTOP_SELECTARG1;
        MmlSceneDraw draw{};draw.size=sizeof draw;draw.frame=99;draw.flags=MML_SCENE_DRAW_COLLECTION;
        draw.world[0]=draw.world[5]=draw.world[10]=draw.world[15]=1;
        draw.stage_stride=sizeof(MmlSceneTextureStage);draw.stages=stages;
        snapshot_test::active=&draw;snapshot_test::observer(&draw,snapshot_test::observerUser);snapshot_test::active=nullptr;
    }
    out->frame=99;out->selected=out->draws=3;return 1;
}
static void Run(IDirect3DDevice9 *device){
    using namespace mml::scene;
    ComPtr<IDirect3DTexture9> textures[2];
    for(auto &texture:textures)Hr(device->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr));
    Paint(textures[0].Get(),0xffff0000,1);Paint(textures[1].Get(),0xff0000ff,2);
    Capture capture{{textures[0].Get(),textures[1].Get()},{0,0,1}};
    MmlHost host{};host.size=uint32_t(offsetof(MmlHost,scene_draw_collect_ex));
    host.scene_draw_observe=snapshot_test::Observe;host.scene_draw_geometry=snapshot_test::Geometry;host.scene_draw_collect=Collect;
    SnapshotBuilder builder;builder.Begin();const mml::ray::Bounds bounds{{-10,-10,-10},{10,10,10}};
    auto collect=[&](){assert(builder.Collect(&host,reinterpret_cast<MmlMod *>(&capture),bounds,true));return builder.Current(builder.Cycle());};
    const auto *first=collect();assert(first->textures.size()==2&&first->textureCopied==2&&first->textureReused==1);
    assert(first->materials[0].textureIds[0]==0&&first->materials[1].textureIds[0]==0&&first->materials[2].textureIds[0]==1);
    capture.order[0]=1;capture.order[1]=capture.order[2]=0;
    const auto *retry=collect();assert(retry->textures.size()==2&&retry->textureReused==3);
    assert(retry->materials[0].textureIds[0]==0&&retry->materials[1].textureIds[0]==1&&retry->materials[2].textureIds[0]==1);
    for(auto &index:capture.order)index=0;capture.mutate=true;
    const auto *changed=collect();assert(changed->textures.size()==2&&changed->textureCopied==1&&changed->textureReused==2);
    assert(changed->materials[0].textureIds[0]==0&&changed->materials[1].textureIds[0]==0&&changed->materials[2].textureIds[0]==1);
    const auto &before=changed->textures[0].value->mips[0].bytes,&after=changed->textures[1].value->mips[0].bytes;
    DWORD oldColor=0,newColor=0;std::memcpy(&oldColor,before.data(),4);std::memcpy(&newColor,after.data(),4);
    assert(oldColor==0xffff0000&&newColor==0xff00ff00);
    std::puts("texture publication slots: repeated revision, same-cycle retry and changed revision within collection PASSED");
}
}
static void CheckImmutableTexture(IDirect3DDevice9 *device){
    using namespace mml::scene;ComPtr<IDirect3DTexture9> source,replacement;
    Hr(device->CreateTexture(8,8,4,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&source,nullptr));
    Hr(device->CreateTexture(4,4,3,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&replacement,nullptr));
    for(unsigned level=0;level<4;level++){
        D3DLOCKED_RECT lock{};Hr(source->LockRect(level,&lock,nullptr,0));const unsigned w=8>>level;
        for(unsigned y=0;y<w;y++)memset(static_cast<uint8_t *>(lock.pBits)+size_t(y)*lock.Pitch,0x35+level,w*4);Hr(source->UnlockRect(level));
    }
    TextureSnapshot snapshot;assert(snapshot.Read(source.Get())==TextureResult::Valid);bool equal=false;
    assert(snapshot.Verify(source.Get(),equal)==TextureResult::Valid&&equal);
    for(unsigned level=0;level<4;level++){
        D3DLOCKED_RECT lock{};Hr(source->LockRect(level,&lock,nullptr,0));*static_cast<uint8_t *>(lock.pBits)^=1;Hr(source->UnlockRect(level));
        assert(snapshot.Verify(source.Get(),equal)==TextureResult::Valid&&!equal); // same pointer, changed mip content
        Hr(source->LockRect(level,&lock,nullptr,0));*static_cast<uint8_t *>(lock.pBits)^=1;Hr(source->UnlockRect(level));
        assert(snapshot.Verify(source.Get(),equal)==TextureResult::Valid&&equal);
    }
    assert(snapshot.Verify(replacement.Get(),equal)==TextureResult::Valid&&!equal);
    assert(snapshot.Verify(nullptr,equal)==TextureResult::Missing&&!equal);
    TextureRef shared(std::move(snapshot));std::vector<TextureRef> refs{shared,shared};TexturePages pages;
    assert(pages.Build(refs)&&refs[0].value==refs[1].value);refs.push_back(TextureRef{});assert(!pages.Build(refs));
    std::puts("immutable texture verification: every mip mutation, metadata replacement, missing input and owned sharing PASSED");
}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);
    HWND window=CreateWindowExW(0,L"STATIC",L"native material reference",WS_POPUP,0,0,4,4,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);assert(window);
    ComPtr<IDirect3D9> api;api.Attach(Direct3DCreate9(D3D_SDK_VERSION));assert(api);
    D3DPRESENT_PARAMETERS present{};present.Windowed=TRUE;present.SwapEffect=D3DSWAPEFFECT_DISCARD;present.hDeviceWindow=window;present.BackBufferWidth=present.BackBufferHeight=4;
    ComPtr<IDirect3DDevice9> native;Hr(api->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&present,&native));
    ComPtr<IDirect3DSurface9> target,read9;Hr(native->CreateRenderTarget(4,4,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&target,nullptr));
    Hr(native->CreateOffscreenPlainSurface(4,4,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&read9,nullptr));Hr(native->SetRenderTarget(0,target.Get()));
    native->SetRenderState(D3DRS_ZENABLE,FALSE);native->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);native->SetRenderState(D3DRS_LIGHTING,FALSE);
    native->SetRenderState(D3DRS_FOGENABLE,FALSE);native->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);native->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    native->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);native->SetTextureStageState(2,D3DTSS_COLOROP,D3DTOP_DISABLE);
    CheckImmutableTexture(native.Get());
    texture_slot_test::Run(native.Get());
    ComPtr<IDirect3DTexture9> textures[2];for(auto &t:textures)Hr(native->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&t,nullptr));
    std::array<Probe,64> probes{};std::array<std::array<unsigned char,4>,64> expected{};
    for(unsigned i=0;i<64;i++){
        auto &p=probes[i];for(unsigned c=0;c<4;c++){p.tex0[c]=float((17+i*31+c*43)%256)/255;p.tex1[c]=float((91+i*13+c*37)%256)/255;
            p.diffuse[c]=float((63+i*19+c*23)%256)/255;p.factor[c]=float((45+i*17+c*29)%256)/255;}
        p.ops[0]=i%4==0?7:5;p.ops[1]=i%4==1?5:i%4==2?14:1;p.ops[2]=i%2?4:2;p.ops[3]=i<60;
        for(unsigned s=0;s<2;s++){
            D3DLOCKED_RECT locked{};Hr(textures[s]->LockRect(0,&locked,nullptr,0));*static_cast<DWORD *>(locked.pBits)=Argb(s?p.tex1:p.tex0);Hr(textures[s]->UnlockRect(0));
            Hr(native->SetTexture(s,(s==0&&!p.ops[3])?nullptr:textures[s].Get()));native->SetSamplerState(s,D3DSAMP_SRGBTEXTURE,0);
            native->SetTextureStageState(s,D3DTSS_COLOROP,p.ops[s]);native->SetTextureStageState(s,D3DTSS_COLORARG1,D3DTA_TEXTURE);native->SetTextureStageState(s,D3DTSS_COLORARG2,s?D3DTA_CURRENT:D3DTA_DIFFUSE);
            native->SetTextureStageState(s,D3DTSS_ALPHAOP,s?4:p.ops[2]);native->SetTextureStageState(s,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);native->SetTextureStageState(s,D3DTSS_ALPHAARG2,s?D3DTA_CURRENT:D3DTA_DIFFUSE);
        }
        native->SetRenderState(D3DRS_TEXTUREFACTOR,Argb(p.factor));
        struct Vertex {float x,y,z,w;DWORD color;float u,v;};DWORD diffuse=Argb(p.diffuse);
        Vertex vertices[]={{-.5f,-.5f,.5f,1,diffuse,.5f,.5f},{3.5f,-.5f,.5f,1,diffuse,.5f,.5f},{-.5f,3.5f,.5f,1,diffuse,.5f,.5f},{3.5f,3.5f,.5f,1,diffuse,.5f,.5f}};
        native->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);Hr(native->BeginScene());Hr(native->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex)));Hr(native->EndScene());
        Hr(native->GetRenderTargetData(target.Get(),read9.Get()));D3DLOCKED_RECT mapped{};Hr(read9->LockRect(&mapped,nullptr,D3DLOCK_READONLY));
        const auto pixel=static_cast<const unsigned char *>(mapped.pBits);expected[i]={pixel[2],pixel[1],pixel[0],pixel[3]};Hr(read9->UnlockRect());
    }
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11ComputeShader> shader;Hr(device->CreateComputeShader(kCsNativeMaterial,sizeof kCsNativeMaterial,nullptr,&shader));
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof probes;desc.StructureByteStride=sizeof(Probe);desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA initial{probes.data(),0,0};ComPtr<ID3D11Buffer> input,output,read;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
    Hr(device->CreateBuffer(&desc,&initial,&input));Hr(device->CreateShaderResourceView(input.Get(),nullptr,&srv));
    desc.ByteWidth=128*16;desc.StructureByteStride=16;desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Hr(device->CreateBuffer(&desc,nullptr,&output));Hr(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(device->CreateBuffer(&desc,nullptr,&read));
    auto *view=srv.Get();auto *out=uav.Get();context->CSSetShader(shader.Get(),nullptr,0);context->CSSetShaderResources(0,1,&view);context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->Dispatch(1,1,1);
    out=nullptr;context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->CopyResource(read.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
    const auto *values=static_cast<const float (*)[4]>(mapped.pData);
    for(unsigned i=0;i<64;i++)for(unsigned c=0;c<4;c++){
        const float error=std::abs(values[i*2][c]*255-expected[i][c]);if(error>1.1f)std::printf("native stage mismatch case%u channel%u GPU%.6f native%u\n",i,c,values[i*2][c],expected[i][c]);assert(error<=1.1f);
        assert(values[i*2+1][3]==1);if(c<3){const float v=values[i*2][c],linear=v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);assert(std::abs(values[i*2+1][c]-linear)<1e-5f);}
    }
    context->Unmap(read.Get(),0);texture_pages_test::Run(native.Get(),device.Get(),context.Get());native_visibility_test::Run(native.Get(),device.Get(),context.Get());native.Reset();DestroyWindow(window);
    std::puts("native materials: D3D9 fixed-stage RGB/alpha versus D3D11 evaluator, encoded operations then linear decode: ALL PASSED");
}
