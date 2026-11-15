#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <cmath>
#include <cstdio>
#include <vector>

static void Check(HRESULT h) { if (FAILED(h)) throw h; }
static unsigned long long Qpc() {
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    return q.QuadPart/f.QuadPart*10000000ull + q.QuadPart%f.QuadPart*10000000ull/f.QuadPart;
}
static void Mark(const char *label, IDirectSoundBuffer *b) {
    LONG volume=0; DWORD frequency=0,status=0;
    Check(b->GetVolume(&volume)); Check(b->GetFrequency(&frequency)); Check(b->GetStatus(&status));
    printf("{\"label\":\"%s\",\"qpc_100ns\":%llu,\"volume\":%ld,\"frequency\":%lu,\"status\":%lu}\n",
           label,Qpc(),volume,frequency,status); fflush(stdout);
}
int main(int argc, char **) {
    try {
        const bool software=argc>1;
        const DWORD playFlags=DSBPLAY_LOOPING|(software?0:DSBPLAY_LOCSOFTWARE);
        IDirectSound8 *driver=nullptr; Check(DirectSoundCreate8(nullptr,&driver,nullptr));
        Check(driver->SetCooperativeLevel(GetDesktopWindow(),DSSCL_NORMAL));
        DSBUFFERDESC primaryDesc={}; primaryDesc.dwSize=sizeof(primaryDesc);
        primaryDesc.dwFlags=DSBCAPS_PRIMARYBUFFER|DSBCAPS_CTRL3D;
        IDirectSoundBuffer *primary=nullptr; Check(driver->CreateSoundBuffer(&primaryDesc,&primary,nullptr));
        IDirectSound3DListener *listener=nullptr;
        Check(primary->QueryInterface(IID_IDirectSound3DListener,reinterpret_cast<void **>(&listener)));
        Check(listener->SetPosition(0,0,0,DS3D_IMMEDIATE));
        WAVEFORMATEX wf={}; wf.wFormatTag=WAVE_FORMAT_PCM; wf.nChannels=1; wf.nSamplesPerSec=22050;
        wf.wBitsPerSample=16; wf.nBlockAlign=2; wf.nAvgBytesPerSec=44100;
        DSBUFFERDESC desc={}; desc.dwSize=sizeof(desc);
        desc.dwFlags=DSBCAPS_CTRLVOLUME|DSBCAPS_CTRLFREQUENCY|DSBCAPS_CTRL3D|DSBCAPS_GLOBALFOCUS|
                     (software?DSBCAPS_LOCSOFTWARE:DSBCAPS_LOCDEFER);
        desc.dwBufferBytes=44100; desc.lpwfxFormat=&wf; desc.guid3DAlgorithm=DS3DALG_NO_VIRTUALIZATION;
        IDirectSoundBuffer *source=nullptr,*buffer=nullptr;
        Check(driver->CreateSoundBuffer(&desc,&source,nullptr));
        void *p=nullptr; DWORD size=0; Check(source->Lock(0,0,&p,&size,nullptr,nullptr,DSBLOCK_ENTIREBUFFER));
        for (DWORD i=0;i<size/2;++i) static_cast<short *>(p)[i]=short(10000*sin(2*3.141592653589793*311*i/22050));
        Check(source->Unlock(p,size,nullptr,0)); Check(driver->DuplicateSoundBuffer(source,&buffer));
        IDirectSound3DBuffer *spatial=nullptr;
        Check(buffer->QueryInterface(IID_IDirectSound3DBuffer,reinterpret_cast<void **>(&spatial)));
        Check(spatial->SetMinDistance(1e9f,DS3D_IMMEDIATE)); Check(spatial->SetMaxDistance(1e9f,DS3D_IMMEDIATE));
        Check(spatial->SetPosition(0,0,5,DS3D_IMMEDIATE));
        Check(buffer->SetVolume(-1500)); Check(buffer->SetFrequency(16000));
        printf("{\"ready\":true,\"pid\":%lu}\n",GetCurrentProcessId()); fflush(stdout); getchar();
        Check(buffer->Play(0,0,playFlags)); Mark("initial",buffer); Sleep(2500);
        Check(buffer->SetVolume(-411)); Check(buffer->SetFrequency(30869));
        Mark("changed_while_playing",buffer); Sleep(2500);
        Check(buffer->Stop()); Sleep(300); Check(buffer->SetVolume(-411));
        Check(buffer->Play(0,0,playFlags)); Mark("restarted",buffer); Sleep(2500);
        Check(buffer->SetCurrentPosition(4400)); Mark("seek_while_playing",buffer); Sleep(2500);
        Check(buffer->SetVolume(-412)); Check(buffer->SetVolume(-411)); Mark("volume_transition",buffer); Sleep(2500);
        Check(buffer->SetFrequency(30870)); Check(buffer->SetFrequency(30869)); Mark("frequency_transition",buffer); Sleep(2500);
        Check(buffer->Stop()); Check(buffer->SetCurrentPosition(4400));
        Check(buffer->SetVolume(-411)); Check(buffer->SetFrequency(30869));
        Check(buffer->Play(0,0,playFlags)); Mark("seek_before_play",buffer); Sleep(2500);
        Check(buffer->Stop()); Mark("finish",buffer);
        spatial->Release(); buffer->Release(); source->Release(); listener->Release(); primary->Release(); driver->Release();
        return 0;
    } catch(HRESULT h) { fprintf(stderr,"HRESULT %08lX\n",h); return 1; }
}
