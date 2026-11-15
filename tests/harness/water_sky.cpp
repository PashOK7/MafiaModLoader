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
#include "mml/water.h"
#include "shaders/CsPaintedSky.h"
#include "shaders/CsWaterOptics.h"

using Microsoft::WRL::ComPtr;
static void Hr(HRESULT h) { assert(SUCCEEDED(h)); }
struct Pixel { float r, g, b, a; };
static bool Same(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    namespace mw = mml::water;
    const double pi = 3.141592653589793;
    assert(mw::SmallWaveGain(2*pi/1.0,.25,0)==1); // visible metre-long waves retained
    assert(mw::SmallWaveGain(2*pi/.05,.25,0)==0); // fine ripples removed
    assert(std::fabs(mw::SmallWaveGain(2*pi/.25,.25,0)-.5)<1e-12);
    auto legacy=mw::MakeSpectrum(3,210,800);
    auto explicitLegacy=mw::MakeSpectrum(3,210,800,.0012f,4,.0043f,1);
    auto smooth=mw::MakeSpectrum(3,210,800,.0012f,4,.0043f,0);
    assert(legacy.h0==explicitLegacy.h0);
    assert(smooth.rest==0 && smooth.drawn>0 && smooth.drawn<legacy.drawn);
    assert(smooth.heightVar>legacy.heightVar*.9f && smooth.peak==legacy.peak);
    auto calm=mw::MakeSpectrum(0,210,800,.0012f,4,.0043f,0);
    assert(calm.drawn==0 && calm.rest==0 && calm.heightVar==0);
    auto physical=mw::MakeSpectrum(3,210,800,.0012f,4,.0043f,1,.25f,false);
    assert(physical.peak==legacy.peak && physical.heightVar>legacy.heightVar*8);
    assert(std::fabs(std::sqrt(physical.heightVar)-.012504f)<1e-5f);
    auto gravityWide=mw::MakeSpectrum(3,210,800,.0012f,4,.0043f,1,.25f,false,{4,1});
    assert(Same(gravityWide.peak,4*physical.peak) && gravityWide.rest==physical.rest);
    assert(gravityWide.patch[2]==physical.patch[2] && Same(gravityWide.patch[1],2*physical.patch[1]));
    // Crossfading the two spectra retains the gravity scale approximately; the transition contributes energy.
    // Compare RMS heights (not variances) within 20%, while preserving the entire fine band exactly.
    assert(gravityWide.heightVar>physical.heightVar*.64f && gravityWide.heightVar<physical.heightVar*1.44f);
    assert(gravityWide.drawn<physical.drawn && gravityWide.drawn>physical.drawn/16);
    auto noGravity=mw::MakeSpectrum(3,210,800,.0012f,4,.0043f,1,.25f,false,{4,0});
    assert(noGravity.heightVar>0 && noGravity.rest==physical.rest); // fine wind ripples survive the gravity control
    auto tallGravity=mw::MakeSpectrum(3,210,800,.0012f,4,.0043f,1,.25f,false,{4,6});
    for(int y=0;y<mw::kN;y++) for(int x=0;x<mw::kN;x++) {
        double k=std::hypot(x-mw::kN/2,y-mw::kN/2)*2*pi/mw::kPatch[2];
        if(k<2*pi/.125) continue;
        size_t at=(size_t(2)*mw::kN*mw::kN+y*mw::kN+x)*4;
        for(int j=0;j<4;j++) assert(tallGravity.h0[at+j]==physical.h0[at+j]);
    }
    auto wide = legacy;
    mw::ScaleWavelength(wide, 4);
    assert(wide.h0==legacy.h0 && wide.heightVar==legacy.heightVar);
    assert(Same(wide.peak,4*legacy.peak) && Same(wide.drawn,legacy.drawn/16) && Same(wide.rest,legacy.rest/16));
    for (int c=0;c<mw::kCascades;c++) assert(Same(wide.patch[c],4*legacy.patch[c]));
    mw::WaveClock clock;
    assert(clock.Advance(10,1)==0 && clock.Advance(12,1)==2);
    assert(clock.Advance(12,4)==2 && clock.Advance(13,4)==6);
    assert(clock.Advance(15,0)==6 && clock.Advance(16,1)==7);
    std::printf("water spectrum: long-wave height %.6f -> %.6f, slopes %.6f -> %.6f, residual %.6f -> %.6f: ALL PASSED\n",
        legacy.heightVar,smooth.heightVar,legacy.drawn,smooth.drawn,legacy.rest,smooth.rest);
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
    Pixel colors[8] = {{.9f,.8f,.7f,1}, {.1f,.1f,.1f,1}, {.9f,.8f,.7f,1},
                       {.9f,.8f,.7f,1}, {0,0,0,1}};
    float depths[8] = {100000,100000,100000,2,100000};
    auto texture = [&](DXGI_FORMAT format, UINT bind, const void *data, UINT pitch, ComPtr<ID3D11Texture2D> &t) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width=8; d.Height=d.MipLevels=d.ArraySize=d.SampleDesc.Count=1; d.Format=format; d.BindFlags=bind;
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
    auto run = [&](Pixel result[8]) {
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
        for(int i=0;i<8;i++) result[i]=p[i];
        ctx->Unmap(read.Get(),0);
    };
    Pixel result[8]; run(result);
    assert(result[0].r>colors[0].r*3); // bright cloud outside the sun's cone retains incident HDR radiance
    assert(std::fabs(result[1].r-(.1f/std::pow(.9f,2.25f)))<1e-5f); // dark cloud stays dark, no invented solar disc
    assert(result[2].r > colors[2].r);    // bright opening on the actual sky
    assert(Same(result[2].r/result[2].g,colors[2].r/colors[2].g)); // hue retained
    assert(Same(result[3].r,colors[3].r/(1-.5f*colors[3].r)) && Same(result[3].a,1)); // geometry gets no solar disc
    assert(Same(result[4].r,0));          // black sky gains no invented light
    colors[1] = colors[2];
    ctx->UpdateSubresource(color.Get(),0,nullptr,colors,sizeof colors,0);
    run(result);
    assert(result[1].r > colors[1].r && result[1].r < result[2].r); // smooth angular falloff
    water[52]=0; run(result);
    assert(Same(result[2].r,colors[2].r/std::pow(1-colors[2].b,2.25f))); // neutral cloud brightness
    assert(Same(result[3].r,colors[3].r/(1-.5f*colors[3].r)) && Same(result[4].r,0)); // general scene radiance, no invented light in black
    std::puts("water sky: cloud mask, angular support/falloff, hue, geometry occlusion and off: ALL PASSED");
    colors[0]={.1f,.3f,.9f,1};
    ctx->UpdateSubresource(color.Get(),0,nullptr,colors,sizeof colors,0);
    run(result);
    assert(std::fabs(result[0].b-(.9f/std::pow(.9f,2.25f)))<1e-5f); // saturated sky is not mistaken for white clouds
    for(float &z:depths) z=3;
    ctx->UpdateSubresource(depth.Get(),0,nullptr,depths,sizeof depths,0);
    Hr(dev->CreateComputeShader(kCsWaterOptics,sizeof kCsWaterOptics,nullptr,&shader));
    frame.atmosphere[0]=200; frame.atmosphere[1]=.002f;
    frame.atmosphere_info[0]=frame.atmosphere_info[2]=1;
    ctx->UpdateSubresource(frameCb.Get(),0,nullptr,&frame,0,0);
    run(result);
    assert(std::fabs(result[0].r-.020373f)<1e-5f); // exact air/water normal-incidence reflectance
    assert(std::fabs(result[0].g-.05969f)<.001f && Same(result[0].b,1));
    assert(Same(result[1].r,1) && result[1].g<1 && result[1].b<1 && Same(result[1].a,0)); // TIR only from within
    assert(Same(result[2].r,1) && Same(result[2].g,0) && Same(result[2].b,.04f) && Same(result[2].a,.09f));
    assert(std::fabs(result[3].r-result[3].g)>.05f && result[3].b>=0 && result[3].b<=1); // radiance/Fresnel covariance
    assert(std::fabs(result[3].a-.04f*.04f)<1e-6f); // full 2D fourth cross moment retained by seven nodes
    const float norm=std::sqrt(1.05f);
    assert(Same(result[4].r,-.2f/norm) && Same(result[4].g,1/norm) && Same(result[4].b,.1f/norm) && Same(result[4].a,1));
    assert(std::fabs(result[5].r-2*std::sqrt(1.09f))<.05f && Same(result[5].g,0) && std::fabs(result[5].b-.4f)<.01f);
    assert(Same(result[6].r,0) && result[6].g>.2f); // no foam at rest, visible patches on compressed crests
    assert(std::fabs(result[7].r-std::exp(-.3f))<1e-5f && std::fabs(result[7].g-std::exp(-.6f))<1e-5f && Same(result[7].b,1));
    std::puts("water optics: exact Fresnel, TIR, projected visibility, facet moments, radiance covariance and zero variance: ALL PASSED");
    // A close hull is in front of the water-ray origin in the reflected camera, not on its forward ray.
    // The former contact repair accepted any negative depth difference and clamped its hit distance to zero.
    for(float &z:depths) z=.5f;
    ctx->UpdateSubresource(depth.Get(),0,nullptr,depths,sizeof depths,0);
    run(result);
    std::printf("projected hull before ray origin: t=%.6f sky=%.0f contact=%.0f\n",result[5].r,result[5].g,result[5].a);
    assert(Same(result[5].g,1) && Same(result[5].a,0));
    std::puts("water projected query: geometry before the ray origin is not a zero-distance hull hit: ALL PASSED");
}
