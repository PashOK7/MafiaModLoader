#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wrl/client.h>
#include <cassert>
#include <cstdio>
#include "mml/scene_radiance_gpu.h"
#include "shaders/CsSceneBlend.h"
using Microsoft::WRL::ComPtr;
using namespace mml::scene;using namespace mml::ray;
static void Hr(HRESULT h){assert(SUCCEEDED(h));}
static float Linear(float v){return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);}
int main(){
    setvbuf(stdout,nullptr,_IONBF,0);Snapshot snapshot;snapshot.coverage={{-100,-100,-100},{100,100,100}};snapshot.cycle=snapshot.nativeFrame=1;snapshot.materialsCollected=true;
    const uint32_t colors[]={0x80804020,0x40204080,0xff408020,0x55402020};
    const unsigned shifts[]={16,8,0,24};float color[4][4];
    for(unsigned m=0;m<4;m++){
        TextureSnapshot texture;texture.native=D3DFMT_A8R8G8B8;texture.format=DXGI_FORMAT_B8G8R8A8_UNORM;TextureMip mip;mip.width=mip.height=mip.rows=1;mip.pitch=4;mip.bytes.resize(4);memcpy(mip.bytes.data(),colors+m,4);texture.mips.push_back(mip);snapshot.textures.push_back(texture);
        SourceMaterial material;material.flags=MML_SCENE_DRAW_COLLECTION;material.textureKeys[0]=1;material.textureIds[0]=m;material.blend=m!=2;material.sourceBlend=5;material.destinationBlend=m==1?2:6;
        auto &s=material.stages[0];s.color_op=s.alpha_op=2;s.color_arg1=s.alpha_arg1=2;s.address_u=s.address_v=1;s.min_filter=s.mag_filter=1;s.mip_filter=0;material.stages[1].color_op=1;snapshot.materials.push_back(material);snapshot.sources.push_back({});
        for(unsigned c=0;c<4;c++){color[m][c]=float((colors[m]>>shifts[c])&255)/255;if(c<3)color[m][c]=Linear(color[m][c]);}
        SurfaceTriangle surface{};surface.color[0]=surface.color[1]=surface.color[2]=0xffffffff;surface.material=m;
        const uint32_t first=uint32_t(snapshot.surfaces.size());snapshot.surfaces.push_back(surface);snapshot.surfaces.push_back(surface);
        assert(snapshot.geometry.AddMesh({{{-1,-1,0},m,{2,0,0},first,{0,2,0},0},{{1,1,0},m,{-2,0,0},first+1,{0,-2,0},0}})==int(m));
    }
    std::vector<Placement> places;for(unsigned i=0;i<4;i++){Placement p;p.mesh=i;p.id=i+1;p.world[14]=i==3?2:float(i*2+2);places.push_back(p);}
    assert(snapshot.geometry.Place(places,{},snapshot.nativeFrame));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11ComputeShader> shader;Hr(device->CreateComputeShader(kCsSceneBlend,sizeof kCsSceneBlend,nullptr,&shader));
    // First ray is exactly on both triangles' shared edge; one native draw must not blend twice.
    float probes[12][4]={{0,0,0,.001f},{0,0,1,20},{0,0,3,.001f},{0,0,1,17},{0,0,5,.001f},{0,0,1,15},{0,0,9,.001f},{0,0,1,11}};
    ComPtr<ID3D11Buffer> input,output,read;ComPtr<ID3D11ShaderResourceView> srv;ComPtr<ID3D11UnorderedAccessView> uav;
    D3D11_BUFFER_DESC d{};d.ByteWidth=sizeof probes;d.StructureByteStride=16;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{probes,0,0};Hr(device->CreateBuffer(&d,&initial,&input));Hr(device->CreateShaderResourceView(input.Get(),nullptr,&srv));
    d.ByteWidth=8*16;d.BindFlags=D3D11_BIND_UNORDERED_ACCESS;Hr(device->CreateBuffer(&d,nullptr,&output));Hr(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(device->CreateBuffer(&d,nullptr,&read));RadianceGpu gpu;
    auto run=[&](){assert(gpu.Upload(device.Get(),snapshot,1)&&gpu.BindCS(context.Get(),snapshot.cycle,1));auto *view=srv.Get();auto *out=uav.Get();
        context->CSSetShaderResources(22,1,&view);context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch(1,1,1);
        out=nullptr;context->CSSetUnorderedAccessViews(0,1,&out,nullptr);RadianceGpu::UnbindCS(context.Get());context->CopyResource(read.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
        Hr(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));std::array<std::array<float,4>,8> result;memcpy(result.data(),mapped.pData,sizeof result);context->Unmap(read.Get(),0);return result;};
    auto result=run();
    assert(result[1][0]==1&&result[1][1]==4&&result[1][2]==4&&result[0][3]==0); // later coplanar draw first
    assert(result[3][0]==1&&result[3][1]==2&&result[5][0]==1&&result[5][1]==1&&result[7][0]==0&&result[6][3]==1);
    for(unsigned c=0;c<3;c++){
        const float add=color[1][c]*color[1][3]+color[2][c];
        const float glass=color[0][c]*color[0][3]+(1-color[0][3])*add;
        const float expected=color[3][c]*color[3][3]+(1-color[3][3])*glass;
        assert(std::abs(result[0][c]-expected)<1e-6f&&std::abs(result[2][c]-add)<1e-6f&&std::abs(result[4][c]-color[2][c])<1e-6f);
    }
    {
        auto original=snapshot;auto instanced=places;
        snapshot.geometry.Clear();
        assert(snapshot.geometry.AddMesh({{{-1,-1,0},0,{2,0,0},0,{0,2,0},0},
            {{1,1,0},0,{-2,0,0},1,{0,-2,0},0}})==0);
        snapshot.instancedMeshes=snapshot.compactAttributes=true;
        snapshot.primitiveCount=8;snapshot.surfaces.resize(2);
        for(auto &surface:snapshot.surfaces)surface.material=UINT32_MAX;
        for(unsigned i=0;i<instanced.size();i++){
            auto &p=instanced[i];p.mesh=0;p.material=i;p.primitiveBase=2*i;p.primitiveCount=2;
        }
        assert(snapshot.geometry.Place(instanced,{},snapshot.nativeFrame));
        const auto compact=run();assert(!memcmp(result.data(),compact.data(),sizeof compact));
        snapshot=std::move(original);
    }
    std::reverse(places.begin(),places.end());assert(snapshot.geometry.Place(places,{},snapshot.nativeFrame));auto reordered=run();assert(!memcmp(result.data(),reordered.data(),sizeof result));
    assert(snapshot.geometry.Place(places,{{{-1,-1,3.5f},1,{1,1,3.6f},0}},snapshot.nativeFrame));auto missing=run();assert(missing[1][0]==2&&missing[1][1]==2&&missing[0][3]>0&&missing[0][3]<1);
    assert(snapshot.geometry.Place(places,{},snapshot.nativeFrame));snapshot.materials[0].destinationBlend=9;auto unsupported=run();assert(unsupported[1][0]==2);snapshot.materials[0].destinationBlend=6;
    // Opaque later source draw at the same exact distance hides the earlier additive draw.
    for(auto &p:places)if(p.id==3)p.world[14]=4;assert(snapshot.geometry.Place(places,{},snapshot.nativeFrame));auto opaqueTie=run();assert(opaqueTie[3][1]==1&&opaqueTie[3][2]==3);
    std::vector<Placement> many;snapshot.sources.resize(40);for(unsigned i=0;i<40;i++){Placement p;p.mesh=0;p.id=i+1;p.world[14]=1+i*.1f;many.push_back(p);}assert(snapshot.geometry.Place(many,{},snapshot.nativeFrame));auto bounded=run();assert(bounded[1][0]==2&&bounded[1][1]==32&&bounded[0][3]>0);
    // Rejected cutouts do not consume the32 contributing-layer bound, and the
    // second triangle of the same draw cannot trap an exact-depth cursor.
    snapshot.materials[0].alphaTest=1;snapshot.materials[0].alphaReference=255;snapshot.materials[0].alphaFunction=7;
    snapshot.sources.resize(41);Placement background;background.mesh=2;background.id=41;background.world[14]=8;many.push_back(background);
    assert(snapshot.geometry.Place(many,{},snapshot.nativeFrame));auto cutouts=run();
    assert(cutouts[1][0]==1&&cutouts[1][1]==1&&cutouts[1][2]==41&&cutouts[0][3]==0);
    for(unsigned c=0;c<3;c++)assert(std::abs(cutouts[0][c]-color[2][c])<1e-6f);
    Scene local;
    std::vector<Triangle> depthLayers;
    for(unsigned z=0;z<5;z++)depthLayers.push_back({{-1,-1,float(z)},2,{2,0,0},0,{1,2,0},0});
    assert(local.AddMesh(depthLayers)==0);
    const int layered=snapshot.geometry.AppendMesh(local,2,4);
    assert(layered>=0);
    Placement layeredInstance;layeredInstance.mesh=layered;layeredInstance.id=42;layeredInstance.world[14]=10;
    many.push_back(layeredInstance);
    probes[0][2]=20;probes[1][2]=-1;
    context->UpdateSubresource(input.Get(),0,nullptr,probes,0,0);
    assert(snapshot.geometry.Place(many,{},snapshot.nativeFrame));auto reversed=run();
    assert(reversed[1][0]==1&&reversed[1][1]==1&&reversed[1][2]==42&&reversed[1][3]==6);
    probes[0][2]=0;probes[1][2]=1;
    context->UpdateSubresource(input.Get(),0,nullptr,probes,0,0);
    // A rejected later draw must reveal the earlier, same-distance draw.
    snapshot.materials[3].alphaTest=1;snapshot.materials[3].alphaReference=255;snapshot.materials[3].alphaFunction=7;
    for(auto &p:places)if(p.id==3)p.world[14]=6;
    assert(snapshot.geometry.Place(places,{},snapshot.nativeFrame));auto alphaTie=run();
    assert(alphaTie[1][0]==1&&alphaTie[1][1]==2&&alphaTie[1][2]==2);
    // The same world hit uses its own footprint for both color and cutout alpha.
    TextureSnapshot filtered=*snapshot.textures[0].value;
    const uint32_t red[]={0xffff0000,0xffff0000,0xffff0000,0xffff0000},blue=0xff0000ff;
    auto &large=filtered.mips[0];
    large.width=large.height=large.rows=2;large.pitch=8;large.bytes.resize(sizeof red);
    memcpy(large.bytes.data(),red,sizeof red);
    TextureMip coarseMip;coarseMip.width=coarseMip.height=coarseMip.rows=1;coarseMip.pitch=4;coarseMip.bytes.resize(sizeof blue);
    memcpy(coarseMip.bytes.data(),&blue,sizeof blue);filtered.mips.push_back(coarseMip);
    snapshot.textures[0]=filtered;
    snapshot.materials[0].blend=0;snapshot.materials[0].stages[0].mip_filter=1;
    snapshot.surfaces[0].uv[1][0]=1;snapshot.surfaces[0].uv[2][1]=1;
    snapshot.surfaces[1].uv[0][0]=snapshot.surfaces[1].uv[0][1]=1;
    snapshot.surfaces[1].uv[1][1]=snapshot.surfaces[1].uv[2][0]=1;
    Placement close;close.id=1;close.world[14]=2;
    assert(snapshot.geometry.Place({close,background},{},snapshot.nativeFrame));
    const auto sharp=run();assert(sharp[0][0]==1&&sharp[0][2]==0&&sharp[1][2]==1);
    probes[8][0]=1;context->UpdateSubresource(input.Get(),0,nullptr,probes,0,0);
    const auto wide=run();assert(wide[0][0]==0&&wide[0][2]==1&&wide[1][2]==1);
    const uint32_t transparentBlue=0x000000ff;
    memcpy(filtered.mips[1].bytes.data(),&transparentBlue,sizeof transparentBlue);
    snapshot.textures[0]=filtered;
    const auto filteredCutout=run();assert(filteredCutout[1][2]==41);
    for(unsigned c=0;c<3;c++)assert(std::abs(filteredCutout[0][c]-color[2][c])<1e-6f);
    snapshot.geometry.Clear();
    assert(snapshot.geometry.AddMesh({{{-1,-1,0},0,{2,0,0},0,{0,2,0},0},
        {{1,1,0},0,{-2,0,0},1,{0,-2,0},0}})==0);
    snapshot.instancedMeshes=true;
    close.mesh=background.mesh=0;
    close.material=0;close.primitiveBase=0;close.primitiveCount=2;
    background.material=2;background.primitiveBase=4;background.primitiveCount=2;
    assert(snapshot.geometry.Place({close,background},{},snapshot.nativeFrame));
    const auto sharedCutout=run();
    assert(!memcmp(filteredCutout.data(),sharedCutout.data(),sizeof sharedCutout));
    probes[8][0]=0;context->UpdateSubresource(input.Get(),0,nullptr,probes,0,0);
    const auto sharedSharp=run();
    assert(sharedSharp[0]==sharp[0]&&sharedSharp[1]==sharp[1]);
    snapshot.compactAttributes=true;snapshot.primitiveCount=uint32_t(snapshot.surfaces.size());
    std::vector<SurfaceTriangle> compactSurfaces;
    for(const auto &triangle:snapshot.geometry.triangles){
        auto surface=snapshot.surfaces[triangle.primitive];surface.material=UINT32_MAX;compactSurfaces.push_back(surface);
    }
    snapshot.surfaces=std::move(compactSurfaces);
    const auto compactSharp=run();assert(!memcmp(sharedSharp.data(),compactSharp.data(),sizeof compactSharp));
    probes[8][0]=1;context->UpdateSubresource(input.Get(),0,nullptr,probes,0,0);
    const auto compactCutout=run();assert(!memcmp(sharedCutout.data(),compactCutout.data(),sizeof compactCutout));
    std::puts("shared local BLAS: per-instance material, primitive and filtered cutout match copied geometry: ALL PASSED");
    {
        std::vector<TextureSnapshot> packedSource(2);
        for(unsigned i=0;i<packedSource.size();i++){
            auto &texture=packedSource[i];
            texture.native=D3DFMT_R5G6B5;texture.format=DXGI_FORMAT_B5G6R5_UNORM;
            const unsigned width=i?1u:1500u;
            TextureMip mip;mip.width=mip.height=mip.rows=width;mip.pitch=width*2;
            mip.bytes.resize(size_t(mip.pitch)*mip.rows);
            for(size_t j=0;j<mip.bytes.size();j++)mip.bytes[j]=uint8_t(j*29+(j>>17)*11+i);
            texture.mips.push_back(std::move(mip));
        }
        TexturePages expected;assert(expected.Build(packedSource)&&expected.packedBytes>4u*1024u*1024u);
        GpuTexturePages streamed;size_t bytes=0;
        assert(streamed.UploadStreamed(device.Get(),packedSource,1,1,&bytes)&&bytes==expected.packedBytes);
        assert(streamed.Bind(context.Get(),1,1,true));
        ComPtr<ID3D11ShaderResourceView> payload;
        context->CSGetShaderResources(25,1,&payload);
        ComPtr<ID3D11Resource> packedBuffer;
        payload->GetResource(&packedBuffer);
        GpuTexturePages::Unbind(context.Get(),true);
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(bytes);desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Buffer> staging;Hr(device->CreateBuffer(&desc,nullptr,&staging));
        context->CopyResource(staging.Get(),packedBuffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Hr(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
        assert(memcmp(mapped.pData,expected.words.data(),bytes)==0);
        context->Unmap(staging.Get(),0);
        std::puts("streamed texture pages: full byte equality across chunks, native mip and page padding: ALL PASSED");
    }
    std::puts("water ray-hit footprints: same geometry selects original mip or minified mip; filtered alpha exposes deeper geometry: ALL PASSED");
    std::puts("scene blend: linear alpha/additive layers, coplanar source order, shared-edge dedup, deeper background, unknown/unsupported, explicit32-layer bound and80 rejected cutout triangles: ALL PASSED");
}
