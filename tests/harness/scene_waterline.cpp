#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cassert>
#include <cstdio>
#include <wrl/client.h>
#include "mml/scene_waterline.h"
#include "mml/scene_waterline_gpu.h"
#include "shaders/CsWaterlineTest.h"
using namespace mml::scene;
using namespace mml::ray;
static void Near(float a,float b){assert(std::abs(a-b)<.0001f);}
static std::vector<Triangle> Wall(float width=5,float height=3){return {
    {{-width,-height,0},0,{2*width,0,0},0,{0,2*height,0},Opaque},
    {{width,height,0},0,{-2*width,0,0},1,{0,-2*height,0},Opaque}};}
static SourceInstance MakeInstance(float x,float z){SourceInstance s{};s.kind=1;s.world[0]=s.world[5]=s.world[10]=s.world[15]=1;s.world[12]=x;s.world[14]=z;return s;}
static Snapshot SceneWith(const std::vector<Triangle> &triangles){
    Snapshot s;s.coverage={{-100,-20,-100},{100,20,100}};s.nativeFrame=s.cycle=1;s.materialsCollected=true;s.materials.resize(1);
    s.sources.push_back(MakeInstance(0,4));const int mesh=s.geometry.AddMesh(triangles);assert(mesh==0);
    Placement p;p.mesh=mesh;p.id=1;std::copy_n(s.sources[0].world,16,p.world);assert(s.geometry.Place({p},{},1));return s;
}
static void CheckGpu(){
    using Microsoft::WRL::ComPtr;const auto hr=[](HRESULT code){assert(SUCCEEDED(code));};
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context));
    ComPtr<ID3D11ComputeShader> shader;hr(device->CreateComputeShader(kCsWaterlineTest,sizeof kCsWaterlineTest,nullptr,&shader));
    struct Point {float x,y,z,r;};const Point points[]={{0,0,3.5f,1},{5.3f,0,3.6f,1},{0,1,3.5f,1},{100,0,0,1},{0,0,4,1},{0,0,0,0},{20,0,3.75f,1},{0,0,2,1},
        {-4.0001f,0,3.5f,1},{-3.9999f,0,3.5f,1},{0,0,2,3},{0,0,3.6f,.5f}};
    constexpr unsigned pointCount=sizeof points/sizeof points[0];
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof points;desc.StructureByteStride=16;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{points,0,0};ComPtr<ID3D11Buffer> input,output,read;ComPtr<ID3D11ShaderResourceView> view;ComPtr<ID3D11UnorderedAccessView> uav;
    hr(device->CreateBuffer(&desc,&initial,&input));hr(device->CreateShaderResourceView(input.Get(),nullptr,&view));
    desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;hr(device->CreateBuffer(&desc,nullptr,&output));hr(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    desc.MiscFlags=desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;hr(device->CreateBuffer(&desc,nullptr,&read));
    auto triangles=Wall();for(unsigned i=0;i<31;i++){auto extra=Wall(1,2);for(auto &t:extra)t.p.x+=20+i*2;triangles.insert(triangles.end(),extra.begin(),extra.end());}
    auto source=SceneWith(triangles);auto model=std::make_shared<WaterlineModel>();assert(BuildWaterline(source,{0},*model));assert(model->nodes.size()>1);
    WaterlineGpu gpu;assert(gpu.UploadOwned(device.Get(),model,7));
    const auto query=[&](const WaterlineModel &reference,unsigned generation){
        assert(gpu.BindCS(context.Get(),reference.cycle,generation));assert(!gpu.BindCS(context.Get(),reference.cycle+1,generation));
        auto *srv=view.Get();auto *out=uav.Get();context->CSSetShaderResources(0,1,&srv);context->CSSetUnorderedAccessViews(0,1,&out,nullptr);context->CSSetShader(shader.Get(),nullptr,0);context->Dispatch(pointCount,1,1);
        out=nullptr;context->CSSetUnorderedAccessViews(0,1,&out,nullptr);WaterlineGpu::UnbindCS(context.Get());context->CopyResource(read.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};hr(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));const auto *answers=static_cast<const Point *>(mapped.pData);
        for(unsigned i=0;i<pointCount;i++){const auto expected=WaterlineReference(reference,{points[i].x,points[i].y,points[i].z},points[i].r);assert(unsigned(answers[i].y)==expected.status);Near(answers[i].x,expected.distance);}
        context->Unmap(read.Get(),0);
    };
    query(*model,7);std::weak_ptr<const WaterlineModel> previous=model;
    auto same=std::make_shared<WaterlineModel>(*model);same->cycle++;
    assert(gpu.UploadOwned(device.Get(),same,7));model.reset();assert(previous.expired());query(*same,7);
    auto uncertain=std::make_shared<WaterlineModel>(*same);uncertain->cycle++;uncertain->unknown={{{-.1f,-1,3.4f},1,{.1f,1,3.6f},0}};
    assert(gpu.UploadOwned(device.Get(),uncertain,7));query(*uncertain,7);
    auto invalid=std::make_shared<WaterlineModel>(*uncertain);invalid->cycle++;invalid->nodes[0].escape=uint32_t(invalid->nodes.size()+1);
    assert(!gpu.UploadOwned(device.Get(),invalid,7)&&!gpu.Ready(uncertain->cycle,7));
    assert(gpu.UploadOwned(device.Get(),same,8)&&!gpu.BindCS(context.Get(),same->cycle,7));query(*same,8);
    ComPtr<ID3D11Device> another;ComPtr<ID3D11DeviceContext> otherContext;
    hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&another,nullptr,&otherContext));
    assert(!gpu.BindCS(otherContext.Get(),same->cycle,8));gpu.Release();assert(!gpu.Ready(same->cycle,8));
    std::puts("waterline GPU: exact local cells/BVH/reference, unknown, current CPU lease, failed publication, frame/device generation and release PASSED");
}
int main(){
    auto source=SceneWith(Wall());WaterlineModel model;
    assert(BuildWaterline(source,{0},model)&&model.definite==2&&model.uncertain==0);
    Near(WaterlineReference(model,{0,0,3.5f},1).distance,.5f);
    Near(WaterlineReference(model,{4.5f,0,3.5f},1).distance,.5f);
    Near(WaterlineReference(model,{5.3f,0,3.6f},1).distance,.5f);
    Near(WaterlineReference(model,{0,1,3.5f},1).distance,1); // another water plane is independent
    assert(WaterlineReference(model,{100,0,0},1).status==2);
    assert(WaterlineReference(model,{0,0,0},0).status==3);
    source.sources[0]=MakeInstance(10,20);source.sources[0].world[0]=0;source.sources[0].world[2]=-2;
    source.sources[0].world[8]=3;source.sources[0].world[10]=0;
    Placement rotated;rotated.mesh=0;rotated.id=1;std::copy_n(source.sources[0].world,16,rotated.world);
    source.nativeFrame=source.cycle=2;assert(source.geometry.Place({rotated},{},2));
    assert(BuildWaterline(source,{0},model));Near(WaterlineReference(model,{9.5f,0,20},1).distance,.5f);
    Near(WaterlineReference(model,{0,0,3.5f},1).distance,1); // previous pose is absent
    source=SceneWith(Wall());const int second=source.geometry.AddMesh(Wall(2,2));assert(second==1);
    source.sources.push_back(MakeInstance(0,1));Placement bank,hull;bank.mesh=0;bank.id=1;hull.mesh=1;hull.id=2;
    std::copy_n(source.sources[0].world,16,bank.world);std::copy_n(source.sources[1].world,16,hull.world);
    assert(source.geometry.Place({bank,hull},{},1));assert(BuildWaterline(source,{0},model));
    Near(WaterlineReference(model,{0,0,.5f},5).distance,.5f);
    source.sources[1].world[14]=hull.world[14]=12;source.cycle=source.nativeFrame=2;
    assert(source.geometry.Place({bank,hull},{},2)&&BuildWaterline(source,{0},model));
    Near(WaterlineReference(model,{0,0,.5f},5).distance,3.5f); // moving hull cannot flatten distant water
    source.geometry.unknown={{{-.1f,-1,3.4f},1,{.1f,1,3.6f},0}};
    assert(BuildWaterline(source,{0},model));assert(WaterlineReference(model,{0,0,3.5f},1).status==2);
    source=SceneWith(Wall());source.materials[0].alphaTest=1;source.materials[0].alphaFunction=5;
    assert(BuildWaterline(source,{0},model)&&model.uncertain==2);
    assert(WaterlineReference(model,{0,0,3.5f},1).status==2);
    source.materials[0].alphaFunction=8;assert(BuildWaterline(source,{0},model)&&model.uncertain==0);
    source.materials[0].alphaFunction=1;assert(BuildWaterline(source,{0},model)&&model.elements.empty());
    source=SceneWith(Wall());source.sources[0].kind=3;
    assert(BuildWaterline(source,{0},model)&&model.elements.empty()&&model.billboards==1);
    source=SceneWith({{{-2,0,-2},0,{4,0,0},0,{0,0,4},Opaque}});source.sources[0]=MakeInstance(0,0);
    Placement flat;flat.id=1;assert(source.geometry.Place({flat},{},1));
    assert(BuildWaterline(source,{0},model)&&(model.elements[0].flags&WaterlineArea));
    Near(WaterlineReference(model,{-1,0,-1},5).distance,0);
    Near(WaterlineReference(model,{2,0,2},5).distance,std::sqrt(8.0f));
    WaterlineElement collapsed{};collapsed.flags=WaterlineArea;Near(float(WaterlineDistance(collapsed,{3,0,4})),25);
    source=SceneWith({{{0,0,0},0,{1,1,0},0,{0,1,1},Opaque}});
    assert(BuildWaterline(source,{0},model)&&model.elements.size()==1);
    Near(WaterlineReference(model,{.3f,0,3.6f},1).distance,.5f);
    source.nativeFrame++;assert(!BuildWaterline(source,{0},model));
    for(unsigned function=1;function<=8;function++)for(unsigned reference:{0u,1u,200u,255u})for(unsigned alpha=0;alpha<256;alpha++){
        const bool pass=function==1?false:function==2?alpha<reference:function==3?alpha==reference:function==4?alpha<=reference:
            function==5?alpha>reference:function==6?alpha!=reference:function==7?alpha>=reference:true;
        assert(NativeAlphaCoverage({alpha/255.0f,alpha/255.0f},function,reference)==unsigned(pass));
    }
    source=SceneWith(Wall());source.surfaces.resize(2);
    for(auto &surface:source.surfaces){surface.material=0;for(unsigned v=0;v<3;v++){surface.color[v]=0xffffffff;surface.uv[v][0]=surface.uv[v][1]=.25f;}}
    auto &m=source.materials[0];m.alphaTest=1;m.alphaFunction=7;m.alphaReference=200;m.textureKeys[0]=1;m.textureIds[0]=0;
    m.stages[0].color_op=4;m.stages[0].alpha_op=2;m.stages[0].alpha_arg1=2;m.stages[0].alpha_arg2=0;
    m.stages[0].address_u=m.stages[0].address_v=3;m.stages[0].mag_filter=1;
    TextureSnapshot texture;texture.native=D3DFMT_A8R8G8B8;TextureMip mip;mip.width=mip.height=mip.rows=4;mip.pitch=16;mip.bytes.resize(64);
    mip.bytes[2*16+2*4+3]=255;texture.mips.push_back(mip);source.textures.push_back(TextureRef(std::move(texture)));
    assert(BuildWaterline(source,{0},model)&&model.elements.empty()); // provably clear card section is no wall
    for(auto &surface:source.surfaces)for(unsigned v=0;v<3;v++)surface.uv[v][0]=surface.uv[v][1]=.625f;
    assert(BuildWaterline(source,{0},model)&&model.definite==2&&model.uncertain==0);
    m.stages[0].mag_filter=2;assert(BuildWaterline(source,{0},model)&&model.uncertain==2); // partial texel support stays unknown
    TextureSnapshot bc;bc.native=D3DFMT_DXT1;TextureMip block;block.width=block.height=4;block.pitch=8;block.rows=1;block.bytes.resize(8);
    block.bytes[2]=255;block.bytes[3]=255;block.bytes[4]=3;bc.mips.push_back(block);float a;
    assert(NativeAlphaTexel(bc,0,0,a));Near(a,0);assert(NativeAlphaTexel(bc,1,0,a));Near(a,1);
    bc.native=D3DFMT_DXT3;bc.mips[0].pitch=16;bc.mips[0].bytes.resize(16);bc.mips[0].bytes[0]=0xf0;
    assert(NativeAlphaTexel(bc,0,0,a));Near(a,0);assert(NativeAlphaTexel(bc,1,0,a));Near(a,1);
    bc.native=D3DFMT_DXT5;std::fill(bc.mips[0].bytes.begin(),bc.mips[0].bytes.end(),0);bc.mips[0].bytes[0]=255;
    bc.mips[0].bytes[2]=7;bc.mips[0].bytes[7]=7<<5;
    assert(NativeAlphaTexel(bc,0,0,a));Near(a,1/7.0f);assert(NativeAlphaTexel(bc,3,3,a));Near(a,1/7.0f);
    CheckGpu();std::puts("scene_waterline: world plane/edge/corner, affine pose, local moving hull, unknown/alpha, billboard and coplanar tests PASSED");
}
