/* World-oriented native sky capture. General scene-view client: no water/material policy or private game data.
 * Requires scene_view_features / MML_VIEW_SKY_ONLY. Cube faces are copied coherently only after all six native
 * views were drawn for this frame. Color is the native LDR backdrop, decoded by an sRGB view; not measured HDR.
 * Explicit Invalidate() refreshes animated/changed backdrops. Reset and scene transitions invalidate automatically
 * when the caller forwards the lifecycle event. No caller-owned camera/texture pointer survives a frame. */
#pragma once
#include "mml.h"
#include <d3d11.h>
#include <cstring>

namespace mml {
namespace environment {

inline void CubeCamera(MmlViewCamera &c, unsigned face) {
    static const float forward[6][3]={{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    static const float up[6][3]={{0,1,0},{0,1,0},{0,0,-1},{0,0,1},{0,1,0},{0,1,0}};
    if (face>=6) return;
    const float *f=forward[face],*u=up[face];
    c.world[0]=u[1]*f[2]-u[2]*f[1]; c.world[1]=u[2]*f[0]-u[0]*f[2]; c.world[2]=u[0]*f[1]-u[1]*f[0];
    for(unsigned j=0;j<3;j++) { c.world[4+j]=u[j]; c.world[8+j]=f[j]; }
    c.world[3]=c.world[7]=c.world[11]=0; c.world[15]=1;
    c.tan_left=c.tan_right=c.tan_down=c.tan_up=1;
    c.flags=MML_VIEW_SKY_ONLY;
}

class SkyCapture {
public:
    SkyCapture()=default;
    SkyCapture(const SkyCapture &)=delete;
    SkyCapture &operator=(const SkyCapture &)=delete;
    ~SkyCapture() { Release(); }

    bool Create(const MmlHost *host,MmlMod *mod,unsigned resolution=512) {
        Release();
        if(!host || !MML_HAS(host,scene_view_features) ||
           !(host->scene_view_features(mod)&MML_VIEW_SKY_ONLY) || resolution<16 || resolution>4096 ||
           (resolution&(resolution-1))) return false;
        host_=host;mod_=mod;resolution_=resolution;
        for(unsigned i=0;i<6;i++) {
            faces_[i]={this,i};
            handles_[i]=host_->scene_view(mod_,static_cast<int>(resolution),static_cast<int>(resolution),Place,&faces_[i]);
            if(!handles_[i]) { Release();return false; }
        }
        pending_=true;
        return true;
    }
    void Invalidate() { pending_=true; }
    void Active(bool on) { if(on&&!active_) Invalidate();active_=on; }
    bool Pending() const { return pending_; }
    unsigned Captures() const { return captures_; }
    unsigned Resolution() const { return resolution_; }
    int Handle(unsigned face) const { return face<6 ? handles_[face] : 0; }
    ID3D11ShaderResourceView *View() const { return view_; }
    void Reset() { DropGpu();pending_=true; }
    void Release() {
        if(host_) for(int &h:handles_) { if(h) host_->scene_view_remove(mod_,h);h=0; }
        DropGpu();host_=nullptr;mod_=nullptr;pending_=true;captures_=0;
    }
    bool Update(const MmlGfxFrame &frame) {
        if(!host_ || !active_) return false;
        if(generation_!=frame.generation) { Reset();generation_=frame.generation; }
        if(!pending_) return view_!=nullptr;
        const MmlGfxView *source[6]={};
        for(unsigned i=0;i<frame.view_count;i++) for(unsigned face=0;face<6;face++)
            if(frame.views[i].handle==handles_[face] && frame.views[i].fresh) source[face]=&frame.views[i];
        for(const auto *s:source) if(!s || s->width!=resolution_ || s->height!=resolution_ || !s->color_texture) return view_!=nullptr;
        auto *device=static_cast<ID3D11Device *>(frame.device);
        auto *ctx=static_cast<ID3D11DeviceContext *>(frame.context);
        D3D11_TEXTURE2D_DESC desc{};
        static_cast<ID3D11Texture2D *>(source[0]->color_texture)->GetDesc(&desc);
        const DXGI_FORMAT sourceFormat=desc.Format;
        DXGI_FORMAT storage=DXGI_FORMAT_UNKNOWN,format=DXGI_FORMAT_UNKNOWN;
        if(sourceFormat==DXGI_FORMAT_B8G8R8A8_UNORM) { storage=DXGI_FORMAT_B8G8R8A8_TYPELESS;format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }
        if(sourceFormat==DXGI_FORMAT_R8G8B8A8_UNORM) { storage=DXGI_FORMAT_R8G8B8A8_TYPELESS;format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; }
        if(storage==DXGI_FORMAT_UNKNOWN) return false; // do not silently reinterpret a different transfer function
        for(const auto *s:source) {
            D3D11_TEXTURE2D_DESC sd{};
            static_cast<ID3D11Texture2D *>(s->color_texture)->GetDesc(&sd);
            if(sd.Format!=sourceFormat || sd.SampleDesc.Count!=1) return false;
        }
        if(!cube_ || format_!=format) {
            DropGpu();
            desc.MipLevels=0;desc.ArraySize=6;desc.Format=storage;desc.Usage=D3D11_USAGE_DEFAULT;
            desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;desc.CPUAccessFlags=0;
            desc.MiscFlags=D3D11_RESOURCE_MISC_TEXTURECUBE|D3D11_RESOURCE_MISC_GENERATE_MIPS;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format=format;vd.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBE;vd.TextureCube.MipLevels=static_cast<UINT>(-1);
            if(FAILED(device->CreateTexture2D(&desc,nullptr,&cube_)) ||
               FAILED(device->CreateShaderResourceView(cube_,&vd,&view_))) { DropGpu();return false; }
            cube_->GetDesc(&desc);mips_=desc.MipLevels;format_=format;
        }
        // All copies and mip generation precede the consuming draw on the same context. No partially filled cube
        // is published and no stale face of an animated backdrop is combined with a current face.
        for(unsigned face=0;face<6;face++) ctx->CopySubresourceRegion(cube_,D3D11CalcSubresource(0,face,mips_),0,0,0,
            static_cast<ID3D11Texture2D *>(source[face]->color_texture),0,nullptr);
        ctx->GenerateMips(view_);pending_=false;++captures_;
        return true;
    }
private:
    struct Face { SkyCapture *capture=nullptr;unsigned index=0; } faces_[6];
    static int Place(MmlViewCamera *camera,void *user) {
        auto &f=*static_cast<Face *>(user);
        if(!f.capture->active_ || !f.capture->pending_) return 0;
        CubeCamera(*camera,f.index);return 1;
    }
    void DropGpu() {
        if(view_) { view_->Release();view_=nullptr; }
        if(cube_) { cube_->Release();cube_=nullptr; }
        format_=DXGI_FORMAT_UNKNOWN;mips_=0;
    }
    const MmlHost *host_=nullptr;MmlMod *mod_=nullptr;
    int handles_[6]={};unsigned resolution_=0,generation_=0,captures_=0,mips_=0;
    bool pending_=true,active_=true;
    ID3D11Texture2D *cube_=nullptr;ID3D11ShaderResourceView *view_=nullptr;
    DXGI_FORMAT format_=DXGI_FORMAT_UNKNOWN;
};

} // namespace environment
} // namespace mml
