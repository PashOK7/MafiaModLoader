// Automatic, bounded pedestrian incident capture at native D3D8 indexed draws.
// Diagnostic only: no ragdoll/animation/vertex/audio writes and no desktop input.
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "json.hpp"
#include "mml/game/draw_probe.h"
#include "mml/path.h"

using json=nlohmann::json;
namespace {
const MmlHost *H;MmlMod *M;
mml_draw_probe::Probe probe;
using Sample=mml_draw_probe::Sample;
std::unordered_map<const void *,void *> targets;
std::vector<MmlPedestrian> pedestrians;
std::deque<Sample> recent;
void *manual=nullptr;
bool hooked=false;
bool wrapped=false;
bool inWorld=false;
unsigned draws=0,matched=0,serial=0,written=0,dropped=0;
std::string lastFile,lastError;
struct Incident {unsigned serial=0;void *actor=nullptr,*model=nullptr;uint64_t start=0,duration=1600;bool saved=false;
    std::vector<Sample> before,after;};
std::array<Incident,8> incidents;
void *lastSelectedModel=nullptr;
std::string Hex(const void *p){char s[32];snprintf(s,sizeof s,"0x%p",p);return s;}
json Point(mml_draw_probe::Point p){return {p.x,p.y,p.z};}
json Row(const Sample &s,uint64_t start){
    return {{"ms",static_cast<int64_t>(s.ms)-static_cast<int64_t>(start)},
        {"frame",s.frame},{"copy_frame",s.copy_frame},{"visual",Hex(s.visual)},
        {"copied_mesh",Hex(s.copied_mesh)},{"matched",s.matched},{"indexed",s.indexed},{"vertices",s.vertices},
        {"draw_range_vertices",s.range_vertices},{"base",s.base},{"first",s.first},
        {"world",std::vector<float>(s.world,s.world+16)},
        {"center",s.matched?Point(s.center):json(nullptr)},
        {"lo",s.matched?Point(s.lo):json(nullptr)},{"hi",s.matched?Point(s.hi):json(nullptr)}};
}
json Dump(const Incident &i){
    json out={{"pid",GetCurrentProcessId()},{"serial",i.serial},{"actor",Hex(i.actor)},
        {"model",Hex(i.model)},{"impact_ms",i.start},{"kind",wrapped?"indexed_native_d3d9_draw":"cpu_skin_at_native_d3d8_draw"},
        {"scope",wrapped?"unique indexed vertices at draw":"skinned vertices within native draw min/count; not indexed pixel coverage"},
        {"before",json::array()},{"after",json::array()}};
    for(const auto &s:i.before)out["before"].push_back(Row(s,i.start));
    for(const auto &s:i.after)out["after"].push_back(Row(s,i.start));
    return out;
}
bool Select(const void *visual,void *){
    lastSelectedModel=nullptr;
    for(int n=0;visual&&n<24;n++){
        if(targets.count(visual)){lastSelectedModel=const_cast<void *>(visual);return true;}
        const void *parent=nullptr;
        if(!H->mem_read(reinterpret_cast<uintptr_t>(visual)+game::kFrameParent,&parent,sizeof parent))break;
        visual=parent;
    }
    return false;
}
void Receive(const Sample &s,void *){
    ++draws;if(s.matched)++matched;
    // Select resolves ancestors again: nested face-group drawing cannot change this sample's owner.
    if(!Select(s.visual,nullptr))return;
    void *model=lastSelectedModel;
    // Keep distinct ranges; repeated material passes of the same range add no position evidence.
    if(std::any_of(recent.rbegin(),recent.rend(),[&](const Sample &p){return p.frame==s.frame&&p.visual==s.visual&&
        p.first==s.first&&p.range_vertices==s.range_vertices;}))return;
    recent.push_back(s);while(recent.size()>512||(!recent.empty()&&s.ms-recent.front().ms>1000))recent.pop_front();
    for(auto &i:incidents)if(i.serial&&!i.saved&&i.model==model&&s.ms>=i.start&&s.ms-i.start<=i.duration){
        const auto previous=std::find_if(i.after.rbegin(),i.after.rend(),[&](const Sample &p){
            return p.visual==s.visual&&p.first==s.first&&p.range_vertices==s.range_vertices;});
        if(previous!=i.after.rend()&&s.ms-previous->ms<16)continue;
        if(i.after.size()<1024)i.after.push_back(s);else ++dropped;
    }
}
void WrappedDraw(const MmlSceneDraw *draw,void *){
    if(!inWorld||draw->view||draw->pass||!Select(draw->visual,nullptr))return;
    Sample s{};s.visual=draw->visual;s.frame=s.copy_frame=draw->frame;s.ms=GetTickCount64();
    s.base=draw->base_vertex;s.first=draw->min_vertex;s.range_vertices=draw->vertex_count;
    std::copy(draw->world,draw->world+16,s.world);
    MmlSceneGeometry geometry{};geometry.size=sizeof geometry;
    if(H->scene_draw_geometry(M,draw,&geometry)&&geometry.vertices<=20000&&geometry.triangles<=20000){
        std::vector<bool> used(geometry.vertices);
        for(unsigned i=0;i<geometry.triangles*3;i++){
            const unsigned index=geometry.index[i];if(index>=geometry.vertices||used[index])continue;used[index]=true;
            const auto *v=reinterpret_cast<const MmlSceneVertex *>(reinterpret_cast<const uint8_t *>(geometry.vertex)+size_t(index)*geometry.vertex_stride);
            const auto &p=v->pos;
            const mml_draw_probe::Point w{p[0]*s.world[0]+p[1]*s.world[4]+p[2]*s.world[8]+s.world[12],
                p[0]*s.world[1]+p[1]*s.world[5]+p[2]*s.world[9]+s.world[13],
                p[0]*s.world[2]+p[1]*s.world[6]+p[2]*s.world[10]+s.world[14]};
            if(!s.vertices)s.lo=s.hi=w;++s.vertices;
            s.center.x+=w.x;s.center.y+=w.y;s.center.z+=w.z;
            s.lo.x=std::min(s.lo.x,w.x);s.lo.y=std::min(s.lo.y,w.y);s.lo.z=std::min(s.lo.z,w.z);
            s.hi.x=std::max(s.hi.x,w.x);s.hi.y=std::max(s.hi.y,w.y);s.hi.z=std::max(s.hi.z,w.z);
        }
        if(s.vertices){s.center.x/=s.vertices;s.center.y/=s.vertices;s.center.z/=s.vertices;s.matched=s.indexed=true;}
    }
    Receive(s,nullptr);
}
void Save(Incident &i){
    if(i.saved)return;i.saved=true;
    SYSTEMTIME st{};GetLocalTime(&st);char name[128];
    snprintf(name,sizeof name,"ped-draw-%04u%02u%02u-%02u%02u%02u-%u-%u.json",
        st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond,GetCurrentProcessId(),i.serial);
    const std::string path=std::string(H->mod_data_dir(M))+name;
    wchar_t wide[1024];if(!mml_widen(path.c_str(),wide,1024)){lastError="path conversion failed";return;}
    HANDLE file=CreateFileW(wide,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){lastError="cannot create unique incident file";return;}
    const std::string text=Dump(i).dump();DWORD done=0;
    const bool ok=WriteFile(file,text.data(),static_cast<DWORD>(text.size()),&done,nullptr)&&done==text.size();
    CloseHandle(file);if(ok){lastFile=path;lastError.clear();++written;}else lastError="incident write failed";
}
void Hit(void *actor,void *model,uint64_t duration=1600){
    if(!actor||!model)return;
    auto &i=incidents[serial%incidents.size()];if(i.serial&&!i.saved)Save(i);
    i={};i.serial=++serial;i.actor=actor;i.model=model;i.start=GetTickCount64();i.duration=duration;
    for(const auto &s:recent)if(Select(s.visual,nullptr)&&lastSelectedModel==model&&i.start-s.ms<500)i.before.push_back(s);
    targets[model]=actor;
}
void Refresh(){
    targets.clear();
    const int n=H->pedestrians_list(M,nullptr,0);pedestrians.resize(std::min(512,std::max(n,0)));
    for(auto &p:pedestrians)p.size=sizeof p;
    const int got=H->pedestrians_list(M,pedestrians.data(),static_cast<int>(pedestrians.size()));
    for(int n=0;n<std::min(got,static_cast<int>(pedestrians.size()));n++)
        if(pedestrians[n].spawned&&pedestrians[n].model)targets[pedestrians[n].model]=pedestrians[n].element;
    std::array<void *,256> humans{};const int hn=H->humans_list(M,humans.data(),static_cast<int>(humans.size()));
    for(int n=0;n<std::min(hn,static_cast<int>(humans.size()));n++){
        void *model=nullptr;H->mem_read(reinterpret_cast<uintptr_t>(humans[n])+game::kActorFrame,&model,sizeof model);
        if(model)targets[model]=humans[n];
    }
    if(manual)targets[manual]=manual;
    const uint64_t now=GetTickCount64();
    for(auto &i:incidents)if(i.serial&&!i.saved){if(now-i.start>=i.duration)Save(i);else targets[i.model]=i.actor;}
}
int Reply(MmlReply *r,const json &j){const auto text=j.dump();H->reply_append(r,text.c_str(),text.size());return 0;}
int Status(const char *,MmlReply *r,void *){
    json out={{"hooked",hooked},{"wrapped",wrapped},{"faults",probe.faults},{"draws",draws},{"matched",matched},{"copies_kept",probe.copiesKept},
        {"targets",targets.size()},{"incidents",serial},{"written",written},{"dropped",dropped},
        {"last_file",lastFile},{"last_error",lastError},{"recent",json::array()}};
    for(auto it=recent.rbegin();it!=recent.rend()&&out["recent"].size()<8;++it){
        if(manual&&(!Select(it->visual,nullptr)||lastSelectedModel!=manual))continue;
        out["recent"].push_back(Row(*it,it->ms));
    }
    return Reply(r,out);
}
int Target(const char *params,MmlReply *r,void *){
    const auto j=json::parse(params?params:"{}",nullptr,false);if(j.is_discarded())return 1;
    if(j.contains("model")){const auto s=j["model"].get<std::string>();manual=reinterpret_cast<void *>(strtoul(s.c_str(),nullptr,0));}
    else manual=nullptr;
    Refresh();recent.clear();return Reply(r,{{"model",Hex(manual)}});
}
int Mark(const char *,MmlReply *r,void *){if(!manual)return Reply(r,{{"error","set a manual model first"}});
    Hit(manual,manual);return Reply(r,{{"serial",serial}});}
int IncidentRead(const char *,MmlReply *r,void *){if(!serial)return Reply(r,{{"error","no incident"}});
    return Reply(r,Dump(incidents[(serial-1)%incidents.size()]));}
void Reset(){for(auto &i:incidents)if(i.serial&&!i.saved)Save(i);
    targets.clear();recent.clear();manual=nullptr;probe.Clear();}
}
MML_EXPORT uint32_t mml_mod_api(void){return MML_API_VERSION;}
MML_EXPORT int mml_mod_load(const MmlHost *h,MmlMod *m){
    H=h;M=m;if(!MML_HAS(H,pedestrians_list)||!game::Detect(H->module_base("LS3DF.dll")))return 1;
    wrapped=H->render_device(9)!=nullptr;
    hooked=wrapped?MML_HAS(H,scene_draw_geometry)&&H->scene_draw_observe(M,WrappedDraw,nullptr):probe.Begin(H,M,Select,Receive,nullptr);
    if(!hooked)return 1;
    H->subscribe(M,MML_EVENT_FRAME,[](const MmlEvent *,void *){Refresh();},nullptr);
    H->subscribe(M,MML_EVENT_RENDER_BEGIN,[](const MmlEvent *e,void *){inWorld=e->world!=0;if(inWorld&&!wrapped)probe.Frame(e->device);},nullptr);
    H->subscribe(M,MML_EVENT_RENDER_WORLD,[](const MmlEvent *e,void *){if(e->world){inWorld=false;if(!wrapped)probe.AfterWorld();}},nullptr);
    H->subscribe(M,MML_EVENT_PEDESTRIAN_HIT,[](const MmlEvent *e,void *){
        if(!e->actor||!e->hit||e->hit->taken)return;
        void *model=nullptr;H->mem_read(reinterpret_cast<uintptr_t>(e->actor)+0x40,&model,sizeof model);Hit(e->actor,model);
    },nullptr);
    H->subscribe(M,MML_EVENT_HUMAN_HIT,[](const MmlEvent *e,void *){
        if(!e->actor||!e->hit||e->hit->taken||!e->hit->was_alive||e->hit->alive)return;
        void *model=nullptr;H->mem_read(reinterpret_cast<uintptr_t>(e->actor)+game::kActorFrame,&model,sizeof model);Hit(e->actor,model,4000);
    },nullptr);
    for(auto event:{MML_EVENT_MISSION_CLOSE,MML_EVENT_GAME_LOADED})
        H->subscribe(M,event,[](const MmlEvent *,void *){Reset();},nullptr);
    H->command(M,"status","Native D3D8 CPU-skin draw matches and immutable incident files",Status,nullptr);
    H->command(M,"target","Optional test/inspection model frame: {model: 0x...}; {} clears",Target,nullptr);
    H->command(M,"mark","Capture a manual model transition for diagnostic validation",Mark,nullptr);
    H->command(M,"incident","Most recent captured transition, before and after native draw samples",IncidentRead,nullptr);
    Refresh();return 0;
}
MML_EXPORT void mml_mod_unload(MmlMod *){Reset();if(wrapped)H->scene_draw_observe(M,nullptr,nullptr);else probe.End();}
