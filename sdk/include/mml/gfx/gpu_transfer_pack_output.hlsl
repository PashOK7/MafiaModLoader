StructuredBuffer<float4> source:register(t0);
RWTexture2DArray<float4> target:register(u0);
cbuffer TransferDimensions:register(b0){uint width,height,layers;uint reserved;};
[numthreads(16,16,1)]
void CsTransferPackOutput(uint3 id:SV_DispatchThreadID){
    if(id.x>=width||id.y>=height)return;
    uint pixel=id.y*width+id.x;
    [loop]for(uint layer=0;layer<layers;layer++)target[uint3(id.xy,layer)]=source[pixel*layers+layer];
}
