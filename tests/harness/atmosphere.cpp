// Actual shared HLSL on WARP, compared with numerical integration (crossings, horizontal rays and limits).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include "mml/mml.h"
#include "shaders/CsAtmosphereTest.h"
#include "shaders/PsPreComposite.h"
#include "shaders/VsFullScreen.h"
using Microsoft::WRL::ComPtr;
static void Hr(HRESULT h) { assert(SUCCEEDED(h)); }
struct Case { float from, to, distance, falloff; };
static double Integrate(const Case &c, double falloff) {
    double result = 0;
    constexpr int steps = 100000;
    for (int i = 0; i < steps; i++) {
        double height = c.from + (c.to - c.from) * (i + 0.5) / steps;
        result += std::exp(-std::max(height, 0.0) * falloff);
    }
    return result * c.distance / steps;
}
int main() {
    const Case cases[] = {{0,0,1000,0.02f},{20,20,1000,0.02f},{-20,-20,1000,0.02f},
        {-20,20,1000,0.02f},{20,-20,1000,0.02f},{-200,300,1000,0.02f},
        {300,-200,1000,0.02f},{0,1000,1000,0.02f},{1000,0,1000,0.02f},
        {100,100.00001f,1000,0.02f},{10000,10000,1000,0.02f},
        {0,1000,1000,0},{0,0,100000,0.02f},{-1,1,2,0.02f},{0,0,0.01f,0.02f}};
    constexpr unsigned count = sizeof cases / sizeof cases[0];
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    Hr(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&dev,nullptr,&ctx));
    ComPtr<ID3D11ComputeShader> shader;
    Hr(dev->CreateComputeShader(kCsAtmosphereTest,sizeof kCsAtmosphereTest,nullptr,&shader));
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof cases; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = sizeof(Case);
    D3D11_SUBRESOURCE_DATA init{cases,0,0};
    ComPtr<ID3D11Buffer> input, output, staging, constants;
    Hr(dev->CreateBuffer(&bd,&init,&input));
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    Hr(dev->CreateBuffer(&bd,nullptr,&output));
    bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; bd.MiscFlags = 0;
    Hr(dev->CreateBuffer(&bd,nullptr,&staging));
    bd = {}; bd.ByteWidth = sizeof(MmlGfxConstants); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    MmlGfxConstants k{};
    k.atmosphere[1] = 0.002f; k.atmosphere[3] = 0.02f;
    k.atmosphere_info[0] = 1; k.atmosphere_info[2] = 0.995f; k.atmosphere_info[3] = 1;
    k.atmosphere_phase[0] = 0.55f; k.atmosphere_phase[1] = 8;
    D3D11_SUBRESOURCE_DATA cb{&k,0,0};
    Hr(dev->CreateBuffer(&bd,&cb,&constants));
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    Hr(dev->CreateShaderResourceView(input.Get(),nullptr,&srv));
    Hr(dev->CreateUnorderedAccessView(output.Get(),nullptr,&uav));
    ctx->CSSetShader(shader.Get(),nullptr,0);
    ctx->CSSetShaderResources(4,1,srv.GetAddressOf());
    ctx->CSSetUnorderedAccessViews(0,1,uav.GetAddressOf(),nullptr);
    ctx->CSSetConstantBuffers(0,1,constants.GetAddressOf());
    for (int mode = 0; mode < 3; mode++) {
        k.atmosphere_info[0] = mode == 1 ? 0.0f : 1.0f;
        k.atmosphere[1] = mode == 2 ? 0.0f : 0.002f;
        ctx->UpdateSubresource(constants.Get(),0,nullptr,&k,0,0);
        ctx->Dispatch(count,1,1);
        ctx->CopyResource(staging.Get(),output.Get());
        D3D11_MAPPED_SUBRESOURCE map{}; Hr(ctx->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
        const float *answer = static_cast<const float *>(map.pData);
        for (unsigned i=0;i<count;i++) {
            double expected = Integrate(cases[i],cases[i].falloff);
            assert(std::isfinite(answer[4*i]) && std::abs(answer[4*i]-expected) < std::max(0.003,expected*0.00003));
            double opacity = mode ? 0 : std::min(0.995,1-std::exp(-0.002*Integrate(cases[i],0.02)));
            assert(std::abs(answer[4*i+1]-opacity) < 0.00005);
            assert(answer[4*i+2] <= answer[4*i+1] + 0.00001f && answer[4*i+2] >= 0);
            double sky = mode ? 0 : std::min(0.995,(1-std::exp(-0.002*Integrate(cases[i],0.02))) *
                std::exp(-std::max(0.0,double(cases[i].to-cases[i].from)/cases[i].distance)*8));
            assert(std::abs(answer[4*i+2]-sky) < 0.00005);
        }
        ctx->Unmap(staging.Get(),0);
    }
    ctx->ClearState();
    // Actual bridge pixel shader: water at 5 m must use its replacement depth, not the 1000 m city behind it.
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11VertexShader> vertex;
    Hr(dev->CreatePixelShader(kPsPreComposite,sizeof kPsPreComposite,nullptr,&pixel));
    Hr(dev->CreateVertexShader(kVsFullScreen,sizeof kVsFullScreen,nullptr,&vertex));
    auto texture = [&](DXGI_FORMAT format, const float *values, ComPtr<ID3D11Texture2D> &tex,
                       ComPtr<ID3D11ShaderResourceView> &view) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width=td.Height=td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
        td.Format=format; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{values,16,0};
        Hr(dev->CreateTexture2D(&td,&data,&tex));
        Hr(dev->CreateShaderResourceView(tex.Get(),nullptr,&view));
    };
    const float cityDepth[4]={1000,0,0,0}, cityColor[4]={0.1f,0.1f,0.1f,1};
    const float waterColor[4]={0.3f,0.3f,0.3f,1}, waterDepth[4]={5,0,0,1};
    const float empty[4]={};
    ComPtr<ID3D11Texture2D> tex[5], target, read;
    ComPtr<ID3D11ShaderResourceView> views[5];
    texture(DXGI_FORMAT_R32_FLOAT,cityDepth,tex[0],views[0]);
    texture(DXGI_FORMAT_R32G32B32A32_FLOAT,cityColor,tex[1],views[1]);
    texture(DXGI_FORMAT_R32G32B32A32_FLOAT,waterColor,tex[2],views[2]);
    texture(DXGI_FORMAT_R32G32B32A32_FLOAT,waterDepth,tex[3],views[3]);
    texture(DXGI_FORMAT_R32G32B32A32_FLOAT,empty,tex[4],views[4]);
    D3D11_TEXTURE2D_DESC td{};
    td.Width=td.Height=td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT; td.BindFlags=D3D11_BIND_RENDER_TARGET;
    Hr(dev->CreateTexture2D(&td,nullptr,&target));
    ComPtr<ID3D11RenderTargetView> rtv; Hr(dev->CreateRenderTargetView(target.Get(),nullptr,&rtv));
    td.BindFlags=0; td.Usage=D3D11_USAGE_STAGING; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Hr(dev->CreateTexture2D(&td,nullptr,&read));
    k={}; k.right[0]=k.up[1]=k.fwd[2]=k.proj[0]=k.proj[1]=1;
    k.size[0]=k.size[1]=k.size[2]=k.size[3]=1; k.depth_proj[2]=100000;
    k.atmosphere[1]=0.01f; k.atmosphere_color[0]=k.atmosphere_color[1]=k.atmosphere_color[2]=0.8f;
    k.atmosphere_info[0]=1; k.atmosphere_info[2]=0.995f;
    D3D11_VIEWPORT vp{0,0,1,1,0,1}; ctx->RSSetViewports(1,&vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vertex.Get(),nullptr,0); ctx->PSSetShader(pixel.Get(),nullptr,0);
    ctx->PSSetConstantBuffers(0,1,constants.GetAddressOf());
    ID3D11ShaderResourceView *resources[6]={views[0].Get(),views[1].Get(),nullptr,nullptr,views[2].Get(),views[3].Get()};
    for(int mode=0;mode<3;mode++) {
        resources[5]=views[mode==1?4:3].Get();
        k.atmosphere_info[0]=mode==2?0.0f:1.0f;
        ctx->UpdateSubresource(constants.Get(),0,nullptr,&k,0,0);
        ctx->PSSetShaderResources(0,6,resources);
        ctx->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
        ctx->Draw(3,0);
        ctx->OMSetRenderTargets(0,nullptr,nullptr);
        ctx->CopyResource(read.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE map{}; Hr(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&map));
        const float *result=static_cast<const float *>(map.pData);
        double opacity=mode==2?0:std::min(0.995,1-std::exp(-0.01*(mode==1?1000:5)));
        double expected=std::pow(std::pow(0.3,2.2)*(1-opacity)+std::pow(0.8,2.2)*opacity,1/2.2);
        assert(std::isfinite(result[0]) && std::abs(result[0]-expected)<0.0001 && result[3]==1);
        ctx->Unmap(read.Get(),0);
    }
    std::puts("Atmosphere shader: 45 integral cases and 3 actual water-depth/opaque-depth/disabled composites passed.");
}
