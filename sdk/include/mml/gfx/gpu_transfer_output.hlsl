// Full-precision array / structured-buffer conversion for a shared GPU result.
Texture2DArray<float4> source:register(t0);
RWStructuredBuffer<float4> target:register(u0);
cbuffer TransferDimensions:register(b0){uint width,height,layers;uint reserved;};
[numthreads(16,16,1)]
void CsTransferOutput(uint3 id:SV_DispatchThreadID){
    if(id.x>=width||id.y>=height)return;
    uint pixel=id.y*width+id.x;
    [loop]for(uint layer=0;layer<layers;layer++)target[pixel*layers+layer]=source.Load(int4(id.xy,layer,0));
}
