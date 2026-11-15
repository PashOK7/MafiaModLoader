// Diagnostic WASAPI render-loopback recording. No microphone or game mutation.
// The endpoint mix is captured, not an isolated process (Windows 10 build 19045).
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

using Microsoft::WRL::ComPtr;
void Check(HRESULT h) { if (FAILED(h)) throw h; }
uint64_t Qpc100ns() {
    LARGE_INTEGER q,f;
    QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    return uint64_t(q.QuadPart/f.QuadPart)*10000000ull +
           uint64_t(q.QuadPart%f.QuadPart)*10000000ull/uint64_t(f.QuadPart);
}
void U32(std::ofstream &f,uint32_t x) { f.write(reinterpret_cast<char *>(&x),4); }
int wmain(int argc,wchar_t **argv) {
    if (argc!=4 && argc!=5) { fprintf(stderr,"loopback output.wav seconds expected_pid [known_background_pid]\n"); return 2; }
    if (GetFileAttributesW(argv[1])!=INVALID_FILE_ATTRIBUTES) { fprintf(stderr,"output exists\n");return 2; }
    WAVEFORMATEX *format=nullptr;
    bool started=false;
    ComPtr<IAudioClient> client;
    try {
        Check(CoInitializeEx(nullptr,COINIT_MULTITHREADED));
        ComPtr<IMMDeviceEnumerator> enumerator;
        Check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)));
        ComPtr<IMMDevice> device;
        Check(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device));
        // Session IDs establish whether other applications could contribute to this endpoint mix.
        ComPtr<IAudioSessionManager2> manager;
        Check(device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,&manager));
        ComPtr<IAudioSessionEnumerator> sessions;
        Check(manager->GetSessionEnumerator(&sessions));
        int count=0;Check(sessions->GetCount(&count));
        std::vector<ComPtr<IAudioMeterInformation>> otherMeters;
        for(int i=0;i<count;++i) {
            ComPtr<IAudioSessionControl> control;Check(sessions->GetSession(i,&control));
            AudioSessionState state;Check(control->GetState(&state));
            if(state!=AudioSessionStateActive) continue;
            ComPtr<IAudioSessionControl2> control2;Check(control.As(&control2));
            DWORD pid=0;Check(control2->GetProcessId(&pid));
            printf("{\"active_session_pid\":%lu}\n",pid);
            if(pid!=wcstoul(argv[3],nullptr,10) && (argc!=5 || pid!=wcstoul(argv[4],nullptr,10))) {
                ComPtr<IAudioMeterInformation> meter;Check(control.As(&meter));
                otherMeters.push_back(meter);
            }
        }
        auto checkOtherAudio = [&]() {
            for(const auto &meter:otherMeters) {
                float peak=0;Check(meter->GetPeakValue(&peak));
                if(peak>0.000001f) {
                    fprintf(stderr,"another render session has a nonzero signal; recording discarded\n");
                    throw E_ACCESSDENIED;
                }
            }
        };
        // Browsers may keep a session active after playback stops. Verify the signal, not that flag alone.
        for(int i=0;i<20;++i) {checkOtherAudio();Sleep(10);}
        Check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,&client));
        Check(client->GetMixFormat(&format));
        Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK,1000000,0,format,nullptr));
        ComPtr<IAudioCaptureClient> capture;Check(client->GetService(IID_PPV_ARGS(&capture)));
        const double seconds=_wtof(argv[2]);
        if(seconds<=0||seconds>60) throw E_INVALIDARG;
        const size_t frames=size_t(seconds*format->nSamplesPerSec);
        std::vector<unsigned char> data(frames*format->nBlockAlign,0);
        uint64_t start=Qpc100ns();
        Check(client->Start());started=true;
        printf("{\"ready\":true,\"start_qpc_100ns\":%llu,\"rate\":%lu,\"channels\":%u,\"bits\":%u}\n",
               start,format->nSamplesPerSec,format->nChannels,format->wBitsPerSample);
        fflush(stdout);
        unsigned packets=0,discontinuities=0,timestampErrors=0;
        while(Qpc100ns()-start<uint64_t(seconds*10000000.0)) {
            checkOtherAudio();
            UINT32 next=0;Check(capture->GetNextPacketSize(&next));
            while(next) {
                BYTE *bytes=nullptr;UINT32 n=0;DWORD flags=0;UINT64 position=0,qpc=0;
                Check(capture->GetBuffer(&bytes,&n,&flags,&position,&qpc));
                ++packets;
                if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
                if(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++timestampErrors;
                const int64_t offset=int64_t(qpc)-int64_t(start);
                const int64_t first=offset*format->nSamplesPerSec/10000000;
                int64_t skip=std::max<int64_t>(0,-first),target=std::max<int64_t>(0,first);
                if(!(flags&AUDCLNT_BUFFERFLAGS_SILENT) && target<int64_t(frames) && skip<int64_t(n)) {
                    const size_t take=std::min<size_t>(n-size_t(skip),frames-size_t(target));
                    memcpy(data.data()+target*format->nBlockAlign,bytes+skip*format->nBlockAlign,take*format->nBlockAlign);
                }
                Check(capture->ReleaseBuffer(n));
                Check(capture->GetNextPacketSize(&next));
            }
            Sleep(3);
        }
        Check(client->Stop());started=false;
        const uint32_t fmtBytes=sizeof(WAVEFORMATEX)+format->cbSize;
        std::ofstream file(argv[1],std::ios::binary|std::ios::out);
        if(!file) throw E_FAIL;
        file.write("RIFF",4);U32(file,4+8+fmtBytes+8+uint32_t(data.size()));file.write("WAVEfmt ",8);
        U32(file,fmtBytes);file.write(reinterpret_cast<char *>(format),fmtBytes);
        file.write("data",4);U32(file,uint32_t(data.size()));file.write(reinterpret_cast<char *>(data.data()),data.size());
        if(!file) throw E_FAIL;
        printf("{\"complete\":true,\"packets\":%u,\"discontinuities\":%u,\"timestamp_errors\":%u}\n",
               packets,discontinuities,timestampErrors);
        CoTaskMemFree(format);CoUninitialize();return 0;
    } catch(HRESULT h) {
        if(started) client->Stop();
        if(format) CoTaskMemFree(format);
        fprintf(stderr,"loopback failed HRESULT %08lX\n",h);return 1;
    }
}
