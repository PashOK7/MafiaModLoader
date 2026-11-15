// Soundscape 0.3: continuous direct fronts, connected room faces and distinct reflected pulses.
// Geometry paths are a separate model; EAX does not expose its internal pressure field.
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "native_audio.h"
#include "acoustics.h"
#include "surfaces.h"
#include "pcm.h"
#include "mml/gfx/legacy_mesh.h"

namespace {
const MmlHost *H;
MmlMod *M;
using V=V3;
using Vertex=mml_legacy::Vertex;
using Mesh=std::vector<Vertex>;
constexpr uint32_t Cyan=0x64EAFB,Amber=0xFFB779,Violet=0xAC9DFF;
bool g_known=false,g_show=true;
float g_time=0,g_sampleAt=-10,g_roomAt=-10,g_speed=14,g_intensity=1,g_range=55;
V g_eye{},g_forward{};
audio_view::Snapshot g_audio;
pcm::Cache g_pcm;
struct Level { float rms=0,peak=0,emission=0,listener=0,backend=0; bool measured=false; };
std::unordered_map<uintptr_t,Level> g_levels;
struct Track { float pulse=-10; uint32_t stamp=0; float lastLevel=0; };
std::unordered_map<uintptr_t,Track> g_tracks;
constexpr int Rows=6,Cols=16,RayCount=(Rows+1)*Cols;
struct Cloud { V origin{}; float reach=0; std::array<acoustic::Path,RayCount> rays{};
    std::array<bool,RayCount> ready{}; unsigned done=0; };
struct Wave {
    std::shared_ptr<Cloud> cloud;
    uintptr_t source=0;
    float birth=0,amplitude=0,reach=0;
};
std::vector<Wave> g_waves;
struct WetPulse {
    float birth, amplitude;
    audio_view::Eax eax;
};
std::vector<WetPulse> g_wet;
acoustic::Room g_room,g_pendingRoom;
bool g_roomPending=false;
uintptr_t g_roomSector=0;
Mesh g_roomFill,g_roomWire,g_frontFill,g_echoFill,g_paths;
struct Surface { V a,b,c,normal; };
std::vector<Surface> g_surfaces;
std::vector<std::array<V,2>> g_boundaries;
unsigned g_seedTriangles=0,g_directTriangles=0,g_echoPulses=0;
std::array<float,48> g_tailHistory{};
unsigned g_tailCursor=0;
float g_tailSample=-1;
unsigned g_shapesFound=0;
unsigned g_emitted=0,g_rays=0,g_reflected=0,g_metered=0,g_voiced=0;
mml_legacy::Result g_draw{};
float g_early=0,g_late=0,g_peak=0;
unsigned g_drawFrames=0;
float Length(V x){return Len(x);}
V Vec(const float *p){return {p[0],p[1],p[2]};}
float Option(const char *k,float fallback,float lo,float hi){
    const float x=static_cast<float>(H->config_number(M,k,fallback));return std::isfinite(x)?std::clamp(x,lo,hi):fallback;
}
uint32_t Color(uint32_t rgb,float alpha){
    return (static_cast<uint32_t>(std::clamp(alpha*g_intensity,0.0f,1.0f)*255)<<24)|rgb;
}
Vertex Point(V p,uint32_t color){return {p.x,p.y,p.z,color};}
void Line(Mesh &v,V a,V b,uint32_t color){if(v.size()<60000){v.push_back(Point(a,color));v.push_back(Point(b,color));}}
void Triangle(Mesh &v,V a,V b,V c,uint32_t color){
    if(v.size()<120000){v.push_back(Point(a,color));v.push_back(Point(b,color));v.push_back(Point(c,color));}
}
void Triangle(Mesh &mesh, const Vertex &a, const Vertex &b, const Vertex &c) {
    if (mesh.size() >= 120000 || ((a.color | b.color | c.color) >> 24) == 0) return;
    mesh.insert(mesh.end(), {a, b, c});
}
acoustic::Hit Ray(V from,V to){
    ++g_rays;MmlHit h{};
    if(H->world_ray(M,&from.x,&to.x,MML_WORLD_STATIC,nullptr,&h)!=1)return {};
    return {Vec(h.pos),Vec(h.normal),true,h.prim};
}
void Reset(){
    g_audio={};g_levels.clear();g_tracks.clear();g_waves.clear();g_wet.clear();g_pcm.Clear();
    g_time=0;g_sampleAt=g_roomAt=-10;g_room={};g_pendingRoom={};g_roomPending=false;
    g_roomFill.clear();g_roomWire.clear();g_frontFill.clear();g_echoFill.clear();g_paths.clear();
    g_surfaces.clear();g_boundaries.clear();
    g_seedTriangles=g_directTriangles=g_echoPulses=0;
    g_tailHistory={};g_tailCursor=0;g_tailSample=-1;
    g_early=g_late=g_peak=0;g_draw={};
}
void Emit(const audio_view::Source &s,const Level &level){
    if(level.emission<.00015f)return;
    if (g_audio.eaxActive) {
        if (g_wet.size() >= 96) g_wet.erase(g_wet.begin());
        g_wet.push_back({g_time, level.emission, g_audio.eax});
    }
    const float reach=std::min({g_range,std::max(0.0f,s.farRange),std::sqrt(level.emission)*85});
    if(reach<.15f)return;
    // Keep continuous sources legible at high gain: at most three fronts per source.
    unsigned own=0;for(const auto &w:g_waves)if(w.source==s.frame)++own;
    if(own>=3){const auto old=std::find_if(g_waves.begin(),g_waves.end(),[&](const Wave &w){return w.source==s.frame;});
        g_waves.erase(old);}
    if(g_waves.size()>=16)g_waves.erase(g_waves.begin());
    auto cloud=std::make_shared<Cloud>();cloud->origin=Vec(s.pos);cloud->reach=reach;
    g_waves.push_back({cloud,s.frame,g_time,level.emission,reach});++g_emitted;
}
void Sample(bool emit){
    g_audio=audio_view::Capture(H,M);g_levels.clear();g_voiced=g_metered=0;g_peak=0;
    std::vector<const audio_view::Source *> candidates;
    for(const auto &s:g_audio.sources){
        if(s.voiced)++g_voiced;
        if(s.voiced&&(s.type==1||s.type==2)&&Length(Vec(s.pos)-g_eye)<g_range)candidates.push_back(&s);
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto *a,const auto *b){
        return Length(Vec(a->pos)-g_eye)<Length(Vec(b->pos)-g_eye);
    });
    bool loaded=false;unsigned used=0;
    for(const auto *p:candidates){
        if(++used>10)break;const auto &s=*p;
        const auto backend=audio_view::Meter(s.buffer);
        const unsigned previousMisses=g_pcm.misses;
        const pcm::Clip *clip=g_pcm.Get(H,M,s.name,backend.cursor,s.loop,!loaded);
        loaded=loaded||g_pcm.misses>previousMisses;
        Level level;level.backend=backend.gain;
        if(clip&&clip->valid&&backend.valid){
            level.rms=clip->rms;level.peak=clip->peak;level.measured=true;++g_metered;
            level.emission=clip->rms*std::max(0.0f,s.volume)*std::clamp(g_audio.master,0.0f,1.0f);
            level.listener=clip->rms*backend.gain;g_peak=std::max(g_peak,level.listener);
        }
        g_levels[s.frame]=level;
        auto &track=g_tracks[s.frame];
        const bool onset=track.stamp!=s.started;
        const bool energyOnset=level.emission>track.lastLevel*1.65f&&g_time-track.pulse>.12f;
        if(emit&&level.measured&&(onset||energyOnset||g_time-track.pulse>.38f)){
            Emit(s,level);track.pulse=g_time;
        }
        track.stamp=s.started;track.lastLevel=level.emission;
    }
}
void ReadSurfaces(){
    g_surfaces.clear();g_boundaries.clear();g_seedTriangles=0;
    std::unordered_set<const void *> hit;
    for(const auto &p:g_room.points)if(p.hit&&p.prim)hit.insert(p.prim);
    const V lo=g_room.lo-V{.15f,.15f,.15f},hi=g_room.hi+V{.15f,.15f,.15f};
    std::vector<MmlShape> shapes(4096);
    const int n=H->world_shapes(M,&lo.x,&hi.x,MML_WORLD_STATIC,shapes.data(),static_cast<int>(shapes.size()));
    g_shapesFound=static_cast<unsigned>(std::max(0,n));
    std::vector<acoustic::Facet> facets;
    for(int i=0;i<std::min(n,static_cast<int>(shapes.size()));i++){
        const auto &s=shapes[i];
        if(s.kind!=MML_SHAPE_TRIANGLE)continue;
        const V a=Vec(s.corner[0]), b=Vec(s.corner[1]), c=Vec(s.corner[2]);
        const V normal=Norm(Cross(b-a,c-a));
        const bool seed=hit.count(s.prim)!=0;
        facets.push_back({a,b,c,normal,seed});
        if(seed)++g_seedTriangles;
    }
    for(size_t i:acoustic::ConnectedSurfaces(facets)){
        const auto &face=facets[i];
        const auto polygon=acoustic::ClipTriangle(face.a,face.b,face.c,lo,hi);
        const V normal=Dot(g_room.center-face.a,face.normal)>=0?face.normal:-face.normal;
        for(size_t k=2;k<polygon.size();k++)
            if(Len(Cross(polygon[k-1]-polygon[0],polygon[k]-polygon[0]))>1e-5f)
                g_surfaces.push_back({polygon[0],polygon[k-1],polygon[k],normal});
    }
    struct RoomEdge { V a,b,normal; unsigned count=0; bool crease=false; };
    std::unordered_map<acoustic::EdgeKey,RoomEdge,acoustic::EdgeHash> edges;
    for(const auto &face:g_surfaces) {
        for(const auto &side:{std::array{face.a,face.b},std::array{face.b,face.c},std::array{face.c,face.a}}) {
            auto &edge=edges[acoustic::EdgeOf(side[0],side[1])];
            if(edge.count==0)edge={side[0],side[1],face.normal};
            else if(std::abs(Dot(edge.normal,face.normal))<.995f)edge.crease=true;
            ++edge.count;
        }
    }
    for(const auto &[key,edge]:edges) {
        if(edge.count!=1&&!edge.crease)continue;
        g_boundaries.push_back({edge.a,edge.b});
    }
}
void Probe(){
    if(!g_roomPending&&(g_time-g_roomAt>1.2f||Length(g_eye-g_room.center)>2.0f||g_roomSector!=g_audio.sector)){
        g_pendingRoom.Begin(g_eye);g_roomPending=true;g_roomSector=g_audio.sector;
    }
    for(int k=0;g_roomPending&&k<18;k++){
        const unsigned i=g_pendingRoom.done;
        const V to=g_pendingRoom.center+acoustic::Direction(i/acoustic::Room::Columns,i%acoustic::Room::Columns,
            acoustic::Room::Rows,acoustic::Room::Columns)*30;
        const auto hit=Ray(g_pendingRoom.center,to);g_pendingRoom.Add(i,hit,to);
        if(g_pendingRoom.Ready()){g_room=g_pendingRoom;g_roomPending=false;g_roomAt=g_time;ReadSurfaces();}
    }
    for(auto &wave:g_waves){
        auto &c=*wave.cloud;
        for(int k=0;k<24&&c.done<RayCount&&g_rays<95;k++){
            const unsigned i=(c.done++*29)%RayCount;
            c.rays[i]=acoustic::Trace(c.origin,acoustic::Direction(i/Cols,i%Cols,Rows,Cols),c.reach,Ray);
            c.ready[i]=true;
        }
    }
}
void RoomGeometry(){
    g_roomFill.clear();
    g_roomWire.clear();
    if(!g_room.Ready())return;
    for(const auto &face:g_surfaces){
        const V offset=face.normal*.025f;
        const float facing=std::abs(Dot(face.normal, Norm({.7f,.9f,.4f})));
        Triangle(g_roomFill,face.a+offset,face.b+offset,face.c+offset,Color(Violet,.13f+.20f*facing));
    }
    for(const auto &edge:g_boundaries)
        Line(g_roomWire,edge[0],edge[1],Color(Violet,.43f));

    // Some collision types have no triangle representation in the shape inventory.
    // Connect locally compatible samples only in those parts of the scan.
    if(!g_surfaces.empty())return;
    for(int r=0;r<acoustic::Room::Rows;r++)for(int c=0;c<acoustic::Room::Columns;c++){
        const unsigned a=r*acoustic::Room::Columns+c,b=r*acoustic::Room::Columns+(c+1)%acoustic::Room::Columns;
        const unsigned d=a+acoustic::Room::Columns,e=b+acoustic::Room::Columns;
        for(auto ids:{std::array<unsigned,3>{a,b,d},std::array<unsigned,3>{b,e,d}}){
            const auto &x=g_room.points[ids[0]],&y=g_room.points[ids[1]],&z=g_room.points[ids[2]];
            if(!x.hit||!y.hit||!z.hit)continue;
            if(std::max({Len(x.at-y.at),Len(y.at-z.at),Len(z.at-x.at)})>4)continue;
            if(Dot(x.normal,y.normal)<.97f||Dot(x.normal,z.normal)<.97f)continue;
            if(std::abs(Dot(y.at-x.at,x.normal))>.08f||std::abs(Dot(z.at-x.at,x.normal))>.08f)continue;
            Triangle(g_roomFill,x.at,y.at,z.at,Color(Violet,.16f));
        }
    }
}
void DirectFront(const Wave &wave, float distance, float brightness){
    // The surface stays spherical. Geometry fades its vertices near the first hit and
    // the game's depth buffer clips it against visible surfaces.
    constexpr int Lat=18, Lon=36;
    static const auto directions=[]{
        std::array<V,(Lat+1)*Lon> values{};
        for(int row=0;row<=Lat;row++)for(int col=0;col<Lon;col++)
            values[row*Lon+col]=acoustic::Direction(row,col,Lat,Lon);
        return values;
    }();
    std::array<Vertex,(Lat+1)*Lon> vertices;
    for(int row=0;row<=Lat;row++)for(int col=0;col<Lon;col++){
        const V dir=directions[row*Lon+col];
        const V pos=wave.cloud->origin+dir*distance;
        const int r0=row*Rows/Lat,r1=std::min(Rows,r0+1);
        const int c0=col*Cols/Lon,c1=(c0+1)%Cols;
        const float v=float(row*Rows%Lat)/Lat,u=float(col*Cols%Lon)/Lon;
        auto visibility=[&](int r,int c){
            const int i=r*Cols+c;
            const auto &p=wave.cloud->rays[i];
            return wave.cloud->ready[i]&&p.hit?acoustic::DirectVisibility(distance,p.firstHit):1.0f;
        };
        const float wall=(1-v)*std::lerp(visibility(r0,c0),visibility(r0,c1),u)+
                         v*std::lerp(visibility(r1,c0),visibility(r1,c1),u);
        const float facing=std::abs(Dot(dir,Norm(g_eye-pos)));
        const float rim=(1-facing)*(1-facing);
        vertices[row*Lon+col]=Point(pos,Color(Cyan,brightness*wall*(.035f+rim*.53f)));
    }
    for(int row=0;row<Lat;row++)for(int col=0;col<Lon;col++){
        const int a=row*Lon+col,b=row*Lon+(col+1)%Lon;
        if(row)Triangle(g_frontFill,vertices[a],vertices[b],vertices[a+Lon]);
        if(row+1<Lat)Triangle(g_frontFill,vertices[b],vertices[b+Lon],vertices[a+Lon]);
    }
}
void Pulse(V center,V direction,float radius,float alpha){
    const V u=Norm(Cross(direction,std::abs(direction.y)>.9f?V{1,0,0}:V{0,1,0}));
    const V v=Cross(direction,u);
    constexpr int segments=16;
    for(int i=0;i<segments;i++){
        auto radial=[&](int j,float r){
            const float a=2*acoustic::Pi*j/segments;
            return center+(u*std::cos(a)+v*std::sin(a))*r;
        };
        const Vertex a=Point(radial(i,radius),Color(Amber,0));
        const Vertex b=Point(radial(i+1,radius),Color(Amber,0));
        const Vertex c=Point(radial(i,radius*.63f),Color(Amber,alpha));
        const Vertex d=Point(radial(i+1,radius*.63f),Color(Amber,alpha));
        const Vertex e=Point(radial(i,radius*.31f),Color(Amber,0));
        const Vertex f=Point(radial(i+1,radius*.31f),Color(Amber,0));
        Triangle(g_echoFill,a,b,c);Triangle(g_echoFill,b,d,c);
        Triangle(g_echoFill,c,d,e);Triangle(g_echoFill,d,f,e);
    }
}
void Fronts(bool reflections,bool reverb){
    g_frontFill.clear();g_echoFill.clear();g_paths.clear();
    g_early=g_late=0;g_reflected=g_echoPulses=g_directTriangles=0;
    if(reverb)for(const auto &pulse:g_wet){
        const float age=g_time-pulse.birth;
        const auto &eax=pulse.eax;
        g_late+=acoustic::Tail(pulse.amplitude,age,eax.reflectionsDelay+eax.reverbDelay,
            eax.decay,static_cast<float>(eax.room),static_cast<float>(eax.reverb),eax.echoTime,eax.echoDepth);
        if(age>=eax.reflectionsDelay)
            g_early+=pulse.amplitude*acoustic::Gain(static_cast<float>(eax.room+eax.reflections))*
                std::exp(-6.907755f*(age-eax.reflectionsDelay)/std::max(.04f,eax.decay*.15f));
    }
    for(const auto &wave:g_waves){
        const float distance=(g_time-wave.birth)*g_speed;
        if(distance<.035f||distance>=wave.reach)continue;
        const float brightness=acoustic::Light(wave.amplitude/(1+distance*.18f))*
            acoustic::Smooth((wave.reach-distance)/std::max(.3f,wave.reach*.22f));
        DirectFront(wave,distance,brightness);
        if(!reflections)continue;
        std::array<V,8> bouncePoints{};
        unsigned count=0;
        for(int n=0;n<RayCount;n++){
            const int i=(n*29)%RayCount;
            if(!wave.cloud->ready[i])continue;
            const auto &path=wave.cloud->rays[i];
            const auto at=acoustic::At(path,distance);
            if(!at.bounce||at.ended)continue;
            ++g_reflected;
            const V hit=path.point[at.bounce];
            if(count==bouncePoints.size()||std::any_of(bouncePoints.begin(),bouncePoints.begin()+count,
                [&](V point){return Len(point-hit)<.75f;}))continue;
            bouncePoints[count++]=hit;
            const float travelled=Len(at.at-hit);
            const float strength=brightness*(at.bounce==1?.68f:.36f);
            const float radius=std::min(.87f,.22f+travelled*.32f);
            Pulse(at.at,at.direction,radius,strength*acoustic::Smooth(travelled/.18f));
            Line(g_paths,hit,at.at,Color(Amber,strength*.32f));
            ++g_echoPulses;
        }
    }
    g_directTriangles=static_cast<unsigned>(g_frontFill.size()/3);
}
Mesh Mini(const Mesh &in,V center,float scale){
    Mesh out;out.reserve(in.size());
    for(auto v:in){const V p=(V{v.x,v.y,v.z}-center)*scale;out.push_back(Point(p,v.color));}return out;
}
void LookAt(V eye,V at,float *m){
    const V z=Norm(at-eye),x=Norm(Cross({0,1,0},z)),y=Cross(z,x);
    std::fill(m,m+16,0.0f);m[0]=x.x;m[4]=x.y;m[8]=x.z;m[12]=-Dot(eye,x);
    m[1]=y.x;m[5]=y.y;m[9]=y.z;m[13]=-Dot(eye,y);
    m[2]=z.x;m[6]=z.y;m[10]=z.z;m[14]=-Dot(eye,z);m[15]=1;
}
void World(const MmlEvent *ev,void *){
    if(!g_show||!MML_EVENT_HAS(ev,world)||!ev->world)return;
    if(H->mission_state()!=MML_MISSION_RUNNING&&H->mission_state()!=MML_MISSION_PAUSED)return;
    const bool waves=H->config_number(M,"waves",1)!=0;
    Fronts(waves&&H->config_number(M,"reflections",1)!=0,H->config_number(M,"reverb",1)!=0);
    RoomGeometry();
    std::array<mml_legacy::Batch,10> batches{};unsigned count=0;
    const bool room=H->config_number(M,"room",1)!=0;
    auto add=[&](const Mesh &v,D3DPRIMITIVETYPE kind,uint32_t flags){
        if(v.empty())return;batches[count++]={v.data(),static_cast<unsigned>(v.size()),kind,flags};
    };
    const uint32_t roomFlags=H->config_number(M,"xray",0)!=0?mml_legacy::OnTop:0;
    if(room)add(g_roomWire,D3DPT_LINELIST,roomFlags);
    if(waves){add(g_frontFill,D3DPT_TRIANGLELIST,0);add(g_echoFill,D3DPT_TRIANGLELIST,mml_legacy::Additive);
        add(g_paths,D3DPT_LINELIST,mml_legacy::Additive);}
    Mesh miniFill,miniWire,miniEcho,back;
    float view[16]{},projection[16]{},identity[16]{};
    D3DVIEWPORT9 viewport{};
    if(g_room.Ready()&&H->config_number(M,"hologram",1)!=0){
        MmlRenderCamera cam{sizeof cam};H->render_camera(&cam);
        const V center=(g_room.lo+g_room.hi)*.5f;
        const V span=g_room.hi-g_room.lo;
        const float scale=2.55f/std::max({span.x,span.y,span.z,1.0f});
        const V insetEye{2.6f,1.75f,2.55f};
        if(room){
            if(g_surfaces.empty())miniFill=Mini(g_roomFill,center,scale);
            for(const auto &face:g_surfaces){
                const V a=(face.a-center)*scale,b=(face.b-center)*scale,c=(face.c-center)*scale;
                if(Dot(face.normal,insetEye-a)<=0)continue;
                const float light=.3f+.7f*std::abs(Dot(face.normal,Norm({.7f,.9f,.4f})));
                Triangle(miniFill,a,b,c,Color(Violet,.33f*light));
            }
            miniWire=Mini(g_roomWire,center,scale);
        }
        if(waves){
            // The inset has an independent viewpoint. Its reflection paths remain visible
            // even when the game camera is looking away or a wall occludes them.
            miniEcho=Mini(g_echoFill,center,scale);
            const auto paths=Mini(g_paths,center,scale);
            miniWire.insert(miniWire.end(),paths.begin(),paths.end());
        }
        viewport={static_cast<DWORD>(cam.width*.70f),static_cast<DWORD>(cam.height*.08f),
            static_cast<DWORD>(cam.width*.28f),static_cast<DWORD>(cam.height*.32f),0,1};
        identity[0]=identity[5]=identity[10]=identity[15]=1;
        const uint32_t dark=0xD50B1421;
        Triangle(back,{-1,-1,.5f},{1,-1,.5f},{-1,1,.5f},dark);
        Triangle(back,{1,-1,.5f},{1,1,.5f},{-1,1,.5f},dark);
        batches[count++]={back.data(),static_cast<unsigned>(back.size()),D3DPT_TRIANGLELIST,mml_legacy::OnTop,identity,identity,&viewport};
        LookAt(insetEye,{0,0,0},view);
        const float f=1/std::tan(.52f);projection[0]=f*viewport.Height/viewport.Width;
        projection[5]=f;projection[10]=20/19.9f;projection[11]=1;projection[14]=-.1f*20/19.9f;
        batches[count++]={miniFill.data(),static_cast<unsigned>(miniFill.size()),D3DPT_TRIANGLELIST,
            mml_legacy::OnTop,view,projection,&viewport};
        batches[count++]={miniEcho.data(),static_cast<unsigned>(miniEcho.size()),D3DPT_TRIANGLELIST,
            mml_legacy::OnTop|mml_legacy::Additive,view,projection,&viewport};
        batches[count++]={miniWire.data(),static_cast<unsigned>(miniWire.size()),D3DPT_LINELIST,
            mml_legacy::OnTop|mml_legacy::Additive,view,projection,&viewport};
    }
    g_draw=mml_legacy::Draw(H->render_device(8),batches.data(),count);++g_drawFrames;
}
void Text(float x,float y,float size,uint32_t color,const char *t){
    H->draw_text(M,x,y,size,color,MML_FONT_SMALL,MML_DRAW_LEFT,t);
}
void Panel(){
    if(H->config_number(M,"panel",1)==0)return;
    H->draw_rect(M,20,24,284,169,0xD00B1421,1);H->draw_rect(M,20,24,2,169,Color(Cyan,.8f),1);
    Text(34,33,16,0xFFF0FCFF,"S O U N D S C A P E  /  3 D");
    std::string sector=g_audio.sectorName;if(sector.size()>36)sector=sector.substr(0,33)+"...";
    Text(34,55,11,0xFFB9C6D6,sector.c_str());char b[180];
    snprintf(b,sizeof b,"PCM %u sources | listener %.1f dBFS | master %.0f%%",g_metered,acoustic::Db(g_peak),g_audio.master*100);
    Text(34,73,11,Color(Cyan,.95f),b);
    const float meter=std::clamp((acoustic::Db(g_peak)+60)/60,0.0f,1.0f);
    H->draw_rect(M,34,92,250,4,0xFF24354A,1);H->draw_rect(M,34,92,250*meter,4,Color(Cyan,.9f),1);
    if(g_audio.eaxActive){
        snprintf(b,sizeof b,"EAX: decay %.2fs | early %.0fms | late +%.0fms",g_audio.eax.decay,
            g_audio.eax.reflectionsDelay*1000,g_audio.eax.reverbDelay*1000);
        Text(34,106,11,Color(Amber,.95f),b);
        snprintf(b,sizeof b,"EAX model  |  early %.0f dB  |  tail %.0f dB",acoustic::Db(g_early),acoustic::Db(g_late));
        Text(34,123,11,Color(Amber,.8f),b);
        if(g_time-g_tailSample>=.055f){
            g_tailHistory[g_tailCursor++%g_tailHistory.size()]=g_late;
            g_tailSample=g_time;
        }
        H->draw_rect(M,34,140,250,30,0xDD182437,1);
        for(unsigned i=0;i<g_tailHistory.size();i++){
            const float level=g_tailHistory[(g_tailCursor+i)%g_tailHistory.size()];
            const float height=28*std::clamp((acoustic::Db(level)+75)/75,0.0f,1.0f);
            if(height>0)H->draw_rect(M,35+5.18f*i,169-height,4,height,Color(Amber,.77f),1);
        }
    }else Text(34,106,11,0xFFB9C6D6,"EAX inactive | geometric reflection model");
    Text(34,177,9,0xFFA3B4CA,"CYAN: DIRECT  /  AMBER: REFLECTED  /  Shift+F10");
    if(g_room.Ready()&&H->config_number(M,"hologram",1)!=0){
        Text(565,36,12,Color(Violet,.95f),"LOCAL ACOUSTIC SURFACES");
        const V size=g_room.hi-g_room.lo;
        snprintf(b,sizeof b,"%.1f x %.1f x %.1f m  |  %u%% ray hits",size.x,size.y,size.z,
            g_room.hits*100/acoustic::Room::Count);
        Text(561,246,11,Color(Violet,.95f),b);
        Text(561,262,9,0xFFAFBDCF,"SAMPLED SPAN  /  CUTAWAY VIEW");
    }
}
std::string Quote(const std::string &s){
    std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\')out+='\\';if(c>=32)out+=char(c);}return out+'"';
}
int Status(const char *,MmlReply *reply,void *){
    const auto native=audio_view::Capture(H,M);
    std::ostringstream o;
    o<<"{\"shown\":"<<(g_show?"true":"false")<<",\"known\":"<<(g_known?"true":"false")
     <<",\"version\":3,\"eax_active\":"<<(native.eaxActive?"true":"false")<<",\"sector\":"<<Quote(native.sectorName)
     <<",\"registered\":"<<native.registered<<",\"waves\":"<<g_waves.size()<<",\"emitted\":"<<g_emitted
     <<",\"time\":"<<g_time<<",\"visual_speed\":"<<g_speed<<",\"lines_last\":0,\"measured_waveform\":true"
     <<",\"traced_reflections\":true,\"measured_wet_output\":false,\"master\":"<<native.master
     <<",\"listener_level\":"<<g_peak<<",\"echo_envelope\":"<<(g_early+g_late)<<",\"metered\":"<<g_metered
     <<",\"eax_early\":"<<g_early<<",\"eax_late\":"<<g_late
     <<",\"direct_triangles\":"<<g_directTriangles<<",\"echo_pulses\":"<<g_echoPulses<<",\"room_seed_triangles\":"<<g_seedTriangles
     <<",\"unsupported_pcm\":"<<g_pcm.unsupported<<",\"rays_last\":"<<g_rays<<",\"reflection_fronts\":"<<g_reflected
     <<",\"world_triangles\":"<<(g_frontFill.size()+g_roomFill.size())/3<<",\"room_triangles\":"<<g_roomFill.size()/3
     <<",\"room_samples\":"<<g_room.done<<",\"room_hits\":"<<g_room.hits
     <<",\"native_surface_triangles\":"<<g_surfaces.size()<<",\"shapes_available\":"<<g_shapesFound
     <<",\"render_frames\":"<<g_drawFrames<<",\"primitives\":"<<g_draw.primitives
     <<",\"state_restored\":"<<(g_draw.restored?"true":"false")<<",\"render_hresult\":"<<g_draw.hr
     <<",\"room_span\":["<<g_room.hi.x-g_room.lo.x<<','<<g_room.hi.y-g_room.lo.y<<','<<g_room.hi.z-g_room.lo.z
     <<"],\"sources\":[";
    bool first=true;for(const auto &s:native.sources){
        if(!first)o<<',';first=false;const auto it=g_levels.find(s.frame);
        const Level l=it!=g_levels.end()?it->second:Level{};
        o<<"{\"frame\":"<<s.frame<<",\"name\":"<<Quote(s.name)<<",\"pos\":["<<s.pos[0]<<','<<s.pos[1]<<','<<s.pos[2]
         <<"],\"volume\":"<<s.volume<<",\"near\":"<<s.nearRange<<",\"far\":"<<s.farRange<<",\"logical\":true"
         <<",\"pcm_measured\":"<<(l.measured?"true":"false")<<",\"rms\":"<<l.rms<<",\"backend_gain\":"<<l.backend
         <<",\"emission_level\":"<<l.emission<<",\"listener_level\":"<<l.listener<<'}';
    }
    o<<"],\"eax\":{\"decay\":"<<native.eax.decay<<",\"size\":"<<native.eax.size
     <<",\"early_delay\":"<<native.eax.reflectionsDelay<<",\"late_delay\":"<<native.eax.reverbDelay<<"}}";
    const auto out=o.str();H->reply_append(reply,out.c_str(),out.size());return 0;
}
void Settings(){
    g_show=H->config_number(M,"show",1)!=0;g_speed=Option("speed",14,3,40);
    g_intensity=Option("intensity",1,.2f,2);g_range=Option("range",55,10,120);
    H->hotkey(M,H->config_string(M,"hotkey","Shift+F10"),[](void *){
        g_show=!g_show;H->config_set(M,"show",g_show?"true":"false");
    },nullptr);if(!g_show)Reset();
}
void Frame(const MmlEvent *ev,void *){
    if(!g_known||!g_show)return;
    const int state=H->mission_state();if(state!=MML_MISSION_RUNNING&&state!=MML_MISSION_PAUSED){Reset();return;}
    float eye[3],forward[3];if(!H->camera_ray(M,400,300,eye,forward))return;
    g_eye=Vec(eye);g_forward=Vec(forward);g_rays=0;
    if(state==MML_MISSION_RUNNING)g_time+=std::clamp(ev->dt,0.0f,.1f);
    if(g_time-g_sampleAt>=.045f){Sample(state==MML_MISSION_RUNNING);g_sampleAt=g_time;}
    Probe();
    g_waves.erase(std::remove_if(g_waves.begin(),g_waves.end(),[](const Wave &w){
        return (g_time-w.birth)*g_speed>=w.reach;
    }),g_waves.end());
    g_wet.erase(std::remove_if(g_wet.begin(),g_wet.end(),[](const WetPulse &pulse){
        const auto &eax=pulse.eax;
        return g_time-pulse.birth>eax.decay+eax.reflectionsDelay+eax.reverbDelay+(eax.echoDepth>0?6*eax.echoTime:0);
    }),g_wet.end());
    Panel();
    if(H->config_number(M,"labels",0)!=0)for(const auto &s:g_audio.sources){
        const auto it=g_levels.find(s.frame);if(it==g_levels.end()||!it->second.measured)continue;
        float screen[3];if(!H->camera_project(M,s.pos,screen)||screen[0]<0||screen[0]>760||screen[1]<0||screen[1]>580)continue;
        const auto slash=s.name.find_last_of("/\\");const std::string name=s.name.substr(slash==std::string::npos?0:slash+1);
        char text[240];snprintf(text,sizeof text,"%s | %.1f dBFS",name.c_str(),acoustic::Db(it->second.listener));
        Text(screen[0],screen[1],10,Color(Cyan,.9f),text);
    }
}
} // namespace
MML_EXPORT uint32_t mml_mod_api(void){return MML_API_VERSION;}
MML_EXPORT int mml_mod_load(const MmlHost *h,MmlMod *m){
    H=h;M=m;
    if(!MML_HAS(H,file_read)||!MML_HAS(H,render_device))return 1;
    g_known=game::Detect(H->module_base("LS3DF.dll"));Settings();
    H->subscribe(M,MML_EVENT_FRAME,Frame,nullptr);H->subscribe(M,MML_EVENT_RENDER_WORLD,World,nullptr);
    H->subscribe(M,MML_EVENT_CONFIG_CHANGED,[](const MmlEvent *,void *){Settings();},nullptr);
    for(auto t:{MML_EVENT_GAME_LOADED,MML_EVENT_MISSION_CLOSE})H->subscribe(M,t,[](const MmlEvent *,void *){Reset();},nullptr);
    H->command(M,"status","3D room/paths, measured source PCM/native gain and predicted EAX envelope",Status,nullptr);
    H->log(M,MML_LOG_INFO,"Soundscape 0.3: continuous wavefronts and connected acoustic surfaces");return 0;
}
MML_EXPORT void mml_mod_unload(MmlMod *){Reset();}
