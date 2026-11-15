/* Native material texture snapshot: original mip texels/BC blocks, not pixels from a scene camera.
 * Read only during the source's valid borrow or while holding a COM reference. A failed/unsupported copy is
 * unavailable material data, never a white/black replacement. CPU snapshot owns its bytes; GPU objects belong
 * to the caller and must be released on reset/unload. No implicit sRGB decode: LS3D's fixed stages operate on
 * encoded colors; decode their final result into linear radiance. Animated textures need explicit refresh. */
#pragma once
#include <d3d9.h>
#include <d3d11.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace mml { namespace scene {
enum class TextureResult { Valid, Missing, Unsupported, Unreadable };
struct TextureMip { unsigned width=0,height=0,pitch=0,rows=0;std::vector<uint8_t> bytes; };
struct TextureSnapshot {
    DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    D3DFORMAT native=D3DFMT_UNKNOWN;
    std::vector<TextureMip> mips;
    // A pointer only selects the candidate. Validate ALL current native mip bytes before immutable reuse.
    // No COM pointer is retained; changed metadata is a mismatch, failed reads are unavailable, never a hit.
    TextureResult Verify(IDirect3DBaseTexture9 *source,bool &equal)const {
        equal=false;if(!source)return TextureResult::Missing;
        if(source->GetType()!=D3DRTYPE_TEXTURE)return TextureResult::Unsupported;
        auto *texture=static_cast<IDirect3DTexture9 *>(source);
        if(texture->GetLevelCount()!=mips.size())return TextureResult::Valid;
        for(unsigned level=0;level<mips.size();level++){
            const auto &m=mips[level];D3DSURFACE_DESC d{};
            if(FAILED(texture->GetLevelDesc(level,&d)))return TextureResult::Unreadable;
            if(d.Format!=native||d.Width!=m.width||d.Height!=m.height)return TextureResult::Valid;
            if(m.bytes.size()!=size_t(m.pitch)*m.rows)return TextureResult::Unreadable;
            D3DLOCKED_RECT lock{};if(FAILED(texture->LockRect(level,&lock,nullptr,D3DLOCK_READONLY)))return TextureResult::Unreadable;
            const bool readable=lock.pBits&&lock.Pitch>0&&unsigned(lock.Pitch)>=m.pitch;bool match=readable;
            if(readable)for(unsigned row=0;row<m.rows;row++)
                if(memcmp(m.bytes.data()+size_t(row)*m.pitch,static_cast<const uint8_t *>(lock.pBits)+size_t(row)*lock.Pitch,m.pitch)!=0){match=false;break;}
            const HRESULT unlocked=texture->UnlockRect(level);
            if(!readable||FAILED(unlocked))return TextureResult::Unreadable;
            if(!match)return TextureResult::Valid;
        }
        equal=!mips.empty();return TextureResult::Valid;
    }
    TextureResult Read(IDirect3DBaseTexture9 *source) {
        if(!source)return TextureResult::Missing;
        if(source->GetType()!=D3DRTYPE_TEXTURE)return TextureResult::Unsupported;
        auto *texture=static_cast<IDirect3DTexture9 *>(source);D3DSURFACE_DESC first{};
        if(FAILED(texture->GetLevelDesc(0,&first)))return TextureResult::Unreadable;
        unsigned block=1,bytes=4;DXGI_FORMAT target=DXGI_FORMAT_UNKNOWN;
        switch(first.Format){
        case D3DFMT_A8R8G8B8:target=DXGI_FORMAT_B8G8R8A8_UNORM;break;
        case D3DFMT_X8R8G8B8:target=DXGI_FORMAT_B8G8R8X8_UNORM;break;
        case D3DFMT_A8B8G8R8:target=DXGI_FORMAT_R8G8B8A8_UNORM;break;
        case D3DFMT_R5G6B5:target=DXGI_FORMAT_B5G6R5_UNORM;bytes=2;break;
        case D3DFMT_A1R5G5B5:target=DXGI_FORMAT_B5G5R5A1_UNORM;bytes=2;break;
        case D3DFMT_A4R4G4B4:target=DXGI_FORMAT_B4G4R4A4_UNORM;bytes=2;break;
        case D3DFMT_DXT1:target=DXGI_FORMAT_BC1_UNORM;block=4;bytes=8;break;
        case D3DFMT_DXT3:target=DXGI_FORMAT_BC2_UNORM;block=4;bytes=16;break;
        case D3DFMT_DXT5:target=DXGI_FORMAT_BC3_UNORM;block=4;bytes=16;break;
        default:return TextureResult::Unsupported;
        }
        const unsigned count=texture->GetLevelCount();
        if(!first.Width||!first.Height||first.Width>8192||first.Height>8192||!count||count>14)return TextureResult::Unsupported;
        TextureSnapshot next;next.native=first.Format;next.format=target;size_t total=0;
        next.mips.reserve(count);
        for(unsigned level=0;level<count;level++){
            D3DSURFACE_DESC d{};if(FAILED(texture->GetLevelDesc(level,&d)))return TextureResult::Unreadable;
            if(d.Format!=first.Format||d.Width!=(std::max)(1u,first.Width>>level)||d.Height!=(std::max)(1u,first.Height>>level))return TextureResult::Unsupported;
            TextureMip mip;mip.width=d.Width;mip.height=d.Height;mip.pitch=((d.Width+block-1)/block)*bytes;mip.rows=(d.Height+block-1)/block;
            const size_t length=size_t(mip.pitch)*mip.rows;total+=length;if(total>256u*1024*1024)return TextureResult::Unsupported;
            mip.bytes.resize(length);D3DLOCKED_RECT locked{};
            if(FAILED(texture->LockRect(level,&locked,nullptr,D3DLOCK_READONLY)))return TextureResult::Unreadable;
            const bool valid=locked.pBits&&locked.Pitch>0&&unsigned(locked.Pitch)>=mip.pitch;
            if(valid)for(unsigned row=0;row<mip.rows;row++)memcpy(mip.bytes.data()+size_t(row)*mip.pitch,static_cast<const uint8_t *>(locked.pBits)+size_t(row)*locked.Pitch,mip.pitch);
            const HRESULT unlock=texture->UnlockRect(level);
            if(!valid||FAILED(unlock))return TextureResult::Unreadable;
            next.mips.push_back(std::move(mip));
        }
        *this=std::move(next);return TextureResult::Valid;
    }
    bool Upload(ID3D11Device *device,ID3D11Texture2D **texture,ID3D11ShaderResourceView **view)const {
        if(!device||!texture||!view||mips.empty()||format==DXGI_FORMAT_UNKNOWN)return false;
        // Outputs are changed only after both objects were made successfully.
        D3D11_TEXTURE2D_DESC d{};d.Width=mips[0].width;d.Height=mips[0].height;d.MipLevels=static_cast<unsigned>(mips.size());
        d.ArraySize=d.SampleDesc.Count=1;d.Format=format;d.Usage=D3D11_USAGE_IMMUTABLE;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> data;data.reserve(mips.size());
        for(const auto &m:mips)data.push_back({m.bytes.data(),m.pitch,0});
        ID3D11Texture2D *made=nullptr;ID3D11ShaderResourceView *srv=nullptr;
        if(FAILED(device->CreateTexture2D(&d,data.data(),&made)))return false;
        if(FAILED(device->CreateShaderResourceView(made,nullptr,&srv))){made->Release();return false;}
        *texture=made;*view=srv;return true;
    }
};
// Snapshot tables share only owned immutable CPU texels. Existing value-based callers remain supported.
struct TextureRef {
    std::shared_ptr<const TextureSnapshot> value;
    TextureRef()=default;
    TextureRef(TextureSnapshot texture):value(std::make_shared<const TextureSnapshot>(std::move(texture))){}
    TextureRef(std::shared_ptr<const TextureSnapshot> texture):value(std::move(texture)){}
};
inline const TextureSnapshot *TextureData(const TextureSnapshot &texture){return &texture;}
inline const TextureSnapshot *TextureData(const TextureRef &texture){return texture.value.get();}
} }
