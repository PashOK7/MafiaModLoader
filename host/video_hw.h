// The GPU's part of the game's videos (video_hw.cpp, for video.cpp): the decoders of the GPU through FFmpeg's
// D3D11VA and DXVA2, and each picture turned into 32-bit colour at its size on the screen by the GPU, handed to the
// game's Direct3D 9 device in a shared texture (or, where the game's device cannot open one: DXVK's, plain
// Direct3D 9, copied through the CPU at that small size).
#pragma once
#include <string>
#include <vector>
#include <d3d9.h>

struct AVBufferRef;
struct AVCodecParameters;
struct AVFrame;

namespace host {
namespace videohw {

enum Api { kSoftware = 0, kD3D11 = 1, kDxva2 = 2 };
const char *ApiName(Api api);   // "D3D11VA", "DXVA2", "software"

// Before the decoder is opened (any thread but the game's device is read: the game thread): whether the GPU decodes
// this stream, and through what. force: "" or "auto" (D3D11VA, else DXVA2, else software), "d3d11va", "dxva2",
// "software". *device: the FFmpeg device to give the decoder (a new reference; null for software); *why: why not
// the GPU (or why not the API asked for).
Api Choose(const AVCodecParameters *par, const std::string &force, AVBufferRef **device, std::string *why);
// The pixel format FFmpeg gives a picture of this API in (AV_PIX_FMT_D3D11 or AV_PIX_FMT_DXVA2_VLD), -1 software.
int PixelFormat(Api api);

// One video's pictures on the GPU (game thread: Begin, SlotTexture, Retire, End; the decoding thread: Convert).
struct Session;
// slots: how many pictures may be on their way at once; w, h: the size each is turned to (its size on the screen).
Session *Begin(Api api, IDirect3DDevice9 *game9, int w, int h, int slots, std::string *why);
bool Shared(const Session *s);   // the pictures go to the game's device in shared textures (else copied through the CPU)
std::string Path(const Session *s);   // "D3D11VA, a shared texture", "DXVA2, copied through the CPU"...
// A GPU picture into slot `slot` (shared) or into `bgra` (w * h * 4, copied); false and why when the GPU could not.
bool Convert(Session *s, const AVFrame *frame, int slot, std::vector<uint8_t> *bgra, std::string *why);
IDirect3DTexture9 *SlotTexture(Session *s, int slot);
// The game's device has drawn slot `slot`: an event query issued; Retired tells when the GPU is done with it.
void Drawn(Session *s, int slot);
bool Retired(Session *s, int slot);
void End(Session *s);

} // namespace videohw
} // namespace host
