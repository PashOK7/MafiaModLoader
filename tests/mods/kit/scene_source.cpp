#include "mml/mml.h"
#include <d3d9.h>
#include "mml/native_texture.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
extern "C" unsigned kit_scene_source_c(const MmlHost *,MmlMod *,const MmlSceneDraw *);
void kit_scene_collect_init(const MmlHost *,MmlMod *);
void kit_scene_collect_draw(const MmlSceneDraw *);
void kit_scene_collect_shutdown();
namespace {
const MmlHost *H;MmlMod *M;
struct Stats{unsigned callbacks=0,main=0,views=0,backdrop=0,copies=0,c_ok=0,c_fail=0,invalid=0,uv=0,textured=0,refused_size=0,triangles=0,max_triangles=0,state_checks=0,state_errors=0,detail_checks=0;uint64_t frame=0;unsigned copied_frame=0,candidate=0;} S;
bool active=false,fail=false,selfRemove=false;
const MmlSceneDraw *saved;
std::vector<void *> textureSeen;
std::vector<mml::scene::TextureSnapshot> pendingTextures;
unsigned textureOk,textureUnavailable,textureUnsupported,textureBytes,textureGpu,textureErrors,textureMips;
int texturePass;
void TextureProbe(const MmlGfxFrame *frame,void *){
    if(frame->in_view)return;
    auto *device=static_cast<ID3D11Device *>(frame->device);auto *context=static_cast<ID3D11DeviceContext *>(frame->context);
    for(const auto &snapshot:pendingTextures){
        ID3D11Texture2D *texture=nullptr,*read=nullptr;ID3D11ShaderResourceView *view=nullptr;
        if(!snapshot.Upload(device,&texture,&view)){textureErrors++;continue;}
        D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&read))){
            context->CopyResource(read,texture);bool equal=true;
            for(unsigned level=0;level<snapshot.mips.size();level++){
                D3D11_MAPPED_SUBRESOURCE mapped{};const auto &m=snapshot.mips[level];
                if(FAILED(context->Map(read,level,D3D11_MAP_READ,0,&mapped))){equal=false;continue;}
                for(unsigned row=0;row<m.rows;row++)if(memcmp(m.bytes.data()+size_t(row)*m.pitch,static_cast<const uint8_t *>(mapped.pData)+size_t(row)*mapped.RowPitch,m.pitch))equal=false;
                context->Unmap(read,level);textureMips++;
            }
            if(equal)textureGpu++;else textureErrors++;read->Release();
        }else textureErrors++;
        view->Release();texture->Release();
    }
    pendingTextures.clear();
}
void Observe(const MmlSceneDraw *draw,void *){
    kit_scene_collect_draw(draw);
    if(draw->flags&MML_SCENE_DRAW_COLLECTION)return;
    S.callbacks++;if(draw->view)S.views++;else S.main++;
    bool textured=draw->stages&&(draw->stages[0].texture||draw->stages[1].texture);
    if(textured)S.textured++;
    if(draw->flags&MML_SCENE_DRAW_BACKDROP){S.backdrop++;return;}
    if(draw->frame!=S.frame){S.frame=draw->frame;S.copied_frame=S.candidate=0;}
    if(fail){*static_cast<volatile int *>(nullptr)=7;return;}
    if(selfRemove){H->scene_draw_observe(M,nullptr,nullptr);active=false;return;}
    if(draw->pass||S.copied_frame>=4||!textured)return;
    // Sweep across the scene rather than repeatedly testing the first four billboard quads.
    if(S.candidate++<(draw->frame%64)*4)return;
    S.copied_frame++;saved=draw;
    for(unsigned stage=0;stage<2;stage++){
        void *source=draw->stages[stage].texture;
        if(!source||textureSeen.size()>=24||std::find(textureSeen.begin(),textureSeen.end(),source)!=textureSeen.end())continue;
        textureSeen.push_back(source);mml::scene::TextureSnapshot texture;
        auto result=texture.Read(static_cast<IDirect3DBaseTexture9 *>(source));
        if(result==mml::scene::TextureResult::Valid){textureOk++;for(const auto &m:texture.mips)textureBytes+=static_cast<unsigned>(m.bytes.size());pendingTextures.push_back(std::move(texture));}
        else if(result==mml::scene::TextureResult::Unsupported)textureUnsupported++;else textureUnavailable++;
    }
    auto *texture=static_cast<IDirect3DBaseTexture9 *>(draw->stages[0].texture?draw->stages[0].texture:draw->stages[1].texture);
    MmlSceneGeometry geometry{};geometry.size=sizeof geometry;
    const bool copied=H->scene_draw_geometry(M,draw,&geometry)!=0;
    IDirect3DDevice9 *device=nullptr;
    if(SUCCEEDED(texture->GetDevice(&device))&&device){
        S.state_checks++;
        D3DMATRIX world{};if(FAILED(device->GetTransform(D3DTS_WORLD,&world))||memcmp(&world,draw->world,sizeof world))S.state_errors++;
        for(unsigned stage=0;stage<2;stage++){
            IDirect3DBaseTexture9 *actual=nullptr;
            if(FAILED(device->GetTexture(stage,&actual))||actual!=draw->stages[stage].texture)S.state_errors++;
            if(actual)actual->Release();
        }
        if(copied&&geometry.material_detail){
            S.detail_checks++;
            const auto &detail=*geometry.material_detail;
            DWORD value=0;
            if(FAILED(device->GetRenderState(D3DRS_BLENDOP,&value))||value!=detail.blend_op)S.state_errors++;
            if(FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE,&value))||value!=detail.color_write)S.state_errors++;
            if(FAILED(device->GetTextureStageState(2,D3DTSS_COLOROP,&value))||value!=detail.stage2_color_op)S.state_errors++;
            for(unsigned stage=0;stage<2;stage++){
                const auto &s=detail.stages[stage];
                if(FAILED(device->GetSamplerState(stage,D3DSAMP_SRGBTEXTURE,&value))||value!=s.srgb)S.state_errors++;
                if(FAILED(device->GetTextureStageState(stage,D3DTSS_RESULTARG,&value))||value!=s.result_arg)S.state_errors++;
                if(FAILED(device->GetSamplerState(stage,D3DSAMP_MAXANISOTROPY,&value))||value!=s.max_anisotropy)S.state_errors++;
                if(FAILED(device->GetSamplerState(stage,D3DSAMP_MAXMIPLEVEL,&value))||value!=s.max_mip_level)S.state_errors++;
                if(FAILED(device->GetSamplerState(stage,D3DSAMP_MIPMAPLODBIAS,&value))||value!=s.lod_bias_bits)S.state_errors++;
            }
        }
        device->Release();
    }else S.state_errors++;
    MmlSceneGeometry tooSmall{};
    S.refused_size+=!H->scene_draw_geometry(M,draw,&tooSmall)&&tooSmall.size==0;
    unsigned c=kit_scene_source_c(H,M,draw);if((c&3)==3)S.c_ok++;else S.c_fail++;
    if(!copied)return;
    MmlSceneGeometry legacy{};legacy.size=offsetof(MmlSceneGeometry,source_fvf)+sizeof(uint32_t);legacy.content_revision=UINT64_MAX;
    if(!H->scene_draw_geometry(M,draw,&legacy)||legacy.vertex!=geometry.vertex||legacy.index!=geometry.index||
        legacy.content_revision!=UINT64_MAX)S.invalid++;
    S.copies++;
    S.triangles+=geometry.triangles;if(geometry.triangles>S.max_triangles)S.max_triangles=geometry.triangles;
    if(geometry.vertex_stride<sizeof(MmlSceneVertex)||!geometry.vertices||!geometry.triangles)S.invalid++;
    for(unsigned v=0;v<geometry.vertices;v++){
        const auto *vertex=reinterpret_cast<const MmlSceneVertex *>(reinterpret_cast<const unsigned char *>(geometry.vertex)+v*geometry.vertex_stride);
        for(float p:vertex->pos)if(!std::isfinite(p))S.invalid++;
        for(float p:vertex->uv0)if(!std::isfinite(p))S.invalid++;
        for(float p:vertex->uv1)if(!std::isfinite(p))S.invalid++;
        if(vertex->uv0[0]!=0||vertex->uv0[1]!=0)S.uv++;
    }
    for(unsigned i=0;i<geometry.triangles*3;i++)if(geometry.index[i]>=geometry.vertices)S.invalid++;
    if(!(c&4))S.c_fail++;
}
int Command(const char *args,MmlReply *reply,void *){
    bool supported=MML_HAS(H,scene_draw_geometry);
    if(args&&strstr(args,"true")){
        if(supported){S={};saved=nullptr;textureSeen.clear();pendingTextures.clear();textureOk=textureUnavailable=textureUnsupported=textureBytes=textureGpu=textureErrors=textureMips=0;
            if(!texturePass)texturePass=H->gfx_pass(M,1000,TextureProbe,nullptr);
            fail=strstr(args,"fault")!=nullptr;selfRemove=strstr(args,"self_remove")!=nullptr;active=H->scene_draw_observe(M,Observe,nullptr)!=0;}
    }else if(args&&strstr(args,"false")){if(supported)H->scene_draw_observe(M,nullptr,nullptr);active=false;}
    MmlSceneGeometry geometry{};geometry.size=sizeof geometry;
    bool staleRefused=!supported||!H->scene_draw_geometry(M,saved,&geometry);
    char json[896];snprintf(json,sizeof json,"{\"supported\":%s,\"on\":%s,\"callbacks\":%u,\"main\":%u,\"views\":%u,\"backdrop\":%u,\"copies\":%u,\"c_ok\":%u,\"c_fail\":%u,\"invalid\":%u,\"uv\":%u,\"textured\":%u,\"refused_size\":%u,\"stale_refused\":%s,\"triangles\":%u,\"max_triangles\":%u,\"state_checks\":%u,\"state_errors\":%u}",
        supported?"true":"false",active?"true":"false",S.callbacks,S.main,S.views,S.backdrop,S.copies,S.c_ok,S.c_fail,S.invalid,S.uv,S.textured,S.refused_size,staleRefused?"true":"false",S.triangles,S.max_triangles,S.state_checks,S.state_errors);
    std::string output=json;output.pop_back();output+=",\"texture_ok\":"+std::to_string(textureOk)+",\"texture_unavailable\":"+std::to_string(textureUnavailable)+",\"texture_unsupported\":"+std::to_string(textureUnsupported)+",\"texture_bytes\":"+std::to_string(textureBytes)+",\"texture_gpu\":"+std::to_string(textureGpu)+",\"texture_errors\":"+std::to_string(textureErrors)+",\"texture_mips\":"+std::to_string(textureMips)+"}";
    output.pop_back();output+=",\"detail_checks\":"+std::to_string(S.detail_checks)+"}";
    H->reply_append(reply,output.c_str(),static_cast<size_t>(-1));return 0;
}
}
void kit_scene_source_init(const MmlHost *host,MmlMod *mod){H=host;M=mod;H->command(M,"scene_source","native buffered scene draw source: on true/false, optional fault or self_remove",Command,nullptr);kit_scene_collect_init(host,mod);}
void kit_scene_source_shutdown(){kit_scene_collect_shutdown();if(MML_HAS(H,scene_draw_observe))H->scene_draw_observe(M,nullptr,nullptr);if(texturePass)H->gfx_pass_remove(M,texturePass);texturePass=0;pendingTextures.clear();textureSeen.clear();active=false;}
