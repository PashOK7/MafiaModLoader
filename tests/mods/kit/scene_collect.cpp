// Diagnostic consumer of explicit uncullled native mesh preparation. Never installed as a shipped mod.
#include "mml/mml.h"
#include <d3d9.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
namespace {
extern "C" unsigned kit_scene_collect_c(const MmlHost *,MmlMod *,const float *,const float *,MmlSceneCollection *);
extern "C" unsigned kit_scene_disk_c(const MmlHost *,MmlMod *,const float *,const float *,MmlSceneCollection *);
extern "C" int kit_scene_owned_c(const MmlHost *,MmlMod *);
extern "C" unsigned kit_scene_billboard_c(const MmlHost *,MmlMod *,const MmlSceneDraw *);
extern "C" unsigned kit_scene_residency_c(const MmlHost *,MmlMod *);
const MmlHost *H;MmlMod *M;
bool pending,success,metadataOnly,collectFault;unsigned captures,draws,copies,unavailable,triangles,invalid,unseen,streamMissing,prepareMissing;
bool procedural,disk;
float lo[3]={-2000,-50,-500},hi[3]={-1000,300,700};
MmlSceneCollection collection{};
uint64_t worldHash,attributeHash,ordinaryFrame;double elapsed;
uint64_t billboardAttributeHash;
std::string billboardExample;
unsigned cChecks;bool stateEqual;
std::map<uint64_t,unsigned> unavailableKinds;
std::map<std::string,unsigned> unavailableLocks;
std::map<std::string,unsigned> inventory;
std::vector<MmlSceneMissing> missingExamples;
void Count(const char *name,unsigned value){inventory[std::string(name)+"="+std::to_string(value)]++;}
// Native facts checked in BOTH ProductBillMatrix/AddFGroups functions; test inventory only.
struct BillboardInfo {unsigned type,axis,mode,proxy,sourceType;};
bool ReadBillboardInfo(const void *visual,bool v10,BillboardInfo *out){
    __try{
        const auto *v=static_cast<const unsigned char *>(visual);out->type=*reinterpret_cast<const unsigned *>(v+0x1D4);
        if(out->type!=0x44524242u)return false;
        out->proxy=(*reinterpret_cast<const unsigned *>(v+0x1D8)&0x10000u)!=0;
        if(out->proxy)v=*reinterpret_cast<const unsigned char *const *>(v+8);
        out->sourceType=*reinterpret_cast<const unsigned *>(v+0x1D4);
        out->axis=*reinterpret_cast<const unsigned *>(v+0x21C);
        out->mode=v10?v[0x220]:*reinterpret_cast<const unsigned *>(v+0x220);return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
struct DeviceState {
    D3DMATRIX matrices[3];D3DVIEWPORT9 viewport;DWORD render[14],stage[2][8],sampler[2][5];void *textures[2];
    bool Read(IDirect3DDevice9 *device){
        bool ok=SUCCEEDED(device->GetTransform(D3DTS_WORLD,matrices))&&SUCCEEDED(device->GetTransform(D3DTS_VIEW,matrices+1))&&SUCCEEDED(device->GetTransform(D3DTS_PROJECTION,matrices+2))&&SUCCEEDED(device->GetViewport(&viewport));
        const D3DRENDERSTATETYPE rs[]={D3DRS_CULLMODE,D3DRS_ALPHABLENDENABLE,D3DRS_ALPHATESTENABLE,D3DRS_ALPHAREF,D3DRS_ALPHAFUNC,D3DRS_SRCBLEND,D3DRS_DESTBLEND,D3DRS_LIGHTING,D3DRS_ZENABLE,D3DRS_ZWRITEENABLE,D3DRS_FOGENABLE,D3DRS_FOGCOLOR,D3DRS_FOGSTART,D3DRS_FOGEND};
        const D3DTEXTURESTAGESTATETYPE ts[]={D3DTSS_COLOROP,D3DTSS_ALPHAOP,D3DTSS_COLORARG1,D3DTSS_COLORARG2,D3DTSS_ALPHAARG1,D3DTSS_ALPHAARG2,D3DTSS_TEXCOORDINDEX,D3DTSS_TEXTURETRANSFORMFLAGS};
        const D3DSAMPLERSTATETYPE ss[]={D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV,D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER};
        for(unsigned i=0;i<14;i++)ok=SUCCEEDED(device->GetRenderState(rs[i],render+i))&&ok;
        for(unsigned s=0;s<2;s++){
            for(unsigned i=0;i<8;i++)ok=SUCCEEDED(device->GetTextureStageState(s,ts[i],stage[s]+i))&&ok;
            for(unsigned i=0;i<5;i++)ok=SUCCEEDED(device->GetSamplerState(s,ss[i],sampler[s]+i))&&ok;
            IDirect3DBaseTexture9 *t=nullptr;ok=SUCCEEDED(device->GetTexture(s,&t))&&ok;textures[s]=t;if(t)t->Release();
        }
        return ok;
    }
};
std::unordered_set<const void *> ordinary,visuals;
std::unordered_map<const void *,std::array<float,16>> transforms;
uint64_t Hash(uint64_t h,const void *data,size_t bytes){
    for(size_t i=0;i<bytes;i++){h^=static_cast<const uint8_t *>(data)[i];h*=1099511628211ull;}return h;
}
void Collect(const MmlEvent *event,void *){
    if(!pending||!event->world)return;
    pending=false;success=false;collection={};collection.size=sizeof collection;draws=copies=unavailable=triangles=invalid=unseen=streamMissing=prepareMissing=0;
    visuals.clear();transforms.clear();unavailableKinds.clear();unavailableLocks.clear();inventory.clear();missingExamples.clear();worldHash=attributeHash=0;
    billboardAttributeHash=0;
    billboardExample.clear();
    const auto before=std::chrono::steady_clock::now();
    auto *device=static_cast<IDirect3DDevice9 *>(H->render_device(9));DeviceState original{},after{};
    const bool stateRead=device&&original.Read(device);
    if(MML_HAS(H,scene_draw_collect)){
        if(disk){cChecks=kit_scene_disk_c(H,M,lo,hi,&collection);success=cChecks==3;}
        else if(procedural&&MML_HAS(H,scene_draw_collect_ex)){
            cChecks=0;success=H->scene_draw_collect_ex(M,lo,hi,MML_SCENE_COLLECT_BILLBOARDS,&collection)!=0;
        }else if(captures%2==0){cChecks=kit_scene_collect_c(H,M,lo,hi,&collection);success=cChecks==3;}
        else success=H->scene_draw_collect(M,lo,hi,&collection)!=0;
    }
    stateEqual=stateRead&&after.Read(device)&&!memcmp(&original,&after,sizeof original);
    elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-before).count();captures++;
    if(success)for(unsigned i=0;i<collection.missing_count;i++){
        const auto &missing=collection.missing[i];
        if(missing.reason==MML_SCENE_MISSING_STREAM)streamMissing++;else prepareMissing++;
        Count("missing",missing.reason);
        if(missing.reason==MML_SCENE_MISSING_DEFORMATION){
            BillboardInfo info{};
            if(ReadBillboardInfo(missing.visual,!strcmp(H->game_build(),"1.0"),&info)){
                Count("billboard_axis",info.axis);Count("billboard_mode",info.mode);Count("billboard_proxy",info.proxy);
                Count("billboard_source_type",info.sourceType);
            }else Count("billboard_unreadable",1);
        }
        if(missingExamples.size()<32)missingExamples.push_back(missing);
        for(unsigned axis=0;axis<3;axis++)if(!std::isfinite(missing.lo[axis])||!std::isfinite(missing.hi[axis])||missing.lo[axis]>missing.hi[axis])invalid++;
    }
    collection.missing=nullptr; // no borrowed pointer retained by the consumer
}
int Command(const char *args,MmlReply *reply,void *){
    if(args&&strstr(args,"true")){
        // Test-only compact box input: six floats [lo.x,lo.y,lo.z,hi.x,hi.y,hi.z].
        if(const char *values=strchr(args,'['))sscanf(values,"[ %f , %f , %f , %f , %f , %f ]",lo,lo+1,lo+2,hi,hi+1,hi+2);
        pending=true;
        procedural=strstr(args,"billboards")!=nullptr;
        disk=strstr(args,"disk")!=nullptr;
        metadataOnly=strstr(args,"metadata")!=nullptr;collectFault=strstr(args,"fault")!=nullptr;
    }
    MmlSceneCollection outside{};outside.size=sizeof outside;
    const bool rejected=!MML_HAS(H,scene_draw_collect)||!H->scene_draw_collect(M,lo,hi,&outside);
    char row[900];snprintf(row,sizeof row,"{\"pending\":%s,\"success\":%s,\"captures\":%u,\"frame\":%llu,\"visited\":%u,\"selected\":%u,\"native_draws\":%u,\"draws\":%u,\"copies\":%u,\"unavailable\":%u,\"triangles\":%u,\"invalid\":%u,\"unique\":%zu,\"unseen\":%u,\"stream_missing\":%u,\"prepare_missing\":%u,\"world_hash\":\"%016llX\",\"ms\":%.3f,\"outside_refused\":%s}",
        pending?"true":"false",success?"true":"false",captures,static_cast<unsigned long long>(collection.frame),collection.visited,collection.selected,collection.draws,draws,copies,unavailable,triangles,invalid,visuals.size(),unseen,streamMissing,prepareMissing,static_cast<unsigned long long>(worldHash),elapsed,rejected?"true":"false");
    std::string output=row;
    output.pop_back();char attributes[140];snprintf(attributes,sizeof attributes,",\"attribute_hash\":\"%016llX\",\"billboard_attribute_hash\":\"%016llX\"}",static_cast<unsigned long long>(attributeHash),static_cast<unsigned long long>(billboardAttributeHash));output+=attributes;
    output.pop_back();output+=",\"unavailable_kinds\":{";
    bool comma=false;for(const auto &entry:unavailableKinds){char value[120];snprintf(value,sizeof value,"%s\"flags=%u fvf=%X\":%u",comma?",":"",unsigned(entry.first>>32),unsigned(entry.first),entry.second);output+=value;comma=true;}output+="}}";
    output.pop_back();output+=",\"unavailable_locks\":{";comma=false;
    for(const auto &entry:unavailableLocks){output+=(comma?",":"")+std::string("\"")+entry.first+"\":"+std::to_string(entry.second);comma=true;}output+="}}";
    output.pop_back();output+=",\"inventory\":{";comma=false;
    for(const auto &entry:inventory){output+=(comma?",":"")+std::string("\"")+entry.first+"\":"+std::to_string(entry.second);comma=true;}output+="}}";
    output.pop_back();output+=",\"missing_examples\":[";comma=false;
    for(const auto &missing:missingExamples){char value[200];snprintf(value,sizeof value,"%s{\"visual\":\"0x%08llX\",\"reason\":%u,\"lo\":[%.5f,%.5f,%.5f],\"hi\":[%.5f,%.5f,%.5f]}",comma?",":"",static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(missing.visual)),missing.reason,missing.lo[0],missing.lo[1],missing.lo[2],missing.hi[0],missing.hi[1],missing.hi[2]);output+=value;comma=true;}output+="]}";
    output.pop_back();if(!billboardExample.empty())output+=",\"billboard_example\":"+billboardExample;
    output+=",\"c_checks\":"+std::to_string(cChecks)+",\"state_equal\":"+(stateEqual?"true":"false");
    char first[80];snprintf(first,sizeof first,",\"first_visual\":\"0x%08llX\"}",transforms.empty()?0ull:static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(transforms.begin()->first)));output+=first;
    if(args)if(const char *pointer=strstr(args,"0x")){
        const auto visual=reinterpret_cast<const void *>(static_cast<uintptr_t>(strtoull(pointer,nullptr,16)));
        auto found=transforms.find(visual);output.pop_back();output+=",\"present\":"+std::string(found==transforms.end()?"false":"true");
        if(found!=transforms.end()){char position[160];snprintf(position,sizeof position,",\"position\":[%.6f,%.6f,%.6f]",found->second[12],found->second[13],found->second[14]);output+=position;}
        output+="}";
    }
    H->reply_append(reply,output.c_str(),static_cast<size_t>(-1));return 0;
}
}
void kit_scene_collect_draw(const MmlSceneDraw *draw){
    if(!(draw->flags&MML_SCENE_DRAW_COLLECTION)){
        if(ordinaryFrame!=draw->frame){ordinaryFrame=draw->frame;ordinary.clear();}
        ordinary.insert(draw->visual);return;
    }
    draws++;
    if(draw->kind==3){Count("billboard_c_checks",kit_scene_billboard_c(H,M,draw));
        MmlSceneBillboard value{};value.size=sizeof value;
        const bool valid=MML_HAS(H,scene_draw_billboard)&&H->scene_draw_billboard(M,draw,&value);
        Count("billboard_descriptor",valid?1:0);
    }
    Count("flags",draw->flags);Count("kind",draw->kind);Count("pass",draw->pass);
    Count("blend",draw->blend);Count("alpha_test",draw->alpha_test);Count("cull",draw->cull);
    if(draw->blend){Count("src_blend",draw->source_blend);Count("dst_blend",draw->destination_blend);}
    if(draw->alpha_test){Count("alpha_func",draw->alpha_function);Count("alpha_ref",draw->alpha_reference);}
    for(unsigned stage=0;stage<2;stage++){
        const auto &s=*reinterpret_cast<const MmlSceneTextureStage *>(reinterpret_cast<const uint8_t *>(draw->stages)+stage*draw->stage_stride);
        const auto prefix="stage"+std::to_string(stage)+"_";
        Count((prefix+"color_op").c_str(),s.color_op);Count((prefix+"alpha_op").c_str(),s.alpha_op);
        if(s.color_op!=D3DTOP_DISABLE){
            Count((prefix+"uv").c_str(),s.coordinate);Count((prefix+"transform").c_str(),s.transform_flags);
            Count((prefix+"color_arg0").c_str(),s.color_arg0);Count((prefix+"color_arg1").c_str(),s.color_arg1);Count((prefix+"color_arg2").c_str(),s.color_arg2);
            Count((prefix+"alpha_arg0").c_str(),s.alpha_arg0);Count((prefix+"alpha_arg1").c_str(),s.alpha_arg1);Count((prefix+"alpha_arg2").c_str(),s.alpha_arg2);
            Count((prefix+"address_u").c_str(),s.address_u);Count((prefix+"address_v").c_str(),s.address_v);
        }
        if(s.texture){
            auto *texture=static_cast<IDirect3DBaseTexture9 *>(s.texture);Count("texture_type",texture->GetType());
            if(texture->GetType()==D3DRTYPE_TEXTURE){D3DSURFACE_DESC d{};if(SUCCEEDED(static_cast<IDirect3DTexture9 *>(texture)->GetLevelDesc(0,&d)))Count("texture_format",d.Format);}
        }
    }
    if(auto *device=static_cast<IDirect3DDevice9 *>(H->render_device(9))){
        DWORD value=0;device->GetFVF(&value);Count("fvf",value);
        device->GetTextureStageState(2,D3DTSS_COLOROP,&value);Count("stage2_color_op",value);
        for(unsigned stage=0;stage<2;stage++){
            device->GetSamplerState(stage,D3DSAMP_SRGBTEXTURE,&value);Count("srgb_texture",value);
            device->GetTextureStageState(stage,D3DTSS_RESULTARG,&value);Count("result_arg",value);
        }
    }
    if(visuals.insert(draw->visual).second&&!ordinary.count(draw->visual))unseen++;
    std::array<float,16> world;std::copy_n(draw->world,16,world.begin());transforms[draw->visual]=world;
    if(collectFault&&draws==5)*static_cast<volatile int *>(nullptr)=7;
    if(metadataOnly)return;
    MmlSceneGeometry geometry{};geometry.size=sizeof geometry;
    if(!H->scene_draw_geometry(M,draw,&geometry)){
        unavailable++;DWORD fvf=0;if(auto *device=static_cast<IDirect3DDevice9 *>(H->render_device(9))){
            device->GetFVF(&fvf);IDirect3DVertexBuffer9 *vb=nullptr;IDirect3DIndexBuffer9 *ib=nullptr;UINT off=0,stride=0;
            device->GetStreamSource(0,&vb,&off,&stride);device->GetIndices(&ib);D3DVERTEXBUFFER_DESC desc{};
            void *p=nullptr;HRESULT vh=E_FAIL,ih=E_FAIL;
            if(vb){vb->GetDesc(&desc);vh=vb->Lock(0,0,&p,D3DLOCK_READONLY);if(SUCCEEDED(vh))vb->Unlock();vb->Release();}
            if(ib){ih=ib->Lock(0,0,&p,D3DLOCK_READONLY);if(SUCCEEDED(ih))ib->Unlock();ib->Release();}
            char reason[160];snprintf(reason,sizeof reason,"vb=%08X ib=%08X usage=%X",unsigned(vh),unsigned(ih),desc.Usage);unavailableLocks[reason]++;
        }
        unavailableKinds[(uint64_t(draw->flags)<<32)|fvf]++;return;
    }
    copies++;triangles+=geometry.triangles;
    if(draw->kind==3&&billboardExample.empty()&&geometry.triangles){
        MmlSceneBillboard value{};value.size=sizeof value;
        if(H->scene_draw_billboard(M,draw,&value)){
            char line[900];float p[9];
            for(unsigned c=0;c<3;c++){const auto *v=reinterpret_cast<const MmlSceneVertex *>(reinterpret_cast<const uint8_t *>(geometry.vertex)+size_t(geometry.index[c])*geometry.vertex_stride);std::copy_n(v->pos,3,p+c*3);}
            snprintf(line,sizeof line,"{\"visual\":\"0x%08llX\",\"axis\":%u,\"mode\":%u,\"world\":[%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f],\"triangle\":[%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f],\"cull\":%u}",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(draw->visual)),value.axis,value.mode,
                value.world[0],value.world[1],value.world[2],value.world[3],value.world[4],value.world[5],value.world[6],value.world[7],value.world[8],value.world[9],value.world[10],value.world[11],value.world[12],value.world[13],value.world[14],value.world[15],
                p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],draw->cull);billboardExample=line;
        }
    }
    uint64_t hash=Hash(14695981039346656037ull,draw->world,64);
    uint64_t attributes=Hash(14695981039346656037ull,draw->world,64);
    uint64_t billboardHash=14695981039346656037ull;
    if(draw->kind==3){MmlSceneBillboard value{};value.size=sizeof value;
        if(MML_HAS(H,scene_draw_billboard)&&H->scene_draw_billboard(M,draw,&value))billboardHash=Hash(billboardHash,value.world,64);
    }
    for(unsigned i=0;i<geometry.triangles*3;i++){
        if(geometry.index[i]>=geometry.vertices){invalid++;continue;}
        const auto *vertex=reinterpret_cast<const MmlSceneVertex *>(reinterpret_cast<const uint8_t *>(geometry.vertex)+size_t(geometry.index[i])*geometry.vertex_stride);
        for(float p:vertex->pos)if(!std::isfinite(p))invalid++;
        hash=Hash(hash,vertex->pos,12);
        attributes=Hash(attributes,&vertex->color,4);attributes=Hash(attributes,vertex->uv0,8);attributes=Hash(attributes,vertex->uv1,8);
        if(draw->kind==3){billboardHash=Hash(billboardHash,vertex->pos,12);billboardHash=Hash(billboardHash,&vertex->color,4);
            billboardHash=Hash(billboardHash,vertex->uv0,8);billboardHash=Hash(billboardHash,vertex->uv1,8);}
    }
    // Commutative multiset fingerprint; native traversal order is not a geometric property.
    worldHash+=hash;
    attributeHash+=attributes;
    if(draw->kind==3)billboardAttributeHash+=billboardHash;
}
void kit_scene_collect_init(const MmlHost *host,MmlMod *mod){
    H=host;M=mod;H->subscribe(M,MML_EVENT_RENDER_WORLD,Collect,nullptr);
    H->command(M,"scene_collect","one-shot native world-mesh collection: on true, optional box [lo3,hi3], disk true and visual 0x address",Command,nullptr);
    H->command(M,"scene_owned","test the C scene job API",[](const char *,MmlReply *reply,void *) -> int {
        H->reply_append(reply,kit_scene_owned_c(H,M)?"true":"false",static_cast<size_t>(-1));return 0;
    },nullptr);
    H->command(M,"residency","native source region: on true/false, optional box [lo3,hi3], or query",[](const char *args,MmlReply *reply,void *) -> int {
        if(!MML_HAS(H,scene_residency)||!MML_HAS(H,scene_residency_state))return 2;
        int accepted=-1;float bounds[6]={-2000,-30,-500,-1000,240,700};
        if(args&&strstr(args,"camera")){
            float radius=608;const char *r=strstr(args,"radius");if(r){r=strchr(r,':');if(!r||sscanf(r+1," %f",&radius)!=1)return 2;}
            if(!MML_HAS(H,scene_residency_camera))return 2;
            accepted=H->scene_residency_camera(M,radius);
        }else if(args&&strstr(args,"true")){
            if(const char *a=strchr(args,'['))if(sscanf(a,"[ %f , %f , %f , %f , %f , %f ]",bounds,bounds+1,bounds+2,bounds+3,bounds+4,bounds+5)!=6)return 2;
            accepted=H->scene_residency(M,bounds,bounds+3);
        }else if(args&&strstr(args,"false"))accepted=H->scene_residency(M,nullptr,nullptr);
        MmlSceneResidencyState state{};state.size=sizeof state;if(!H->scene_residency_state(M,&state))return 2;
        char out[500];snprintf(out,sizeof out,"{\"accepted\":%d,\"requested\":%u,\"ready\":%u,\"owners\":%u,\"blocks\":%u,\"active\":%u,\"required\":%u,\"available\":%u,\"scene\":%llu,\"c_checks\":%u}",
            accepted,state.requested,state.ready,state.owners,state.blocks,state.active,state.required,state.available,static_cast<unsigned long long>(state.scene_generation),kit_scene_residency_c(H,M));
        std::string result=out;result.pop_back();char box[220];snprintf(box,sizeof box,",\"lo\":[%.4f,%.4f,%.4f],\"hi\":[%.4f,%.4f,%.4f]}",state.lo[0],state.lo[1],state.lo[2],state.hi[0],state.hi[1],state.hi[2]);result+=box;
        H->reply_append(reply,result.c_str(),result.size());return 0;
    },nullptr);
}
void kit_scene_collect_shutdown(){pending=false;ordinary.clear();visuals.clear();transforms.clear();}
