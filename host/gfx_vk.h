// The graphics bridge on DXVK (gfx_vk.cpp): the game's Direct3D 9 may be DXVK's (its d3d9.dll beside the game, which
// dxwrapper loads before the system's), whose shared handles are Vulkan's own and cannot be opened by Direct3D 11.
// There each shared texture is two: the Direct3D 9 one (DXVK's image) and a Direct3D 11 one made shared by Direct3D
// 11 and imported into DXVK's Vulkan device, copied on DXVK's queue at each hand-over between the two devices.
#pragma once
#include <d3d9.h>
#include <d3d11.h>
#include <string>

namespace host {
namespace gfxvk {

struct Link;   // one texture's two halves and their copies

// The game's device is DXVK's: Vulkan made ready for the copies (true). False with why empty: not DXVK (Direct3D 9Ex
// shares by handle); false with why: DXVK, but its device cannot take the copies (why says what is missing).
bool Start(IDirect3DDevice9 *dev9, std::string *why);
bool On();
void Stop();                                   // every link freed first
bool IsDxvk(IDirect3DDevice9 *dev9);           // DXVK's device (its interop interface answers)

// The Direct3D 11 half of a Direct3D 9 texture (made by the caller, D3DPOOL_DEFAULT, no shared handle): a shared
// texture of the same size and format (render target and shader resource), imported into DXVK's device. Null and why
// when it cannot be.
Link *Make(IDirect3DTexture9 *t9, ID3D11Device *dev11, ID3D11Texture2D **t11, std::string *why);
void Free(Link *&link);

// The hand-overs: Direct3D 9's pictures to Direct3D 11's halves (Direct3D 9 is done: the caller waited for its event
// query), Direct3D 11's to Direct3D 9's (Direct3D 11 is done: its event query waited for). Null links are skipped;
// each returns once its copies are done on the GPU.
bool To11(Link *const *links, int n, std::string *why);
bool To9(Link *const *links, int n, std::string *why);

std::string Describe();                        // for gfx.state: the device, the copies, their time
} // namespace gfxvk
} // namespace host
