// Run the actual water sky compute shader on WARP with synthetic cloud/geometry samples.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include "mml/mml.h"
#include "shaders/CsPaintedSky.h"

using Microsoft::WRL::ComPtr;
static void Hr(HRESULT h) { assert(SUCCEEDED(h)); }
struct Pixel { float r, g, b, a; };
static bool Same(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    Hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                        &dev, nullptr, &ctx));
    ComPtr<ID3D11ComputeShader> shader;
    Hr(dev->CreateComputeShader(kCsPaintedSky, sizeof kCsPaintedSky, nullptr, &shader));
    MmlGfxConstants frame{};
    frame.depth_proj[2] = 100000;
    frame.sun_seen[2] = frame.sun_seen[3] = 1;
    float water[112] = {};
    water[52] = 1; // glitter strength, already-linear source
    water[56] = 5; water[57] = 1; water[58] = 0.2f; water[59] = 1;
    // Inverse perspective: near=1, far=2, tan-half-FOV=1. Pixel rays span x/z = -0.8..0.8.
    water[64] = water[69] = water[78] = water[79] = 1;
    water[75] = -0.5f;
    water[81] = std::cos(30.0f * 0.01745329252f);
    auto buffer = [&](UINT bytes, const void *data, ComPtr<ID3D11Buffer> &b) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = bytes; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA s{data}; Hr(dev->CreateBuffer(&d, &s, &b));
    };
    ComPtr<ID3D11Buffer> frameCb, waterCb;
    buffer(sizeof frame, &frame, frameCb); buffer(sizeof water, water, waterCb);
    Pixel colors[5] = {{.9f,.8f,.7f,1}, {.1f,.1f,.1f,1}, {.9f,.8f,.7f,1},
                       {.9f,.8f,.7f,1}, {0,0,0,1}};
    float depths[5] = {100000,100000,100000,2,100000};
    auto texture = [&](DXGI_FORMAT format, UINT bind, const void *data, UINT pitch, ComPtr<ID3D11Texture2D> &t) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width=5; d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1; d.Format=format; d.BindFlags=bind;
        D3D11_SUBRESOURCE_DATA s{data,pitch}; Hr(dev->CreateTexture2D(&d, data ? &s : nullptr, &t));
    };
    ComPtr<ID3D11Texture2D> color, depth, out, read;
    texture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_SHADER_RESOURCE, colors, sizeof colors, color);
    texture(DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, depths, sizeof depths, depth);
    texture(DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0, out);
    D3D11_TEXTURE2D_DESC rd{}; out->GetDesc(&rd);
    rd.BindFlags=0; rd.Usage=D3D11_USAGE_STAGING; rd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    Hr(dev->CreateTexture2D(&rd,nullptr,&read));
    ComPtr<ID3D11ShaderResourceView> colorView, depthView;
    ComPtr<ID3D11UnorderedAccessView> outView;
    Hr(dev->CreateShaderResourceView(color.Get(),nullptr,&colorView));
    Hr(dev->CreateShaderResourceView(depth.Get(),nullptr,&depthView));
    Hr(dev->CreateUnorderedAccessView(out.Get(),nullptr,&outView));
    auto run = [&](Pixel result[5]) {
        ctx->UpdateSubresource(waterCb.Get(),0,nullptr,water,0,0);
        ID3D11Buffer *cb[] = {frameCb.Get(),waterCb.Get()};
        ID3D11ShaderResourceView *srv[] = {colorView.Get(),depthView.Get()};
        ID3D11UnorderedAccessView *uav = outView.Get();
        ctx->CSSetConstantBuffers(0,2,cb); ctx->CSSetShaderResources(4,2,srv);
        ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr); ctx->CSSetShader(shader.Get(),nullptr,0);
        ctx->Dispatch(1,1,1);
        uav=nullptr; ctx->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        ctx->CopyResource(read.Get(),out.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{}; Hr(ctx->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped));
        const auto *p = static_cast<const Pixel *>(mapped.pData);
        for(int i=0;i<5;i++) result[i]=p[i];
        ctx->Unmap(read.Get(),0);
    };
    Pixel result[5]; run(result);
    assert(Same(result[0].r,colors[0].r)); // bright sky outside the sun's angular radius
    assert(Same(result[1].r,colors[1].r)); // dark cloud inside it
    assert(result[2].r > colors[2].r);    // bright opening on the actual sky
    assert(Same(result[2].r/result[2].g,colors[2].r/colors[2].g)); // hue retained
    assert(Same(result[3].r,colors[3].r)); // geometry in front of the sun is never boosted
    assert(Same(result[4].r,0));          // black sky gains no invented light
    colors[1] = colors[2];
    ctx->UpdateSubresource(color.Get(),0,nullptr,colors,sizeof colors,0);
    run(result);
    assert(result[1].r > colors[1].r && result[1].r < result[2].r); // smooth angular falloff
    water[52]=0; run(result);
    for(int i=0;i<5;i++) assert(Same(result[i].r,colors[i].r)); // off is an identity operation
    std::puts("water sky: cloud mask, angular support/falloff, hue, geometry occlusion and off: ALL PASSED");
}
