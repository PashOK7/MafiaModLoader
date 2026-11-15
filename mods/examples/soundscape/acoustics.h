// Pure geometry/energy math, independent of game memory and renderer.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include "mml/game/mathx.h"

namespace acoustic {
constexpr float Pi=3.141592653589793f;
inline float Gain(float millibels){return millibels<=-10000?0:std::pow(10.0f,millibels/2000);}
inline float Db(float gain){return gain>1e-7f?20*std::log10(gain):-140;}
inline float Light(float level){return std::clamp(std::log1p(std::max(0.0f,level)*400)/std::log(401.0f),0.0f,1.0f);}
inline V3 Reflect(V3 direction,V3 normal){return Norm(direction-normal*(2*Dot(direction,normal)));}
inline V3 Direction(int row,int column,int rows,int columns) {
    const float pitch=-Pi*.5f+Pi*row/rows, yaw=2*Pi*column/columns;
    return {std::cos(pitch)*std::cos(yaw),std::sin(pitch),std::cos(pitch)*std::sin(yaw)};
}
struct Hit { V3 at{},normal{}; bool hit=false; const void *prim=nullptr; };
struct Path {
    std::array<V3,4> point{};
    std::array<float,3> length{};
    unsigned segments=0;
    float firstHit=0;
    bool hit=false;
};
template<class Ray> Path Trace(V3 start,V3 direction,float reach,Ray ray) {
    Path p;p.point[0]=start;
    V3 from=start,dir=Norm(direction);
    for(unsigned i=0;i<3;i++) {
        const V3 target=from+dir*reach;
        const Hit h=ray(from,target);
        p.point[i+1]=h.hit?h.at:target;
        p.length[i]=Len(p.point[i+1]-p.point[i]);
        ++p.segments;
        if(!i){p.hit=h.hit;p.firstHit=p.length[0];}
        if(!h.hit)break;
        dir=Reflect(dir,Norm(h.normal));from=h.at+dir*.035f;
    }
    return p;
}
struct Front { V3 at{},direction{}; unsigned bounce=0; bool ended=false; };
inline Front At(const Path &p,float distance) {
    if(!p.segments)return {};
    for(unsigned i=0;i<p.segments;i++) {
        if(distance<=p.length[i]||i+1==p.segments) {
            const V3 d=Norm(p.point[i+1]-p.point[i]);
            return {p.point[i]+d*std::min(distance,p.length[i]),d,i,distance>p.length[i]};
        }
        distance-=p.length[i];
    }
    return {};
}
// Modeled EAX energy envelope. RT60 is -60 dB after decay seconds.
// No output minimum: mute and zero RMS remain exactly dark.
inline float Tail(float amplitude,float age,float delay,float decay,float room,float reverb,
                  float echoTime,float echoDepth) {
    if(amplitude<=0||age<delay||decay<=0)return 0;
    const float t=age-delay;
    float v=amplitude*Gain(room+reverb)*std::exp(-6.907755f*t/decay);
    if(echoDepth>0&&echoTime>.001f)for(int n=1;n<=6;n++) {
        const float e=t-n*echoTime;
        if(e>=0)v+=amplitude*Gain(room+reverb)*std::pow(echoDepth,static_cast<float>(n))*
            std::exp(-6.907755f*e/decay);
    }
    return v;
}
struct RoomPoint { V3 at{},normal{}; bool hit=false; const void *prim=nullptr; };
struct Room {
    static constexpr int Rows=10,Columns=24,Count=(Rows+1)*Columns;
    V3 center{},lo{},hi{};
    std::array<RoomPoint,Count> points{};
    unsigned done=0,hits=0;
    void Begin(V3 c){center=lo=hi=c;done=hits=0;points={};}
    void Add(unsigned i,Hit h,V3 end) {
        points[i]={h.hit?h.at:end,h.normal,h.hit,h.prim};
        if(h.hit){++hits;for(int k=0;k<3;k++){
            (&lo.x)[k]=std::min((&lo.x)[k],(&h.at.x)[k]);(&hi.x)[k]=std::max((&hi.x)[k],(&h.at.x)[k]);}}
        ++done;
    }
    bool Ready()const{return done==Count;}
};
inline std::vector<V3> ClipTriangle(V3 a,V3 b,V3 c,V3 lo,V3 hi){
    std::vector<V3> polygon{a,b,c};
    for(int axis=0;axis<3;axis++)for(int side=0;side<2;side++){
        if(polygon.empty())return polygon;
        const float plane=side?(&hi.x)[axis]:(&lo.x)[axis];
        auto distance=[&](V3 p){return ((&p.x)[axis]-plane)*(side?-1.0f:1.0f);};
        std::vector<V3> clipped;V3 previous=polygon.back();float dp=distance(previous);
        for(V3 current:polygon){
            const float dc=distance(current);
            if((dc>=0)!=(dp>=0))clipped.push_back(previous+(current-previous)*(dp/(dp-dc)));
            if(dc>=0)clipped.push_back(current);previous=current;dp=dc;
        }
        polygon=std::move(clipped);
    }
    return polygon;
}
} // namespace acoustic
