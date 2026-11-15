// Real D3D8 -> D3D9Ex resize regression. No game assets, foreground window or injected desktop input.
#pragma once
#include <d3d9.h>
#include "../../sdk/include/mml/native_texture_revision.h"

inline uint64_t TextureRevision(IDirect3DTexture9 *texture) {
    uint64_t value=0;
    DWORD bytes=sizeof value;
    if (FAILED(texture->GetPrivateData(mml::scene::NativeTextureRevision,&value,&bytes))||bytes!=sizeof value) return 0;
    return value;
}

inline bool ResizeCheck(HMODULE module) {
    auto create = reinterpret_cast<void *(WINAPI *)(UINT)>(GetProcAddress(module,"Direct3DCreate8"));
    void *d8 = create ? create(220) : nullptr, *dev = nullptr;
    HWND wnd = CreateWindowExW(WS_EX_NOACTIVATE,L"STATIC",L"MML resize test",WS_OVERLAPPEDWINDOW,
                              0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    DWORD p8[13] = {640,480,D3DFMT_X8R8G8B8,1,0,D3DSWAPEFFECT_DISCARD,DWORD(uintptr_t(wnd)),TRUE,0,0,0,0,0};
    HRESULT hr = E_FAIL;
    if (d8 && wnd) {
        using Make=HRESULT(__stdcall *)(void *,UINT,DWORD,HWND,DWORD,DWORD *,void **);
        hr=reinterpret_cast<Make>((*static_cast<void ***>(d8))[15])(d8,0,D3DDEVTYPE_HAL,wnd,
              D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_MULTITHREADED,p8,&dev);
    }
    IDirect3DDevice9Ex *d9=nullptr;
    if (SUCCEEDED(hr)) hr=static_cast<IUnknown *>(dev)->QueryInterface(__uuidof(IDirect3DDevice9Ex),reinterpret_cast<void **>(&d9));
    IDirect3DTexture9 *managed=nullptr;
    if (SUCCEEDED(hr)) hr=d9->CreateTexture(4,4,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&managed,nullptr);
    D3DLOCKED_RECT lock{};
    if (SUCCEEDED(hr)) hr=managed->LockRect(0,&lock,nullptr,0);
    if (SUCCEEDED(hr)) { *static_cast<DWORD *>(lock.pBits)=0xFF123456; hr=managed->UnlockRect(0); }
    const uint64_t first=SUCCEEDED(hr)?TextureRevision(managed):0;
    if (SUCCEEDED(hr)&&first==0) hr=E_FAIL;
    IDirect3DSurface9 *level=nullptr;
    if (SUCCEEDED(hr)) hr=managed->GetSurfaceLevel(0,&level);
    if (SUCCEEDED(hr)) hr=level->LockRect(&lock,nullptr,0);
    if (SUCCEEDED(hr)) {
        const bool invalidated=TextureRevision(managed)==0;
        *static_cast<DWORD *>(lock.pBits)=0xFF654321;
        hr=level->UnlockRect();
        if (SUCCEEDED(hr)&&!invalidated) hr=E_FAIL;
    }
    if (level) level->Release();
    const uint64_t revision=SUCCEEDED(hr)?TextureRevision(managed):0;
    if (SUCCEEDED(hr)&&(revision==0||revision==first)) hr=E_FAIL;
    for (UINT width : {960u,640u,800u}) {
        if (FAILED(hr)) break;
        D3DPRESENT_PARAMETERS p{};
        p.BackBufferWidth=width; p.BackBufferHeight=480; p.BackBufferFormat=D3DFMT_X8R8G8B8;
        p.BackBufferCount=1; p.SwapEffect=D3DSWAPEFFECT_DISCARD; p.hDeviceWindow=wnd; p.Windowed=TRUE;
        hr=d9->Reset(&p); // must reach ResetEx with a NULL fullscreen descriptor
        IDirect3DSurface9 *back=nullptr;
        if (SUCCEEDED(hr)) hr=d9->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back);
        D3DSURFACE_DESC desc{};
        if (back) { back->GetDesc(&desc); back->Release(); }
        if (SUCCEEDED(hr) && (desc.Width!=width || desc.Height!=480)) hr=E_FAIL;
        if (SUCCEEDED(hr)) hr=managed->LockRect(0,&lock,nullptr,D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) {
            const bool same=*static_cast<DWORD *>(lock.pBits)==0xFF654321;
            hr=managed->UnlockRect(0);
            if (SUCCEEDED(hr)&&(!same||TextureRevision(managed)!=revision)) hr=E_FAIL;
        }
        printf("resize: %ux480, managed texture retained: %08lX\n",width,unsigned long(hr));
    }
    if (managed) managed->Release();
    if (d9) d9->Release();
    if (dev) static_cast<IUnknown *>(dev)->Release();
    if (d8) static_cast<IUnknown *>(d8)->Release();
    if (wnd) DestroyWindow(wnd);
    return SUCCEEDED(hr);
}
