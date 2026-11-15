#pragma once
#include <guiddef.h>

namespace mml::scene {
// Optional IDirect3DResource9 private data. The resource owner publishes a
// nonzero uint64_t unique to the resource lifetime and current texel contents.
// Missing or zero means the consumer must verify bytes as usual.
inline constexpr GUID NativeTextureRevision =
    {0x9c548926,0x505c,0x44c5,{0xa6,0x19,0x30,0x80,0x42,0x59,0x78,0xe1}};
}
