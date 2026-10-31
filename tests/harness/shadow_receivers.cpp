// Actual compute shader on WARP: hidden receiver slabs cannot inflate a fitted shadow map.
#include <d3d11.h>
#include <wrl/client.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "mml/mml.h"
#include "shaders/CsShadowReceivers.h"
using Microsoft::WRL::ComPtr;
static void Hr(HRESULT h){assert(SUCCEEDED(h));}
static bool Near(float a,float b){return std::fabs(a-b)<0.002f;}
struct Fit {float draw[4],range[4],basis[3][4],slabs[32][4];};
int main(){
    ComPtr<ID3D11Device> d;ComPtr<ID3D11DeviceContext> c;D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&d,nullptr,&c));
    ComPtr<ID3D11ComputeShader> cs;Hr(d->CreateComputeShader(kCsShadowReceivers,sizeof kCsShadowReceivers,nullptr,&cs));
    MmlGfxConstants k{};k.eye[1]=10;k.right[0]=1;k.up[1]=k.up[2]=0.70710678f;
    k.fwd[1]=-0.70710678f;k.fwd[2]=0.70710678f;k.proj[0]=k.proj[1]=1;
    Fit f{};f.draw[0]=2;f.draw[1]=1;f.range[0]=.05f;f.range[1]=5000;f.range[2]=1;
    f.basis[0][0]=f.basis[1][1]=f.basis[2][2]=1;
    f.slabs[0][0]=f.slabs[0][2]=-100;f.slabs[1][0]=f.slabs[1][2]=100;
    ComPtr<ID3D11Buffer> cb0,cb1,out,read;
    auto cb=[&](UINT size,ComPtr<ID3D11Buffer>&b){D3D11_BUFFER_DESC q{};q.ByteWidth=size;q.BindFlags=D3D11_BIND_CONSTANT_BUFFER;Hr(d->CreateBuffer(&q,nullptr,&b));};
    cb(sizeof k,cb0);cb(sizeof f,cb1);
    D3D11_BUFFER_DESC b{};b.ByteWidth=28;b.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    b.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;b.StructureByteStride=4;Hr(d->CreateBuffer(&b,nullptr,&out));
    b.BindFlags=b.MiscFlags=b.StructureByteStride=0;b.Usage=D3D11_USAGE_STAGING;b.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Hr(d->CreateBuffer(&b,nullptr,&read));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=7;
    ComPtr<ID3D11UnorderedAccessView> uav;Hr(d->CreateUnorderedAccessView(out.Get(),&ud,&uav));
    D3D11_TEXTURE2D_DESC td{};td.Width=2;td.Height=td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32_FLOAT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> depth;ComPtr<ID3D11ShaderResourceView> srv;
    Hr(d->CreateTexture2D(&td,nullptr,&depth));Hr(d->CreateShaderResourceView(depth.Get(),nullptr,&srv));
    float bounds[6];unsigned samples=0;
    auto run=[&](float a,float z){
        const float scene[2]={a,z};const unsigned empty[7]={~0u,~0u,~0u,0,0,0,0};
        c->UpdateSubresource(depth.Get(),0,nullptr,scene,sizeof scene,0);
        c->UpdateSubresource(cb0.Get(),0,nullptr,&k,0,0);c->UpdateSubresource(cb1.Get(),0,nullptr,&f,0,0);
        c->UpdateSubresource(out.Get(),0,nullptr,empty,0,0);
        ID3D11Buffer *buffers[2]={cb0.Get(),cb1.Get()};ID3D11ShaderResourceView *s=srv.Get();ID3D11UnorderedAccessView *u=uav.Get();
        c->CSSetConstantBuffers(0,2,buffers);c->CSSetShaderResources(0,1,&s);c->CSSetUnorderedAccessViews(0,1,&u,nullptr);
        c->CSSetShader(cs.Get(),nullptr,0);c->Dispatch(1,1,1);u=nullptr;c->CSSetUnorderedAccessViews(0,1,&u,nullptr);
        c->CopyResource(read.Get(),out.Get());D3D11_MAPPED_SUBRESOURCE m{};Hr(c->Map(read.Get(),0,D3D11_MAP_READ,0,&m));
        const auto *v=static_cast<const unsigned *>(m.pData);samples=v[6];
        for(int i=0;i<6;i++){unsigned bits=(v[i]&0x80000000)?(v[i]^0x80000000):~v[i];std::memcpy(bounds+i,&bits,4);}
        c->Unmap(read.Get(),0);
    };
    run(10,10);assert(samples==0); // receiver behind opaque foreground
    run(100000,100000);assert(samples==2);
    assert(Near(bounds[0],-7.071068f)&&Near(bounds[3],7.071068f));
    assert(Near(bounds[1],0)&&Near(bounds[4],0)&&Near(bounds[2],10)&&Near(bounds[5],10));
    f.slabs[0][1]=-1;f.slabs[1][1]=1;run(10,100000);assert(samples==1);
    assert(Near(bounds[0],6.363961f)&&Near(bounds[3],7.778175f));
    assert(Near(bounds[1],-1)&&Near(bounds[4],1)&&Near(bounds[2],9)&&Near(bounds[5],11));
    f.draw[2]=.5f;run(10,100000);assert(samples==1&&Near(bounds[0],0)&&Near(bounds[3],0)); // jitter belongs to depth's frame
    f.draw[2]=0;k.fwd[1]=0;k.fwd[2]=1;k.up[1]=1;k.up[2]=0;run(100000,100000);assert(samples==0); // parallel, outside slab
    k.eye[1]=0;run(2,2);assert(samples==2&&bounds[5]<2.1f&&bounds[2]>=.049f); // parallel inside: opaque depth bounds it
    f.slabs[0][0]=30;f.slabs[1][0]=40;run(2,2);assert(samples==0); // finite X bound rejects unrelated rays
    f.range[2]=0;run(100000,100000);assert(samples==0);
    std::puts("shadow receivers: current depth, hidden slabs, signed bounds, height envelope, jitter and parallel rays: ALL PASSED");
}
