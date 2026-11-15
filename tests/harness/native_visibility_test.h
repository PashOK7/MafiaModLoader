#pragma once
#include "mml/ray_scene_reference.h"
namespace native_visibility_test {
inline void Run(IDirect3DDevice9 *native,ID3D11Device *device,ID3D11DeviceContext *context){
    ComPtr<IDirect3DSurface9> target,read9;Hr(native->GetRenderTarget(0,&target));Hr(native->CreateOffscreenPlainSurface(4,4,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&read9,nullptr));
    ComPtr<IDirect3DTexture9> texture;Hr(native->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr));native->SetTexture(0,texture.Get());
    native->SetSamplerState(0,D3DSAMP_MAXMIPLEVEL,0);native->SetSamplerState(0,D3DSAMP_MINFILTER,1);native->SetSamplerState(0,D3DSAMP_MAGFILTER,1);
    native->SetTextureStageState(0,D3DTSS_COLOROP,2);native->SetTextureStageState(0,D3DTSS_ALPHAOP,2);native->SetTextureStageState(0,D3DTSS_COLORARG1,2);native->SetTextureStageState(0,D3DTSS_ALPHAARG1,2);
    native->SetTextureStageState(1,D3DTSS_COLOROP,1);native->SetRenderState(D3DRS_CULLMODE,1);
    struct Vertex {float x,y,z,w;DWORD color;float u,v;};
    const Vertex quad[]={{-.5f,-.5f,.5f,1,0xffffffff,.5f,.5f},{3.5f,-.5f,.5f,1,0xffffffff,.5f,.5f},{-.5f,3.5f,.5f,1,0xffffffff,.5f,.5f},{3.5f,3.5f,.5f,1,0xffffffff,.5f,.5f}};
    native->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);
    auto pixel=[&](unsigned x,unsigned y){Hr(native->GetRenderTargetData(target.Get(),read9.Get()));D3DLOCKED_RECT mapped{};Hr(read9->LockRect(&mapped,nullptr,D3DLOCK_READONLY));
        uint32_t value;memcpy(&value,static_cast<const unsigned char *>(mapped.pBits)+y*mapped.Pitch+x*4,4);Hr(read9->UnlockRect());return value;};
    std::vector<Probe> probes;std::vector<bool> expected;
    const unsigned references[]={1,128,200,255};
    for(unsigned function=1;function<=8;function++)for(unsigned ref:references){
        const unsigned alphas[]={0,ref-1,ref,(std::min)(ref+1,255u),255};
        for(unsigned alpha:alphas){D3DLOCKED_RECT locked{};Hr(texture->LockRect(0,&locked,nullptr,0));*static_cast<DWORD *>(locked.pBits)=alpha<<24|0x20c040;Hr(texture->UnlockRect(0));
            native->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE);native->SetRenderState(D3DRS_ALPHAFUNC,function);native->SetRenderState(D3DRS_ALPHAREF,ref);
            Hr(native->Clear(0,nullptr,D3DCLEAR_TARGET,0xff010203,1,0));Hr(native->BeginScene());Hr(native->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex)));Hr(native->EndScene());expected.push_back(pixel(0,0)!=0xff010203);
            Probe p{};p.tex0[0]=32/255.0f;p.tex0[1]=192/255.0f;p.tex0[2]=64/255.0f;p.tex0[3]=alpha/255.0f;std::fill_n(p.diffuse,4,1);std::fill_n(p.factor,4,1);
            p.ops[0]=2;p.ops[1]=1;p.ops[2]=2;p.ops[3]=1;p.info[0]=1;p.info[1]=function;p.info[2]=ref;probes.push_back(p);
        }
    }
    // Fractional diffuse alpha checks the comparison BEFORE render-target byte quantization.
    {D3DLOCKED_RECT locked{};Hr(texture->LockRect(0,&locked,nullptr,0));*static_cast<DWORD *>(locked.pBits)=0xff20c040;Hr(texture->UnlockRect(0));}
    Vertex gradient[4];std::copy_n(quad,4,gradient);gradient[0].color=gradient[2].color=0x00ffffff;
    native->SetTextureStageState(0,D3DTSS_ALPHAOP,3);native->SetTextureStageState(0,D3DTSS_ALPHAARG2,0);
    for(unsigned x=0;x<4;x++)for(unsigned function=1;function<=8;function++)for(unsigned sideRef=0;sideRef<2;sideRef++){
        const float alpha=(x+.5f)/4;const unsigned ref=unsigned(std::floor(alpha*255))+sideRef;
        native->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE);native->SetRenderState(D3DRS_ALPHAFUNC,function);native->SetRenderState(D3DRS_ALPHAREF,ref);
        Hr(native->Clear(0,nullptr,D3DCLEAR_TARGET,0xff010203,1,0));Hr(native->BeginScene());Hr(native->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,gradient,sizeof(Vertex)));Hr(native->EndScene());expected.push_back(pixel(x,0)!=0xff010203);
        Probe p{};p.tex0[0]=32/255.0f;p.tex0[1]=192/255.0f;p.tex0[2]=64/255.0f;p.tex0[3]=alpha;std::fill_n(p.diffuse,4,1);std::fill_n(p.factor,4,1);
        p.ops[0]=2;p.ops[1]=1;p.ops[2]=2;p.ops[3]=1;p.info[0]=1;p.info[1]=function;p.info[2]=ref;probes.push_back(p);
    }
    native->SetTextureStageState(0,D3DTSS_ALPHAOP,2);
    ComPtr<ID3D11ComputeShader> shader;Hr(device->CreateComputeShader(kCsNativeMaterial,sizeof kCsNativeMaterial,nullptr,&shader));
    ComPtr<ID3D11Buffer> input,output,read;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
    D3D11_BUFFER_DESC d{};d.ByteWidth=UINT(probes.size()*sizeof(Probe));d.StructureByteStride=sizeof(Probe);d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{probes.data(),0,0};Hr(device->CreateBuffer(&d,&initial,&input));Hr(device->CreateShaderResourceView(input.Get(),nullptr,&srv));
    d.ByteWidth=UINT(probes.size()*32);d.StructureByteStride=16;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Hr(device->CreateBuffer(&d,nullptr,&output));Hr(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(device->CreateBuffer(&d,nullptr,&read));
    auto *view=srv.Get();auto *out=uav.Get();context->CSSetShaderResources(0,1,&view);context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch(UINT((probes.size()+63)/64),1,1);
    out=nullptr;context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->CopyResource(read.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
    const auto *r=static_cast<const float (*)[4]>(mapped.pData);for(unsigned i=0;i<probes.size();i++){if((r[i*2+1][3]>.5)!=expected[i])std::printf("alpha mismatch function%u ref%u alpha%.4f native%d GPU%.1f\n",probes[i].info[1],probes[i].info[2],probes[i].tex0[3],int(expected[i]),r[i*2+1][3]);assert((r[i*2+1][3]>.5)==expected[i]);}context->Unmap(read.Get(),0);
    native->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    using namespace mml::ray;
    for(unsigned mirror=0;mirror<2;mirror++)for(unsigned cull=1;cull<=3;cull++){
        const float side=mirror?-1.0f:1.0f;
        Vertex triangle[]={{2+1.5f*side,3.5f,.5f,1,0xffffffff,.5f,.5f},{2-2.5f*side,3.5f,.5f,1,0xffffffff,.5f,.5f},{2+1.5f*side,-.5f,.5f,1,0xffffffff,.5f,.5f}};
        // Original object (-1,-1),(1,-1),(-1,1), with x screen = (x+1)*2-.5.
        triangle[0].x=mirror?3.5f:-.5f;triangle[1].x=mirror?-.5f:3.5f;triangle[2].x=mirror?3.5f:-.5f;
        native->SetRenderState(D3DRS_CULLMODE,cull);Hr(native->Clear(0,nullptr,D3DCLEAR_TARGET,0xff010203,1,0));Hr(native->BeginScene());Hr(native->DrawPrimitiveUP(D3DPT_TRIANGLELIST,1,triangle,sizeof(Vertex)));Hr(native->EndScene());
        const bool visible=pixel(mirror?3:0,2)!=0xff010203;
        Scene scene;Triangle t{{-1,-1,0},0,{2,0,0},0,{0,2,0},Opaque|(cull==3?CullBack:cull==2?CullFront:0)};assert(scene.AddMesh({t})==0);
        Placement place;place.world[0]=side;assert(scene.Place({place},{},1));auto hit=ReferenceGeometry(scene,{{-10,-10,-10},{10,10,10}},{mirror?.75f:-.75f,-.25f,-1},{0,0,1},.001f,2);
        assert((hit.status==1)==visible);
    }
    // Validate the native blend factors separately from color encoding. The scene query composes LINEAR
    // radiance; matching the legacy gamma-encoded framebuffer is deliberately not its lighting contract.
    native->SetRenderState(D3DRS_CULLMODE,1);native->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    native->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);native->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    unsigned blendCases=0;const unsigned shifts[]={16,8,0};
    for(unsigned destination:{2u,6u})for(unsigned alpha:{0u,1u,64u,128u,255u}){
        D3DLOCKED_RECT locked{};Hr(texture->LockRect(0,&locked,nullptr,0));
        *static_cast<DWORD *>(locked.pBits)=alpha<<24|0x204060;Hr(texture->UnlockRect(0));
        native->SetRenderState(D3DRS_DESTBLEND,destination);
        Hr(native->Clear(0,nullptr,D3DCLEAR_TARGET,0xff604020,1,0));Hr(native->BeginScene());
        Hr(native->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Vertex)));Hr(native->EndScene());
        const uint32_t actual=pixel(0,0);const float a=alpha/255.0f;
        for(unsigned shift:shifts){const float source=float((0x204060>>shift)&255),background=float((0x604020>>shift)&255);
            const float expectedColor=source*a+background*(destination==6?1-a:1);
            assert(std::abs(float((actual>>shift)&255)-expectedColor)<=1.1f);
        }blendCases++;
    }
    native->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    std::printf("native visibility: %zu D3D9 alpha comparisons, six CW/CCW/mirrored cases, %u alpha/additive blend-factor cases: ALL PASSED\n",probes.size(),blendCases);
}
}
