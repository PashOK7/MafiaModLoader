// Exact word transport. Integer atlases preserve arbitrary encoded texture bits.
// The producer copies a logical structured-buffer view into a raw GPU buffer.
#if defined(MML_TRANSFER_UNPACK)
Texture2D<uint> source:register(t0);
RWByteAddressBuffer target:register(u0);
#else
ByteAddressBuffer source:register(t0);
RWTexture2D<uint> target:register(u0);
#endif
cbuffer TransferDimensions:register(b0){uint wordCount,width;uint2 reserved;};
[numthreads(16,16,1)]
void CsTransferWords(uint3 id:SV_DispatchThreadID){
    uint word=id.y*width+id.x;
    if(id.x>=width||word>=wordCount)return;
#if defined(MML_TRANSFER_UNPACK)
    target.Store(word*4,source.Load(int3(id.xy,0)));
#else
    target[id.xy]=source.Load(word*4);
#endif
}
