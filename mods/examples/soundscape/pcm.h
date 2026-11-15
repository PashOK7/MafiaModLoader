// Bounded source-WAV cache. No sound-buffer locks/writes or decoder ownership changes.
// PCM source analysis is distinct from post-EAX mixed output.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "mml/mml.h"
namespace pcm {
struct Clip {
    std::vector<uint8_t> data;
    unsigned channels=0,bits=0,rate=0,offset=0,bytes=0;
    bool valid=false;
    float rms=0,peak=0;
};
inline uint16_t U16(const uint8_t *p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
inline uint32_t U32(const uint8_t *p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline float Sample(const Clip &c,unsigned frame,unsigned channel) {
    const unsigned at=c.offset+(frame*c.channels+channel)*(c.bits/8);
    if(at+c.bits/8>c.data.size())return 0;
    return c.bits==8?(int(c.data[at])-128)/128.0f:
        static_cast<int16_t>(U16(c.data.data()+at))/32768.0f;
}
inline void Parse(Clip &c) {
    if(c.data.size()<12||memcmp(c.data.data(),"RIFF",4)||memcmp(c.data.data()+8,"WAVE",4))return;
    unsigned tag=0;
    for(size_t i=12;i+8<=c.data.size();) {
        const uint32_t bytes=U32(c.data.data()+i+4);
        if(bytes>c.data.size()-i-8)return;
        const auto *p=c.data.data()+i+8;
        if(!memcmp(c.data.data()+i,"fmt ",4)&&bytes>=16) {
            tag=U16(p);c.channels=U16(p+2);c.rate=U32(p+4);c.bits=U16(p+14);
        } else if(!memcmp(c.data.data()+i,"data",4)){c.offset=static_cast<unsigned>(i+8);c.bytes=bytes;}
        i+=8+size_t(bytes)+(bytes&1);
    }
    c.valid=tag==1&&(c.channels==1||c.channels==2)&&(c.bits==8||c.bits==16)&&c.rate>=4000&&c.rate<=192000&&c.bytes>0;
}
inline void Window(Clip &c,uint32_t cursor,bool loop) {
    c.rms=c.peak=0;if(!c.valid)return;
    const unsigned frameBytes=c.channels*(c.bits/8),frames=c.bytes/frameBytes;
    if(!frames)return;
    const unsigned start=(cursor/frameBytes)%frames;
    const unsigned window=std::min(c.rate/25,frames);
    double sum=0;unsigned count=0;
    for(unsigned i=0;i<window;i++) {
        unsigned f=start+i;if(f>=frames){if(!loop)break;f%=frames;}
        for(unsigned ch=0;ch<c.channels;ch++){const float v=Sample(c,f,ch);sum+=v*v;c.peak=std::max(c.peak,std::abs(v));++count;}
    }
    if(count)c.rms=static_cast<float>(std::sqrt(sum/count));
}
class Cache {
    std::unordered_map<std::string,Clip> clips;
    size_t bytes=0;
public:
    unsigned misses=0,unsupported=0;
    const Clip *Get(const MmlHost *h,MmlMod *m,const std::string &name,uint32_t cursor,bool loop,bool allowLoad) {
        auto it=clips.find(name);
        if(it==clips.end()) {
            if(!allowLoad||name.empty())return nullptr;
            if(clips.size()>=64||bytes>=24*1024*1024)return nullptr;
            ++misses;Clip clip;
            const int64_t size=h->file_read(m,name.c_str(),nullptr,0,0);
            if(size>0&&size<=4*1024*1024) {
                clip.data.resize(static_cast<size_t>(size));
                if(h->file_read(m,name.c_str(),clip.data.data(),clip.data.size(),0)==size)Parse(clip);
                bytes+=clip.data.size();
            }
            if(!clip.valid)++unsupported;
            it=clips.emplace(name,std::move(clip)).first;
        }
        Window(it->second,cursor,loop);return &it->second;
    }
    void Clear(){clips.clear();bytes=0;misses=unsupported=0;}
};
} // namespace pcm
