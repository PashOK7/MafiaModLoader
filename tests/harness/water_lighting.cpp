#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include "shaders/CsWaterLightingTest.h"
using Microsoft::WRL::ComPtr;
static void Hr(HRESULT result) { assert(SUCCEEDED(result)); }
int main() {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&ctx));
    ComPtr<ID3D11ComputeShader> shader;Hr(device->CreateComputeShader(kCsWaterLightingTest,sizeof kCsWaterLightingTest,nullptr,&shader));
    D3D11_BUFFER_DESC bd{};bd.ByteWidth=64*16;bd.StructureByteStride=16;bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Buffer> output,read;Hr(device->CreateBuffer(&bd,nullptr,&output));
    bd.BindFlags=0;bd.MiscFlags=0;bd.Usage=D3D11_USAGE_STAGING;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(device->CreateBuffer(&bd,nullptr,&read));
    ComPtr<ID3D11UnorderedAccessView> uav;Hr(device->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    auto *out=uav.Get();ctx->CSSetShader(shader.Get(),nullptr,0);ctx->CSSetUnorderedAccessViews(0,1,&out,nullptr);ctx->Dispatch(1,1,1);
    out=nullptr;ctx->CSSetUnorderedAccessViews(0,1,&out,nullptr);ctx->CopyResource(read.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};Hr(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
    const auto *p=static_cast<const float (*)[4]>(mapped.pData);
    for(unsigned i=0;i<64;i++) for(unsigned j=0;j<4;j++) assert(std::isfinite(p[i][j]));
    std::printf("direct sun aligned/45/90/180: %.7g %.7g %.7g %.7g\n",p[0][0],p[0][1],p[0][2],p[0][3]);
    assert(p[0][0]>1 && p[0][1]<p[0][0]*1e-4 && p[0][2]<1e-8 && p[0][3]<1e-8);
    for(float x:p[1]) assert(x==0); // no colored emitter at zero scattering; no below-horizon sun
    assert(std::fabs(p[2][0]-p[2][1])<1e-4*std::max(p[2][0],1.0f)); // BRDF reciprocity
    const float eta=1.333f,reflectance=(eta-1)*(eta-1)/((eta+1)*(eta+1));
    const float beam=.2f/.3f*.5f*(1-reflectance)/(eta*eta)/(4*3.14159265359f);
    assert(std::fabs(p[3][0]-beam)<1e-6 && p[3][0]>p[3][1] && p[3][2]==0);
    assert(std::fabs(p[3][3]-beam*(1-reflectance))<1e-6); // entry in medium helper; eye-side Fresnel once in material
    assert(std::fabs(p[4][0]-.1f)<1e-6 && std::fabs(p[4][1]-.2f)<1e-6 && std::fabs(p[4][2]-.3f)<1e-6);
    assert(p[5][0]<=1 && p[5][0]>.99f && std::fabs(p[5][1]/p[5][0]-.5f)<1e-6 && std::fabs(p[5][2]/p[5][0]-.25f)<1e-6);
    assert(p[6][0]>=0 && p[6][1]<=1 && p[6][2]<=1);
    assert(p[7][0]>0 && p[7][0]<1e-6 && p[7][3]==0);
    assert(p[8][0]==1 && p[8][1]==1 && p[8][2]==0 && p[8][3]==1); // foreground hull is not a shore contact
    assert(p[9][0]==0); // upward flat-water ray escapes; its direction is never bent
    assert(p[10][0]==1 && std::fabs(p[10][1]-1)<.001f);
    assert(p[11][0]==1 && std::fabs(p[11][1]-.8f)<.001f);
    assert(p[12][0]==1 && std::fabs(p[12][1]-1.0471976f)<.002f); // horizontal ray hits the next crest
    assert(p[13][0]==1 && p[13][1]>0 && std::fabs(p[13][3])<.001f);
    assert(p[14][0]==2); // finite search exhaustion is not a proven sky miss
    for(unsigned i=15;i<64;i++) assert(p[i][0]>=-1e-6 && p[i][1]>=0 && p[i][2]<=1 && p[i][3]<=1);
    ctx->Unmap(read.Get(),0);
    std::puts("water lighting: actual WARP sun localization/reciprocity, facets/energy, wave intersections, medium and display: ALL PASSED");
}
