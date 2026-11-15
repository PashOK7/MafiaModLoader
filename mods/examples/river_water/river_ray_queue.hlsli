// Work scheduling only. Keep all active optical rays and their per-pixel sample order.
RWStructuredBuffer<uint> g_waterRayQueue:register(u1);
RWByteAddressBuffer g_waterQueueCounts:register(u2);
RWByteAddressBuffer g_waterQueueArgs:register(u3);

[numthreads(1,1,1)]
void CsWaterQueueReset() {
    g_waterQueueCounts.Store3(0,uint3(0,0,0));
}

void WaterQueueAppend(uint queue,bool active,uint pixel) {
    uint begin=0,total=WaveActiveCountBits(active);
    if(WaveIsFirstLane()&&total)g_waterQueueCounts.InterlockedAdd(queue*4,total,begin);
    begin=WaveReadLaneFirst(begin);
    uint offset=begin+WavePrefixCountBits(active);
    if(active)g_waterRayQueue[queue*g_transportSize.x*g_transportSize.y+offset]=pixel;
}

[numthreads(8,8,1)]
void CsWaterQueueBuild(uint3 id:SV_DispatchThreadID) {
    bool up=false,down=false;
    uint pixel=id.y*g_transportSize.x+id.x;
    if(all(id.xy<g_transportSize)) {
        float4 centerB=g_waterDerivatives.Load(int4(id.xy,0,0));
        uint count=(uint)abs(centerB.w);
        if(g_transportSample<count) {
            float3 view=g_waterSeeds.Load(int4(id.xy,1,0)).xyz;
            float4 centerN=g_waterSeeds.Load(int4(id.xy,2,0)),centerT=g_waterSeeds.Load(int4(id.xy,3,0));
            WaterOpticalFrame optical;
            optical.n=centerN.xyz;optical.t=centerT.xyz;optical.b=centerB.xyz;
            optical.alpha=float2(centerN.w,centerT.w);
            float3 facet=count>1?WaterVisibleFacet(optical,view,WaterOpticalSample(g_transportSample,count)):optical.n;
            up=reflect(-view,facet).y>0;
            down=!up;
        } else if(g_transportSample==0) {
            g_waterTransportOut[pixel*3]=0;
            g_waterTransportOut[pixel*3+1]=0;
            g_waterTransportOut[pixel*3+2]=0;
        }
    }
    WaterQueueAppend(0,up,pixel);
    WaterQueueAppend(1,down,pixel);
}

[numthreads(1,1,1)]
void CsWaterQueueDispatch() {
    g_waterQueueArgs.Store3(0,uint3((g_waterQueueCounts.Load(0)+63)/64,1,1));
    g_waterQueueArgs.Store3(12,uint3((g_waterQueueCounts.Load(4)+63)/64,1,1));
}

[numthreads(1,1,1)]
void CsWaterQueueLayeredDispatch() {
    g_waterQueueArgs.Store3(24,uint3((g_waterQueueCounts.Load(8)+63)/64,1,1));
}

bool WaterQueuePixel(uint index,out uint3 id) {
    id=0;
    uint queue=g_transportResolve;
    if(index>=g_waterQueueCounts.Load(queue*4))return false;
    uint pixel=g_waterRayQueue[queue*g_transportSize.x*g_transportSize.y+index];
    id=uint3(pixel%g_transportSize.x,pixel/g_transportSize.x,0);
    return true;
}
