// Native diagnostic: the real sky-only cube, all face pixels and depth, without a visual approximation.
#include "mml/environment.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
extern "C" uint32_t kit_environment_features_c(const MmlHost *,MmlMod *);
namespace {
const MmlHost *H;MmlMod *M;
mml::environment::SkyCapture sky;
int pass;
bool wanted;
unsigned readCaptures,callbacks,effectCalls;
std::string evidence="null";
template<class T> void Drop(T *&p) { if(p) { p->Release();p=nullptr; } }
void Probe(const MmlGfxFrame *frame,void *) {
    if(frame->in_view) {
        for(unsigned face=0;face<6;face++) if(frame->in_view==sky.Handle(face)) ++effectCalls;
        return;
    }
    if(!wanted || !sky.Update(*frame) || readCaptures==sky.Captures()) return;
    auto *ctx=static_cast<ID3D11DeviceContext *>(frame->context);
    auto *device=static_cast<ID3D11Device *>(frame->device);
    ID3D11Resource *resource=nullptr;sky.View()->GetResource(&resource);
    ID3D11Texture2D *cube=nullptr,*read=nullptr;
    if(!resource || FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void **>(&cube)))) { Drop(resource);return; }
    D3D11_TEXTURE2D_DESC d{};cube->GetDesc(&d);
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
    if(FAILED(device->CreateTexture2D(&d,nullptr,&read))) { Drop(cube);Drop(resource);return; }
    ctx->CopyResource(read,cube);
    std::string faces="[";uint64_t hash=14695981039346656037ull;
    for(unsigned face=0;face<6;face++) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(FAILED(ctx->Map(read,D3D11CalcSubresource(0,face,d.MipLevels),D3D11_MAP_READ,0,&mapped))) break;
        for(unsigned y=0;y<d.Height;y++) for(unsigned x=0;x<d.Width*4;x++) {
            hash^=static_cast<const uint8_t *>(mapped.pData)[y*mapped.RowPitch+x];hash*=1099511628211ull;
        }
        const auto *p=static_cast<const uint8_t *>(mapped.pData)+d.Height/2*mapped.RowPitch+d.Width/2*4;
        const bool bgra=d.Format==DXGI_FORMAT_B8G8R8A8_TYPELESS;
        char row[96];std::snprintf(row,sizeof row,"%s[%u,%u,%u]",face?",":"",p[bgra?2:0],p[1],p[bgra?0:2]);faces+=row;
        ctx->Unmap(read,D3D11CalcSubresource(0,face,d.MipLevels));
    }
    faces+="]";Drop(read);Drop(cube);Drop(resource);
    float minDepth=1e30f;
    for(unsigned face=0;face<6;face++) for(unsigned i=0;i<frame->view_count;i++) {
        const auto &v=frame->views[i];if(v.handle!=sky.Handle(face)||!v.fresh) continue;
        static_cast<ID3D11ShaderResourceView *>(v.depth)->GetResource(&resource);
        auto *depth=static_cast<ID3D11Texture2D *>(resource);
        depth->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d.MiscFlags=0;
        if(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&read))) {
            ctx->CopyResource(read,depth);D3D11_MAPPED_SUBRESOURCE mapped{};
            if(SUCCEEDED(ctx->Map(read,0,D3D11_MAP_READ,0,&mapped))) {
                for(unsigned y=0;y<d.Height;y++) for(unsigned x=0;x<d.Width;x++)
                    minDepth=std::min(minDepth,reinterpret_cast<const float *>(static_cast<const uint8_t *>(mapped.pData)+y*mapped.RowPitch)[x]);
                ctx->Unmap(read,0);
            }
        }
        Drop(read);Drop(resource);
    }
    char head[180];std::snprintf(head,sizeof head,"{\"hash\":\"%016llx\",\"captures\":%u,\"min_depth\":%.3f,\"faces\":",
                              static_cast<unsigned long long>(hash),sky.Captures(),minDepth);
    evidence=std::string(head)+faces+"}";readCaptures=sky.Captures();
}
void Event(const MmlEvent *event,void *) {
    if(event->type==MML_EVENT_MISSION_LOADED || event->type==MML_EVENT_DEVICE_RESET_BEGIN) sky.Reset();
    if(event->type==MML_EVENT_VIEW_WORLD) for(unsigned face=0;face<6;face++) if(event->view==sky.Handle(face)) ++callbacks;
}
int Command(const char *args,MmlReply *reply,void *) {
    if(args&&std::strstr(args,"off")) {
        if(pass) H->gfx_pass_remove(M,pass);
        pass=0;sky.Release();wanted=false;readCaptures=callbacks=effectCalls=0;evidence="null";
    } else if(args&&std::strstr(args,"on")&&!wanted) {
        wanted=sky.Create(H,M,64);
        if(wanted) { pass=H->gfx_pass(M,1000,Probe,nullptr);H->gfx_pass_views(M,pass,1); }
    } else if(args&&std::strstr(args,"refresh")) sky.Invalidate();
    char row[220];std::snprintf(row,sizeof row,"{\"features_c\":%u,\"features_cpp\":%u,\"on\":%s,\"callbacks\":%u,\"sample\":",
        kit_environment_features_c(H,M),MML_HAS(H,scene_view_features)?H->scene_view_features(M):0,wanted?"true":"false",callbacks);
    std::string result=std::string(row)+evidence+",\"effect_calls\":"+std::to_string(effectCalls)+"}";
    H->reply_append(reply,result.c_str(),result.size());return 0;
}
}
void kit_environment_init(const MmlHost *host,MmlMod *mod) {
    H=host;M=mod;host->command(mod,"environment","native sky-only cube probe: on, refresh, off",Command,nullptr);
    host->subscribe(mod,MML_EVENT_MISSION_LOADED,Event,nullptr);host->subscribe(mod,MML_EVENT_VIEW_WORLD,Event,nullptr);
    host->subscribe(mod,MML_EVENT_DEVICE_RESET_BEGIN,Event,nullptr);
}
void kit_environment_shutdown() { sky.Release(); }
