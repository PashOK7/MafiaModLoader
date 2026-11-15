#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cassert>
#include <cstdio>
#include "mml/ray_scene.h"
#include "mml/ray_scene_gpu.h"
#include "mml/ray_scene_reference.h"
#include "scene_snapshot_test.h"
#include "shaders/CsRaySceneTest.h"
using Microsoft::WRL::ComPtr;
using namespace mml::ray;
static void Hr(HRESULT h){assert(SUCCEEDED(h));}
#include "scene_attributes_test.h"
#include "scene_opacity_test.h"
struct Float4{float x,y,z,w;};
struct Constants{uint32_t counts[4],more[4];float lo[4],hi[4];};
static std::vector<Triangle> Quad(float size,uint32_t material,uint32_t id){
    return {{{-size,-size,0},material,{2*size,0,0},id,{0,2*size,0},Opaque},
            {{size,size,0},material,{-2*size,0,0},id+1,{0,-2*size,0},Opaque}};
}
int main(){
    for(uint32_t sign:{0u,0x80000000u})for(uint32_t exponent=0;exponent<256;exponent++)
        for(uint32_t mantissa:{0u,1u,0x3fffffu,0x400000u,0x7fffffu}){
            const float value=std::bit_cast<float>(sign|(exponent<<23)|mantissa);
            assert(Finite(value)==std::isfinite(value));
            assert(Finite(Vec3{value,0,0})==Finite(value));
            assert(Finite(Vec3{0,value,0})==Finite(value));
            assert(Finite(Vec3{0,0,value})==Finite(value));
        }
    Scene scene;
    auto background=Quad(20,1,100); // many triangles exercise internal BLAS nodes and second mesh root offsets
    for(unsigned i=0;i<31;i++){auto q=Quad(.1f,1,102+i*2);for(auto &t:q)t.p.x+=10+i;background.insert(background.end(),q.begin(),q.end());}
    int bank=scene.AddMesh(background),hull=scene.AddMesh(Quad(1,2,200)),cutout=scene.AddMesh(Quad(1,11,300)),unsupported=scene.AddMesh(Quad(1,12,400));
    assert(bank==0&&hull==1&&cutout==2&&unsupported==3);
    auto bad=Quad(1,1,0);bad[0].e1={};const auto oldVersion=scene.geometryVersion;assert(scene.AddMesh(bad)==-1&&scene.geometryVersion==oldVersion);
    Placement bankPlacement;bankPlacement.mesh=bank;bankPlacement.id=10;bankPlacement.world[14]=8;
    Placement close;close.mesh=hull;close.id=20;close.world[14]=4;
    assert(scene.Place({bankPlacement,close},{},1));
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&ctx));
    snapshot_test::Run(device.Get(),ctx.Get());
    ComPtr<ID3D11ComputeShader> shader;Hr(device->CreateComputeShader(kCsRaySceneTest,sizeof kCsRaySceneTest,nullptr,&shader));
    auto structured=[&](const void *data,UINT stride,UINT count,ComPtr<ID3D11Buffer> &buffer,ComPtr<ID3D11ShaderResourceView> &view){
        D3D11_BUFFER_DESC d{};d.ByteWidth=stride*std::max(count,1u);d.StructureByteStride=stride;d.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<uint8_t> zero(d.ByteWidth);D3D11_SUBRESOURCE_DATA initial{count?data:zero.data()};
        buffer.Reset();view.Reset();Hr(device->CreateBuffer(&d,&initial,&buffer));Hr(device->CreateShaderResourceView(buffer.Get(),nullptr,&view));
    };
    ComPtr<ID3D11Buffer> output,read,cb;ComPtr<ID3D11UnorderedAccessView> uav;
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=16*sizeof(Float4);bd.StructureByteStride=sizeof(Float4);bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    Hr(device->CreateBuffer(&bd,nullptr,&output));Hr(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;bd.MiscFlags=bd.BindFlags=0;Hr(device->CreateBuffer(&bd,nullptr,&read));
    bd={};bd.ByteWidth=sizeof(Constants);bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Hr(device->CreateBuffer(&bd,nullptr,&cb));
    std::array<Float4,16> probes{{{0,0,0,.001f},{0,0,1,10}, {0,0,5,.001f},{0,0,1,5},
        {2,0,0,.001f},{0,0,1,10}, {0,0,5,.001f},{0,0,-1,5},
        {0,0,0,5},{0,0,1,10}, {0,0,0,.001f},{0,0,2,5},
        {0,0,0,.001f},{0,0,1,120}, {0,0,0,.001f},{0,0,0,10}}};
    GpuScene gpu;
    auto run=[&](){
        assert(gpu.Upload(device.Get(),scene,{{-100,-100,-100},{100,100,100}},scene.frameId,1));
        assert(gpu.Ready(scene.frameId,1)&&gpu.SourceFrame()==scene.frameId&&!gpu.Ready(scene.frameId+1,1)&&!gpu.Ready(scene.frameId,2));
        assert(gpu.BindCS(ctx.Get(),scene.frameId,1));
        ComPtr<ID3D11Buffer> probeBuffer;ComPtr<ID3D11ShaderResourceView> probeView;
        structured(probes.data(),sizeof(Float4),static_cast<UINT>(probes.size()),probeBuffer,probeView);
        auto *p=probeView.Get();ctx->CSSetShaderResources(22,1,&p);
        auto *target=uav.Get();ctx->CSSetUnorderedAccessViews(0,1,&target,nullptr);ctx->CSSetShader(shader.Get(),nullptr,0);ctx->Dispatch(1,1,1);
        target=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&target,nullptr);ctx->CopyResource(read.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};Hr(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));std::array<Float4,16> result;memcpy(result.data(),mapped.pData,sizeof result);ctx->Unmap(read.Get(),0);
        GpuScene::UnbindCS(ctx.Get());p=nullptr;ctx->CSSetShaderResources(22,1,&p);return result;
    };
    auto hit=[](Float4 r,unsigned instance,float distance){return r.x==1&&r.z==instance&&std::fabs(r.y-distance)<1e-5;};
    auto a=run();assert(hit(a[0],20,4)&&hit(a[2],10,3)&&hit(a[4],10,8)&&hit(a[6],20,1)&&hit(a[8],10,8)&&hit(a[10],20,2));
    auto reference=[&](const auto &results){for(unsigned i=0;i<8;i++){
        const auto &o=probes[i*2],&d=probes[i*2+1];const auto cpu=ReferenceGeometry(scene,{{-100,-100,-100},{100,100,100}},{o.x,o.y,o.z},{d.x,d.y,d.z},o.w,d.w);
        assert(cpu.status==unsigned(results[i*2].x));if(cpu.status==1){assert(cpu.instance==unsigned(results[i*2].z));assert(std::abs(cpu.distance-results[i*2].y)<1e-5f);}
    }};reference(a);
    assert(a[12].x==1&&a[14].x==3); // known hit before coverage ends; zero direction is invalid
    auto repeat=run();assert(!memcmp(a.data(),repeat.data(),sizeof a)); // there is no observer view to change a world-ray answer
    close.world[12]=4;assert(scene.Place({bankPlacement,close},{},2));auto moved=run();assert(hit(moved[0],10,8));
    reference(moved);
    close.world[12]=0;close.world[0]=close.world[5]=close.world[10]=2;close.world[14]=6;
    assert(scene.Place({bankPlacement,close},{},3));auto scaled=run();assert(hit(scaled[0],20,6)&&hit(scaled[2],20,1));
    reference(scaled);
    // Native CCW/CW sidedness and a negative determinant must agree with the independent CPU query.
    const auto scaledPlacement=close;
    auto sided=Quad(1,3,500);for(auto &t:sided)t.flags=Opaque|CullBack;int sidedMesh=scene.AddMesh(sided);
    close=Placement{};close.mesh=sidedMesh;close.id=40;close.world[14]=4;
    assert(scene.Place({bankPlacement,close},{},4));auto back=run();reference(back);assert(hit(back[0],10,8)&&hit(back[6],40,1));
    close.world[0]=-1;assert(scene.Place({bankPlacement,close},{},5));auto mirror=run();reference(mirror);assert(hit(mirror[0],40,4)&&mirror[6].x==0);
    sided=Quad(1,3,600);for(auto &t:sided)t.flags=Opaque|CullFront;int frontMesh=scene.AddMesh(sided);close.world[0]=1;close.mesh=frontMesh;
    assert(scene.Place({bankPlacement,close},{},6));auto front=run();reference(front);assert(hit(front[0],40,4)&&front[6].x==0);
    close=scaledPlacement;assert(scene.Place({bankPlacement,close},{},3));
    const auto generation=scene.placementVersion;Placement singular=close;singular.world[0]=0;
    assert(!scene.Place({bankPlacement,singular},{},4)&&scene.frameId==3&&scene.placementVersion==generation);
    close=Placement{};close.mesh=cutout;close.id=30;close.world[14]=4;assert(scene.Place({bankPlacement,close},{},5));
    auto alpha=run();assert(hit(alpha[0],10,8)); // center lies on transparent portion: background still exists behind it
    close.mesh=unsupported;assert(scene.Place({bankPlacement,close},{},6));auto missing=run();assert(missing[0].x==2&&std::fabs(missing[0].y-4)<1e-5);
    assert(scene.Place({bankPlacement},{{{-1,-1,2},1,{1,1,3},0}},7));auto unknown=run();assert(unknown[0].x==2&&unknown[0].y==2&&hit(unknown[2],10,3));
    assert(scene.Place({bankPlacement},{},8));auto removed=run();assert(hit(removed[0],10,8));
    assert(scene.Place({}, {},9));auto empty=run();assert(empty[0].x==0&&empty[12].x==2&&empty[12].y==100);
    // A long finite parameter interval can still fit inside coverage for a small nonzero direction. Its miss
    // must not become UNKNOWN merely because float(tMax+1) rounds to tMax.
    probes[0]={0,0,0,0};probes[1]={0,0,1e-8f,1e9f};auto longSegment=run();assert(longSegment[0].x==0);
    assert(!gpu.Upload(device.Get(),scene,{{1,0,0},{-1,0,0}},scene.frameId,1));
    assert(!gpu.Ready(scene.frameId,1)&&!gpu.BindCS(ctx.Get(),scene.frameId,1)); // failed publication cannot use retained old bytes
    auto broken=scene;broken.nodes[0].escape=0;assert(!gpu.Upload(device.Get(),broken,{{-100,-100,-100},{100,100,100}},scene.frameId,1));
    run();gpu.Invalidate();assert(!gpu.BindPS(ctx.Get(),scene.frameId,1));run();gpu.Release();assert(!gpu.Ready(scene.frameId,1));run();
    ComPtr<ID3D11Device> recreated;ComPtr<ID3D11DeviceContext> recreatedContext;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&recreated,nullptr,&recreatedContext));
    assert(gpu.Upload(recreated.Get(),scene,{{-100,-100,-100},{100,100,100}},100,2));
    assert(!gpu.BindCS(ctx.Get(),100,2)&&gpu.BindPS(recreatedContext.Get(),100,2));GpuScene::UnbindPS(recreatedContext.Get());
    {
        std::vector<Placement> city{bankPlacement};
        for(uint32_t i=1;i<=32;i++){
            Placement p;p.mesh=hull;p.id=100+i;p.world[12]=float(3*i);p.world[14]=4;city.push_back(p);
        }
        assert(scene.Place(city,{},101));
        const auto original=scene;
        city.back().world[12]=0;
        assert(scene.Place(city,{},102,&original)&&scene.topRefits==1&&scene.instanceOrder==original.instanceOrder);
        probes[0]={0,0,0,.001f};probes[1]={0,0,1,10};
        const auto refitted=run();assert(hit(refitted[0],132,4));reference(refitted);
        assert(scene.Place(city,{},103));const auto rebuilt=run();assert(!memcmp(refitted.data(),rebuilt.data(),sizeof rebuilt));
        city.pop_back();assert(scene.Place(city,{},104,&scene)&&scene.topRefits==0);assert(hit(run()[0],10,8));
        std::puts("current TLAS refit: moved occluder, rebuilt GPU equivalence, removal fallback: ALL PASSED");
    }
    std::puts("world rays: actual WARP local-mesh/TLAS, moved/scaled/removed occluder and hidden background, alpha, unknown coverage, positive distance: ALL PASSED");
    attributes_test::Run(device.Get(),ctx.Get());
    opacity_test::Run(device.Get(),ctx.Get());
    {
        mml::gfx::ImmutableBuffer previous,next;
        std::vector<uint32_t> original(262144);
        for(size_t i=0;i<original.size();i++)original[i]=uint32_t(i*3+1);
        auto current=original;
        current[70000]=UINT32_MAX;
        current.push_back(123);
        Hr(previous.Make(device.Get(),original));
        Hr(next.Make(device.Get(),current,&previous));
        assert(next.buffer!=previous.buffer&&next.uploadedBytes>0&&next.uploadedBytes<current.size()*sizeof(uint32_t)/2);
        const auto verify=[&](ID3D11Buffer *buffer,const std::vector<uint32_t> &expected){
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(expected.size()*sizeof(uint32_t));
            desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Buffer> readback;
            Hr(device->CreateBuffer(&desc,nullptr,&readback));
            ctx->CopyResource(readback.Get(),buffer);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Hr(ctx->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
            assert(memcmp(mapped.pData,expected.data(),desc.ByteWidth)==0);
            ctx->Unmap(readback.Get(),0);
        };
        verify(next.buffer,current);
        verify(previous.buffer,original);
        std::puts("structured buffer partial upload: current bytes and previous publication: ALL PASSED");
    }
    scene.Clear();assert(scene.AddMesh(Quad(1,2,200))==0);
    Placement billboard;billboard.id=77;billboard.world[12]=5;
    for(unsigned mode:{uint32_t(BillboardFacing),uint32_t(BillboardY)}){
        billboard.flags=mode;assert(scene.Place({billboard},{},200));
        probes[0]={0,0,0,.001f};probes[1]={1,0,0,10};
        // A frozen native XY quad is edge-on to this ray. Both required procedural orientations must hit.
        probes[2]={5,10,0,.001f};probes[3]={0,-1,0,20};
        const auto rotated=run();assert(rotated[0].x==1&&std::abs(rotated[0].y-5)<1e-5f&&rotated[0].z==77);
        auto ref=ReferenceGeometry(scene,{{-100,-100,-100},{100,100,100}},{0,0,0},{1,0,0},.001f,10);
        assert(ref.status==1&&std::abs(ref.distance-rotated[0].y)<1e-5f);
        assert(rotated[2].x==2); // degenerate observer/axis is explicit UNKNOWN, never a frozen quad or sky
    }
    billboard.flags=BillboardFacing|BillboardY;assert(!scene.Place({billboard},{},201));
    std::puts("billboard world rays: per-ray orientation, conservative bounds, CPU reference and degenerate UNKNOWN: ALL PASSED");
}
