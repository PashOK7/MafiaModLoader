#pragma once
#include "mml/native_texture_pages.h"
#include "shaders/CsNativeTexture.h"
namespace texture_pages_test {
struct Probe {unsigned info[4];float uvLod[4];unsigned filter[4];float samplingState[4]{};};
inline void Run(IDirect3DDevice9 *native,ID3D11Device *device,ID3D11DeviceContext *context){
    using namespace mml::scene;
    const D3DFORMAT formats[]={D3DFMT_A8R8G8B8,D3DFMT_A8B8G8R8,D3DFMT_X8R8G8B8,D3DFMT_R5G6B5,D3DFMT_A1R5G5B5,D3DFMT_A4R4G4B4,D3DFMT_DXT1,D3DFMT_DXT3,D3DFMT_DXT5};
    const DXGI_FORMAT dxgi[]={DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8X8_UNORM,DXGI_FORMAT_B5G6R5_UNORM,DXGI_FORMAT_B5G5R5A1_UNORM,DXGI_FORMAT_B4G4R4A4_UNORM,DXGI_FORMAT_BC1_UNORM,DXGI_FORMAT_BC2_UNORM,DXGI_FORMAT_BC3_UNORM};
    std::vector<TextureSnapshot> source;std::vector<ComPtr<IDirect3DTexture9>> textures;
    for(unsigned f=0;f<9;f++){
        const unsigned dimension=f==0?132:8; // one original mip crosses a 64KiB page; all neighbors remain distinct
        ComPtr<IDirect3DTexture9> texture;const HRESULT available=native->CreateTexture(dimension,dimension,4,0,formats[f],D3DPOOL_MANAGED,&texture,nullptr);
        std::printf("texture native format%u (%u) D3D9=%08X\n",f,unsigned(formats[f]),unsigned(available));fflush(stdout);
        TextureSnapshot snapshot;snapshot.native=formats[f];snapshot.format=dxgi[f];
        for(unsigned level=0;level<4;level++){
            const unsigned size=dimension>>level,block=f>=6?4:1,bytes=f>=6?(f==6?8:16):(f>=3?2:4),cols=(size+block-1)/block,rows=(size+block-1)/block;
            TextureMip mip;mip.width=mip.height=size;mip.pitch=cols*bytes;mip.rows=rows;mip.bytes.resize(size_t(mip.pitch)*rows);
            for(unsigned y=0;y<rows;y++)for(unsigned x=0;x<cols;x++){
                auto *at=mip.bytes.data()+y*mip.pitch+x*bytes;
                for(unsigned b=0;b<bytes;b++)at[b]=static_cast<unsigned char>(13+f*37+level*61+x*29+y*19+b*43);
                if(f>=6){auto *color=at+(f==6?0:8);uint16_t ends[2]={uint16_t((x+y)%2?0x18e3:0xdf15),uint16_t((x+y)%2?0xdf15:0x18e3)};memcpy(color,ends,4);
                    if(f==8){at[0]=(x+y)%2?24:231;at[1]=(x+y)%2?231:24;}}
            }
            if(texture){D3DLOCKED_RECT locked{};Hr(texture->LockRect(level,&locked,nullptr,0));
                for(unsigned y=0;y<rows;y++)memcpy(static_cast<unsigned char *>(locked.pBits)+y*locked.Pitch,mip.bytes.data()+y*mip.pitch,mip.pitch);Hr(texture->UnlockRect(level));}
            snapshot.mips.push_back(std::move(mip));
        }
        if(texture){TextureSnapshot read;assert(read.Read(texture.Get())==TextureResult::Valid);for(unsigned l=0;l<4;l++)assert(read.mips[l].bytes==snapshot.mips[l].bytes);snapshot=std::move(read);}
        source.push_back(std::move(snapshot));textures.push_back(std::move(texture));
    }
    TexturePages pages;assert(pages.Build(source));assert(pages.usefulBytes>0&&pages.words.size()*4%TexturePages::PageBytes==0);
    std::printf("texture pages: useful%zu, allocated%zu bytes, %zu mip rectangles\n",pages.usefulBytes,pages.words.size()*4,pages.mips.size());
    GpuTexturePages gpu;assert(gpu.Upload(device,pages,1,1)&&gpu.Ready(1,1)&&!gpu.Ready(2,1)&&!gpu.Ready(1,2));
    std::vector<Probe> probes;std::vector<std::array<float,4>> expected;
    native->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);native->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    native->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    ComPtr<IDirect3DSurface9> target;Hr(native->GetRenderTarget(0,&target));ComPtr<IDirect3DSurface9> read9;Hr(native->CreateOffscreenPlainSurface(4,4,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&read9,nullptr));
    auto sampleNative=[&](unsigned texture,unsigned level,unsigned address,unsigned filter,float u,float v){
        if(!textures[texture])return std::array<float,4>{}; // typed D3D11 reference below still tests this format
        Hr(native->SetTexture(0,textures[texture].Get()));
        native->SetSamplerState(0,D3DSAMP_ADDRESSU,address);native->SetSamplerState(0,D3DSAMP_ADDRESSV,address);
        native->SetSamplerState(0,D3DSAMP_MINFILTER,filter);native->SetSamplerState(0,D3DSAMP_MAGFILTER,filter);
        native->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_POINT);native->SetSamplerState(0,D3DSAMP_MAXMIPLEVEL,level);
        struct Vertex {float x,y,z,w;DWORD color;float u,v;};Vertex vertices[]={{-.5f,-.5f,.5f,1,0xffffffff,u,v},{3.5f,-.5f,.5f,1,0xffffffff,u,v},{-.5f,3.5f,.5f,1,0xffffffff,u,v},{3.5f,3.5f,.5f,1,0xffffffff,u,v}};
        Hr(native->BeginScene());Hr(native->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex)));Hr(native->EndScene());Hr(native->GetRenderTargetData(target.Get(),read9.Get()));
        D3DLOCKED_RECT mapped{};Hr(read9->LockRect(&mapped,nullptr,D3DLOCK_READONLY));const auto *p=static_cast<const unsigned char *>(mapped.pBits);
        std::array<float,4> result{p[2]/255.0f,p[1]/255.0f,p[0]/255.0f,p[3]/255.0f};Hr(read9->UnlockRect());return result;
    };
    const float coords[][2]={{-.13f,.23f},{1.17f,-1.31f},{0,1},{.49f,.51f}};
    for(unsigned f=0;f<9;f++)for(unsigned level=0;level<4;level++)for(unsigned address=1;address<=3;address++)for(unsigned filter=1;filter<=2;filter++)for(const auto &uv:coords){
        Probe p{{f,address,address,0},{uv[0],uv[1],float(level),0},{filter,filter,1,0}};probes.push_back(p);expected.push_back(sampleNative(f,level,address,filter,uv[0],uv[1]));
        if(level<3){p.uvLod[2]=level+.37f;p.filter[2]=2;probes.push_back(p);auto a=expected.back(),b=sampleNative(f,level+1,address,filter,uv[0],uv[1]);for(unsigned c=0;c<4;c++)a[c]=a[c]*.63f+b[c]*.37f;expected.push_back(a);}
    }
    ComPtr<ID3D11ComputeShader> shader;Hr(device->CreateComputeShader(kCsNativeTexture,sizeof kCsNativeTexture,nullptr,&shader));
    std::array<ComPtr<ID3D11Texture2D>,9> referenceTextures;std::array<ComPtr<ID3D11ShaderResourceView>,9> referenceViews;
    for(unsigned f=0;f<9;f++)assert(source[f].Upload(device,&referenceTextures[f],&referenceViews[f]));
    std::array<ComPtr<ID3D11SamplerState>,12> samplers;
    for(unsigned address=1;address<=3;address++)for(unsigned filter=1;filter<=2;filter++)for(unsigned mip=1;mip<=2;mip++){
        D3D11_SAMPLER_DESC d{};d.AddressU=d.AddressV=d.AddressW=D3D11_TEXTURE_ADDRESS_MODE(address);d.MaxLOD=D3D11_FLOAT32_MAX;
        d.Filter=filter==1?(mip==1?D3D11_FILTER_MIN_MAG_MIP_POINT:D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR):
            mip==1?D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT:D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        Hr(device->CreateSamplerState(&d,&samplers[(address-1)*4+(filter-1)*2+(mip-1)]));
    }
    auto run=[&](const std::vector<Probe> &input){
        ComPtr<ID3D11Buffer> buffer,output,read;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
        D3D11_BUFFER_DESC d{};d.ByteWidth=UINT(input.size()*sizeof(Probe));d.StructureByteStride=sizeof(Probe);d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial{input.data(),0,0};Hr(device->CreateBuffer(&d,&initial,&buffer));Hr(device->CreateShaderResourceView(buffer.Get(),nullptr,&srv));
        d.ByteWidth=UINT(input.size()*32);d.StructureByteStride=16;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Hr(device->CreateBuffer(&d,nullptr,&output));Hr(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
        d.Usage=D3D11_USAGE_STAGING;d.MiscFlags=d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(device->CreateBuffer(&d,nullptr,&read));
        assert(gpu.Bind(context,1,1,true));auto *view=srv.Get();auto *out=uav.Get();context->CSSetShader(shader.Get(),nullptr,0);context->CSSetShaderResources(0,1,&view);
        ID3D11ShaderResourceView *references[9];for(unsigned f=0;f<9;f++)references[f]=referenceViews[f].Get();context->CSSetShaderResources(1,9,references);
        ID3D11SamplerState *states[12];for(unsigned s=0;s<12;s++)states[s]=samplers[s].Get();context->CSSetSamplers(0,12,states);
        context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->Dispatch(UINT((input.size()+63)/64),1,1);
        out=nullptr;view=nullptr;context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->CSSetShaderResources(0,1,&view);GpuTexturePages::Unbind(context,true);context->CopyResource(read.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));std::vector<std::array<float,4>> values(input.size()*2);memcpy(values.data(),mapped.pData,values.size()*16);context->Unmap(read.Get(),0);return values;
    };
    auto result=run(probes);float worst=0,nativeBc=0;unsigned bad=0;
    for(unsigned i=0;i<probes.size();i++){assert(result[i*2+1][0]>=0);for(unsigned c=0;c<4;c++){
        float error=std::abs(result[i*2][c]-result[i*2+1][c])*255;
        const float legacy=std::abs(result[i*2][c]-expected[i][c])*255;
        if(textures[probes[i].info[0]]){
            if(probes[i].info[0]>=6){nativeBc=(std::max)(nativeBc,legacy);if(legacy>5.1f)bad++;}
            else error=(std::max)(error,legacy);
        }worst=(std::max)(worst,error);
        if(error>2.1f){if(bad++<12)std::printf("texture mismatch %u format%u lod%.2f address%u filter%u channel%u GPU%.4f typed%.4f native%.4f\n",i,probes[i].info[0],probes[i].uvLod[2],probes[i].info[1],probes[i].filter[0],c,result[i*2][c],result[i*2+1][c],expected[i][c]);}
    }}std::printf("texture samples%zu typed/uncompressed worst%.4f/255, measured D3D9 BC codec difference%.4f/255, bad%u\n",probes.size(),worst,nativeBc,bad);assert(!bad);
    std::vector<Probe> footprint;
    for(unsigned format=0;format<9;format++)for(unsigned level=0;level<4;level++){
        Probe p{{format,1,1,1},{.23f,.51f,float(1u<<level),float(1u<<level)},{2,2,2,1}};footprint.push_back(p);
    }
    auto gradients=run(footprint);
    for(unsigned i=0;i<footprint.size();i++)for(unsigned c=0;c<4;c++)assert(std::abs(gradients[i*2][c]-gradients[i*2+1][c])<2.1f/255);
    auto constant=source;
    for(unsigned level=0;level<4;level++){auto &mip=constant[0].mips[level];
        for(size_t at=0;at<mip.bytes.size();at+=4){mip.bytes[at]=64;mip.bytes[at+1]=96;mip.bytes[at+2]=uint8_t(16+level*64);mip.bytes[at+3]=255;}}
    assert(pages.Build(constant)&&gpu.Upload(device,pages,1,1));footprint.clear();
    for(unsigned aniso:{1u,2u,4u,8u,16u})footprint.push_back({{0,1,1,1},{.23f,.51f,8,1},{3,2,2,aniso}});
    gradients=run(footprint);const unsigned expectedLevel[]={3,2,1,0,0};
    for(unsigned i=0;i<5;i++){assert(gradients[i*2+1][0]>=0);assert(std::abs(gradients[i*2][0]-(16+expectedLevel[i]*64)/255.0f)<.001f);}
    footprint.clear();
    footprint.push_back({{0,1,1,1},{.23f,.51f,8,1},{3,2,2,16},{0,2,0,0}});
    footprint.push_back({{0,1,1,1},{.23f,.51f,8,1},{3,2,2,1},{1.5f,0,0,0}});
    footprint.push_back({{0,1,1,1},{.23f,.51f,8,1},{3,2,2,1},{-1,0,0,0}});
    footprint.push_back({{0,1,1,1},{.23f,.51f,8,1},{3,2,2,16},{-10,2,0,0}});
    footprint.push_back({{0,1,1,1},{.23f,.51f,8,1},{3,2,0,16},{4,2,0,0}});
    gradients=run(footprint);const unsigned stateLevel[]={2,3,2,2,0};
    for(unsigned i=0;i<5;i++)assert(gradients[i*2+1][0]>=0&&std::abs(gradients[i*2][0]-(16+stateLevel[i]*64)/255.0f)<.001f);
    std::puts("texture path footprints: original dimensions/mips/formats and native anisotropy bounds PASSED");
    auto before=result[0];source[0].mips[0].bytes.assign(source[0].mips[0].bytes.size(),0xff);
    assert(pages.Build(source)&&gpu.Upload(device,pages,1,1));auto replaced=run({probes[0]});assert(replaced[0][0]==1&&replaced[0][1]==1&&replaced[0][2]==1&&replaced[0][3]==1&&before!=replaced[0]);
    auto invalid=source;invalid[0].mips[0].pitch++;assert(!pages.Build(invalid)&&!gpu.Upload(device,pages,1,1)&&!gpu.Ready(1,1));
    assert(pages.Build(source)&&gpu.Upload(device,pages,1,1));gpu.Invalidate();assert(!gpu.Bind(context,1,1,false));gpu.Release();assert(!gpu.Ready(1,1));
    std::puts("texture pages: original mips, native packed/BC formats, wrap/mirror/clamp, point/bilinear/trilinear, replacement/failure/lifecycle: ALL PASSED");
}
}
