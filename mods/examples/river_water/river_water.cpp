// river_water: the game's own water replaced (geom_hide, geom_capture, scene_view, shader_shadows; docs/MODDING.md
// "The game's own geometry hidden or replaced"), drawn by the SDK's water: mml/water.h (the game's water's names, its
// planes, the ripples, the reflection's copy), mml/gfx/water.hlsli (the shading), mml/gfx/water_waves.hlsli (the
// ripples' kernels). knowledge/notes/water.md has the findings behind it.
//
// - The game's water (00TEST01..20) and the shade it lays on it under the bridges (the frames watercity.stin*) are
//   hidden from every drawing (the frame and the scene views);
// - the water's triangles are captured from what LS3D draws, every few seconds (a capture holds only what the frame
//   drew), kept by group and sorted into planes by their level (the river at -9.99, a fountain at 2.41);
// - the reflection is a scene view of the camera mirrored in the plane the screen showed most of (an occlusion query
//   a plane counts its pixels; none on the screen: no reflection drawn, the second drawing of the scene saved);
// - the wind's ripples are made each frame on the GPU (a JONSWAP spectrum, three cascades, an inverse FFT, the slopes'
//   mipmaps), only while water is on the screen;
// - a graphics pass draws the water on the captured triangles (river_water.hlsl): the reflection, the body lit and
//   shadowed from the visible sun by default (or the scene light, selected in settings), the sun scattered in the water
//   and its glitter towards the sun the sky shows (scene_sun_seen: its lens flare, elsewhere than that light).
// `river_water.status`; settings (mod.json: the game's Options > Mods): on, reflection, wind, wind_from, waves,
// clarity, depth, scattering, crests, glitter, foam; config.json also: texture, shade, fetch, colour, tint,
// phase, shadows, shadow_source (0 visible sun, 1 scene light), debug.
#include <windows.h>
#include <d3d11.h>
#include <d3d11_3.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <wrl/client.h>
#include "mml/mml.h"
#include "mml/water.h"
#include "mml/water_mesh.h"
#include "mml/environment.h"
#include "mml/environment_light.h"
#include "mml/scene_snapshot.h"
#include "mml/ray_scene_gpu.h"
#include "mml/ray_scene_reference.h"
#include "mml/native_texture_pages.h"
#include "mml/scene_radiance_gpu.h"
#include "mml/scene_waterline.h"
#include "mml/scene_waterline_gpu.h"
#include "mml/gpu_transfer.h"
#include "river_transport.h"
#include "shaders/VsWater.h"
#include "shaders/HsWater.h"
#include "shaders/DsWater.h"
#include "shaders/DsWaterWorld.h"
#include "shaders/DsWaterSeed.h"
#include "shaders/PsWater.h"
#include "shaders/PsWaterWorld.h"
#include "shaders/PsWaterResolve.h"
#include "shaders/PsWaterSeed.h"
#include "shaders/PsWaterSeedOrdered.h"
#include "shaders/CsWaterBoundsTiles.h"
#include "shaders/CsWaterBoundsHierarchy.h"
#include "shaders/CsWaterBoundsShore.h"
#include "shaders/CsWaterTransport.h"
#include "shaders/PsWaterGeometry.h"
#include "shaders/PsWaterRadiance.h"
#include "shaders/CsWaterWorldRay.h"
#include "shaders/CsWaterWorldBlend.h"
#include "shaders/CsPaintedSky.h"
#include "shaders/CsEnvironmentEmitter.h"
#include "shaders/CsSpectrum.h"
#include "shaders/CsFftRows.h"
#include "shaders/CsFftCols.h"
#include "shaders/CsResolve.h"
#include "shaders/CsTransferWords.h"
#include "shaders/CsTransferOutput.h"

namespace {
namespace mw = mml::water;
const MmlHost *H;
MmlMod *M;
constexpr float kWorldPathMetres=600;
// Keep the original margin around the total optical path and the 1 m waterline search.
constexpr float kWorldSourceRadius=kWorldPathMetres+8;
bool WorldMaterialMode(int debug){
    // Historical planar/painted/single-scatter comparisons are separate entries,
    // so the production shader need not compile their alternative transport.
    return debug!=10&&debug!=16&&debug!=21&&debug!=27&&!(debug>=22&&debug<=26);
}
int g_pass, g_view, g_hide, g_hideShade;
mml::environment::SkyCapture g_sky;
struct PreparedWaterline {
    std::vector<float> levels;
    std::shared_ptr<const mml::scene::WaterlineModel> model;
    using FacetRange=std::array<uint32_t,4>;
    std::shared_ptr<const std::vector<FacetRange>> facetRanges;
    uint64_t facetCycle=0,facetFrame=0;
    uint32_t triangles=0,billboards=0;
    bool prepareFacets=false;
    double milliseconds=0;
} g_preparedWaterline; // outlives g_scene and its pending worker
mml::scene::SnapshotBuilder g_scene;
mml::scene::RadianceGpu g_sceneGpu;
mml::scene::WaterlineGpu g_waterlineGpu;
std::shared_ptr<const mml::scene::WaterlineModel> g_waterline;
double g_waterlineBuildMs=0;
mml::ray::Bounds g_sceneBounds{{-2000,-30,-500},{-1000,240,700}};
bool g_scenePending=false;
float g_probeRay[12]={-1490,-9.992f,30,.02f,0,.2f,1,300,0,0,0,0};
struct RayResult {uint32_t status=3,instance=0,triangle=0,material=0;float distance=0,bary[2]{};uint32_t primitive=0;};
static_assert(sizeof(RayResult)==32,"MmlRayHit GPU layout");
RayResult g_rayResult;
struct WorldProbe {RayResult hit;float uv[4],diffuse[4];uint32_t attributes[4];float encoded[4],radiance[4],path[4],composite[4];};
static_assert(sizeof(WorldProbe)==144,"world probe GPU layout");
WorldProbe g_worldProbe{};bool g_attributesAgree=false;
uint64_t g_sceneCaptures=0,g_sceneFrame=0;unsigned g_sceneTriangles=0,g_sceneMissing=0;
uintptr_t g_hitVisual=0;bool g_sceneSuccess=false;
bool g_cpuAgrees=false;float g_cpuDistance=0;unsigned g_cpuStatus=3;
bool g_sceneMaterials=false,g_pagesReady=false;
bool g_keepSceneCache=false;
bool g_retainPacked=true,g_appendPackedGpu=false,g_deferPacked=true,g_stageMeshes=true;
mml::gfx::TransferProducer g_hardwareProducer;
mml::gfx::ImmutableBuffer g_hardwareRanges;
MmlChannel *g_hardwareRequest,*g_hardwareReply,*g_hardwareStatus;
bool g_hardwareRequested=false,g_hardwareLastUsed=false,g_hardwareFacets=false;
uint64_t g_hardwareWorker=0,g_hardwareUsed=0,g_hardwareFallback=0;
HRESULT g_hardwareError=S_OK;
double g_hardwareCpuMs=0;
double g_hardwareParts[6]{};
unsigned g_hardwareFailedBinding=0;
struct CacheObservation {unsigned meshBuilt=0,meshReused=0,textureCopied=0,textureReused=0;size_t meshBytes=0,textureBytes=0;uint64_t revision=0;HRESULT uploadError=S_OK;const char *uploadStage="none";
    unsigned gpuReused=0;size_t gpuBytes=0;double collectMs=0,copyMs=0,buildMs=0,textureMs=0,instancesMs=0,uploadMs=0;} g_probeCache;
double g_sceneUploadMs=0;
bool g_autoRay=false;uintptr_t g_sampleVisual=0;
bool g_sampleCutout=false;unsigned g_sampleSkip=0;
bool g_sampleBlend=false,g_sampleAdditive=false;
float g_sampleBary[2]={1.0f/3,1.0f/3},g_sampleRange=.1f;
mml::scene::SourceMaterial g_probeNativeMaterial{};
unsigned g_sampleRejected[6]{};unsigned g_sampleUnsupported=0;
unsigned g_textureCount=0,g_textureMissing=0;size_t g_textureBytes=0,g_pageBytes=0;
bool ArgumentTrue(const char *args,const char *key){
    const char *p=args?strstr(args,key):nullptr;p=p?strchr(p,':'):nullptr;if(!p)return false;
    do{p++;}while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r');return strncmp(p,"true",4)==0;
}
int PackingMode(const char *args,MmlReply *reply,void *){
    if(args&&strstr(args,"\"on\""))g_retainPacked=ArgumentTrue(args,"\"on\"");
    if(args&&strstr(args,"\"gpu\""))g_appendPackedGpu=ArgumentTrue(args,"\"gpu\"");
    if(args&&strstr(args,"\"defer\""))g_deferPacked=ArgumentTrue(args,"\"defer\"");
    if(args&&strstr(args,"\"geometry\""))g_stageMeshes=ArgumentTrue(args,"\"geometry\"");
    char result[112];snprintf(result,sizeof result,"{\"on\":%s,\"gpu\":%s,\"defer\":%s,\"geometry\":%s}",
        g_retainPacked?"true":"false",g_appendPackedGpu?"true":"false",g_deferPacked?"true":"false",g_stageMeshes?"true":"false");
    H->reply_append(reply,result,static_cast<size_t>(-1));return 0;
}
int SceneProbe(const char *args,MmlReply *reply,void *) {
    if(ArgumentTrue(args,"\"release\"")){g_scenePending=false;g_sceneMaterials=false;g_keepSceneCache=false;g_scene.Release();g_sceneGpu.Release();g_waterline.reset();g_waterlineGpu.Release();
        if(MML_HAS(H,scene_residency))H->scene_residency(M,nullptr,nullptr);}
    else if(ArgumentTrue(args,"\"capture\"")){
        auto nextBounds=g_sceneBounds;float nextRay[12];std::copy_n(g_probeRay,12,nextRay);
        float bary[2]={1.0f/3,1.0f/3},range=.1f;unsigned skip=0;
        if(const char *key=strstr(args,"\"box\"")){const char *a=strchr(key,'[');float b[6];
            if(!a||sscanf(a,"[ %f , %f , %f , %f , %f , %f ]",b,b+1,b+2,b+3,b+4,b+5)!=6)return 2;
            mml::ray::Bounds bounds{{b[0],b[1],b[2]},{b[3],b[4],b[5]}};if(!bounds.Valid())return 2;nextBounds=bounds;}
        if(const char *key=strstr(args,"\"ray\"")){const char *a=strchr(key,'[');float b[7];
            if(!a||sscanf(a,"[ %f , %f , %f , %f , %f , %f , %f ]",b,b+1,b+2,b+3,b+4,b+5,b+6)!=7)return 2;
            for(float x:b)if(!std::isfinite(x))return 2;
            const float length=std::sqrt(b[3]*b[3]+b[4]*b[4]+b[5]*b[5]);if(length<1e-6f||b[6]<=.02f)return 2;
            std::copy_n(b,3,nextRay);for(unsigned i=0;i<3;i++)nextRay[i+4]=b[i+3]/length;nextRay[7]=b[6];}
        if(const char *key=strstr(args,"\"bary\"")){const char *a=strchr(key,'[');if(!a||sscanf(a,"[ %f , %f ]",bary,bary+1)!=2||
            !std::isfinite(bary[0])||!std::isfinite(bary[1])||bary[0]<0||bary[1]<0||bary[0]+bary[1]>1)return 2;}
        if(const char *key=strstr(args,"\"range\"")){const char *a=strchr(key,':');if(!a||sscanf(a+1," %f",&range)!=1||!std::isfinite(range)||range<.1f||range>300)return 2;}
        if(const char *key=strstr(args,"\"skip\"")){const char *a=strchr(key,':');if(!a||sscanf(a+1," %u",&skip)!=1||skip>50000)return 2;}
        g_sceneBounds=nextBounds;std::copy_n(nextRay,12,g_probeRay);std::copy_n(bary,2,g_sampleBary);g_sampleRange=range;g_sampleSkip=skip;
        g_sampleCutout=ArgumentTrue(args,"\"sample_cutout\"");
        g_sampleBlend=ArgumentTrue(args,"\"sample_blend\"");g_sampleAdditive=ArgumentTrue(args,"\"sample_additive\"");
        g_sceneMaterials=ArgumentTrue(args,"\"materials\"")||ArgumentTrue(args,"\"trace_materials\"")||ArgumentTrue(args,"\"blend_layers\"");g_autoRay=ArgumentTrue(args,"\"sample\"");g_sampleVisual=0;
        if(const char *key=strstr(args,"\"visual\""))if(const char *pointer=strstr(key,"0x"))g_sampleVisual=static_cast<uintptr_t>(strtoull(pointer,nullptr,16));
        g_probeRay[3]=.02f;
        g_probeRay[8]=ArgumentTrue(args,"\"trace_materials\"")?1.0f:0.0f;
        g_probeRay[9]=ArgumentTrue(args,"\"blend_layers\"")?1.0f:0.0f;
        g_keepSceneCache=ArgumentTrue(args,"\"keep_cache\"");g_scenePending=true;
    }
    char out[900];snprintf(out,sizeof out,"{\"mode\":\"geometry diagnostic\",\"pending\":%s,\"success\":%s,\"captures\":%llu,\"source_frame\":%llu,\"triangles\":%u,\"missing\":%u,\"hit\":{\"status\":%u,\"distance\":%.6f,\"instance\":%u,\"primitive\":%u,\"material\":%u,\"bary\":[%.7f,%.7f],\"visual\":\"0x%08llX\"},\"cpu_reference\":{\"agrees\":%s,\"status\":%u,\"distance\":%.6f}}",
        g_scenePending?"true":"false",g_sceneSuccess?"true":"false",static_cast<unsigned long long>(g_sceneCaptures),static_cast<unsigned long long>(g_sceneFrame),g_sceneTriangles,g_sceneMissing,
        g_rayResult.status,g_rayResult.distance,g_rayResult.instance,g_rayResult.primitive,g_rayResult.material,g_rayResult.bary[0],g_rayResult.bary[1],static_cast<unsigned long long>(g_hitVisual),
        g_cpuAgrees?"true":"false",g_cpuStatus,g_cpuDistance);
    std::string result=out;result.pop_back();char pages[300];snprintf(pages,sizeof pages,",\"textures\":{\"ready\":%s,\"count\":%u,\"unavailable\":%u,\"native_bytes\":%zu,\"page_bytes\":%zu}}",
        g_pagesReady?"true":"false",g_textureCount,g_textureMissing,g_textureBytes,g_pageBytes);result+=pages;
    result.pop_back();char attributes[400];snprintf(attributes,sizeof attributes,",\"attributes\":{\"agrees\":%s,\"material\":%u,\"uv\":[%.7f,%.7f,%.7f,%.7f],\"diffuse\":[%.7f,%.7f,%.7f,%.7f]}}",
        g_attributesAgree?"true":"false",g_worldProbe.attributes[0],g_worldProbe.uv[0],g_worldProbe.uv[1],g_worldProbe.uv[2],g_worldProbe.uv[3],
        g_worldProbe.diffuse[0],g_worldProbe.diffuse[1],g_worldProbe.diffuse[2],g_worldProbe.diffuse[3]);result+=attributes;
    result.pop_back();char material[350];snprintf(material,sizeof material,",\"material\":{\"ready\":%s,\"encoded\":[%.7f,%.7f,%.7f,%.7f],\"linear\":[%.7f,%.7f,%.7f,%.7f]}}",
        g_worldProbe.attributes[2]?"true":"false",g_worldProbe.encoded[0],g_worldProbe.encoded[1],g_worldProbe.encoded[2],g_worldProbe.encoded[3],
        g_worldProbe.radiance[0],g_worldProbe.radiance[1],g_worldProbe.radiance[2],g_worldProbe.radiance[3]);result+=material;
    result.pop_back();char rejected[250];snprintf(rejected,sizeof rejected,",\"sample_rejected\":[%u,%u,%u,%u,%u,%u],\"unsupported_mask\":%u}",
        g_sampleRejected[0],g_sampleRejected[1],g_sampleRejected[2],g_sampleRejected[3],g_sampleRejected[4],g_sampleRejected[5],g_sampleUnsupported);result+=rejected;
    result.pop_back();char native[550];snprintf(native,sizeof native,",\"ray\":[%.7f,%.7f,%.7f,%.7f,%.7f,%.7f,%.7f],\"native\":{\"alpha_test\":%u,\"reference\":%u,\"function\":%u,\"blend\":%u,\"cull\":%u,\"source_blend\":%u,\"destination_blend\":%u}}",
        g_probeRay[0],g_probeRay[1],g_probeRay[2],g_probeRay[4],g_probeRay[5],g_probeRay[6],g_probeRay[7],
        g_probeNativeMaterial.alphaTest,g_probeNativeMaterial.alphaReference,g_probeNativeMaterial.alphaFunction,g_probeNativeMaterial.blend,g_probeNativeMaterial.cull,
        g_probeNativeMaterial.sourceBlend,g_probeNativeMaterial.destinationBlend);result+=native;
    result.pop_back();char path[300];snprintf(path,sizeof path,",\"path\":{\"status\":%u,\"layers\":%u,\"transmission\":%.7f,\"radiance\":[%.7f,%.7f,%.7f]}}",
        unsigned(g_worldProbe.path[0]),unsigned(g_worldProbe.path[1]),g_worldProbe.path[2],g_worldProbe.composite[0],g_worldProbe.composite[1],g_worldProbe.composite[2]);result+=path;
    result.pop_back();char cache[900];snprintf(cache,sizeof cache,",\"cache\":{\"mesh_built\":%u,\"mesh_reused\":%u,\"texture_copied\":%u,\"texture_reused\":%u,\"mesh_bytes\":%zu,\"texture_bytes\":%zu,\"hit_revision\":%llu},\"publication\":{\"stage\":\"%s\",\"error\":\"%08X\",\"gpu_reused\":%u,\"gpu_uploaded_bytes\":%zu},\"cost_ms\":{\"native_collect_inclusive\":%.3f,\"source_copy\":%.3f,\"mesh_build\":%.3f,\"texture_verify_copy\":%.3f,\"instances\":%.3f,\"upload\":%.3f}}",
        g_probeCache.meshBuilt,g_probeCache.meshReused,g_probeCache.textureCopied,g_probeCache.textureReused,g_probeCache.meshBytes,g_probeCache.textureBytes,static_cast<unsigned long long>(g_probeCache.revision),g_probeCache.uploadStage,unsigned(g_probeCache.uploadError),g_probeCache.gpuReused,g_probeCache.gpuBytes,
        g_probeCache.collectMs,g_probeCache.copyMs,g_probeCache.buildMs,g_probeCache.textureMs,g_probeCache.instancesMs,g_probeCache.uploadMs);result+=cache;
    H->reply_append(reply,result.c_str(),static_cast<size_t>(-1));return 0;
}

constexpr int kMaxPlanes = 16;
constexpr int kRing = 4;   // occlusion queries in flight a plane
constexpr unsigned kTransportSamples=river::TransportSamples;
constexpr unsigned kSurfaceEndStamp=4;

struct Settings {
    bool on = true, reflection = true, shadows = true;
    int shadowSource = 0;       // 0: visible sky sun (default); 1: scene's directional light
    float shadowSoftness = 1;   // solar angular radius multiplier; penumbra grows with blocker/receiver separation
    bool geometry = true;
    float waveHeight = 6.5f;
    float smallRipples = .65f;
    float reflectionSoftness = .25f; // artistic unresolved-facet slope deviation; not wave displacement
    float reflectionStrength = 1;
    float waveWidth = 4.75f, waveSpeed = 1;
    std::string texture = mw::kGameWater, shade = mw::kGameShade;
    float wind = 5, windFrom = 210, fetch = 800, waves = 1, clarity = 2, depth = 10, glitter = .2f, foam = 1.1f;
    float scattering = 0.4f, phase = 0.75f, crests = 1, chop = 1, sunRadius = 18;
    int debug = 0;              // the shader's debug view (river_water.hlsl g_light.z)
    float colour[3], tint[3];   // linear: the light the body scatters of white light; white light after `clarity` m
} S;

mw::Surfaces g_water;
std::vector<float> g_patches;
std::vector<mw::Plane> g_patchPlanes;
unsigned long long g_pixels[kMaxPlanes];   // each plane's pixels at its last count (~0: not counted yet)
unsigned g_captures;
DWORD g_nextAsk;
bool g_asked;
int g_reflPlane = -1;                      // the plane the reflection was placed for this frame
float g_reflLevel;

mw::Spectrum g_spectrum;
unsigned g_specVersion = 1;

struct Gpu {
    ID3D11VertexShader *vs;
    ID3D11HullShader *hs;
    ID3D11DomainShader *ds;
    ID3D11DomainShader *worldDs, *seedDs;
    ID3D11PixelShader *ps;
    ID3D11PixelShader *worldPs;
    ID3D11PixelShader *resolvePs;
    ID3D11PixelShader *seedPs, *seedOrderedPs;
    ID3D11ComputeShader *waveBoundsTiles,*waveBoundsHierarchy,*waveBoundsShore;
    ID3D11ShaderResourceView *waveBoundsView;
    ID3D11UnorderedAccessView *waveBoundsWrite;
    bool waveBoundsActive;
    ID3D11ComputeShader *transportCs;
    ID3D11PixelShader *geometryPs;
    ID3D11PixelShader *radiancePs;
    ID3D11ComputeShader *rayProbe;
    ID3D11ComputeShader *blendProbe;
    ID3D11ComputeShader *paintedSky;
    ID3D11Texture2D *skyTexture;
    ID3D11ShaderResourceView *skyView;
    ID3D11UnorderedAccessView *skyOut;
    unsigned skyW, skyH;
    bool skyReady;
    ID3D11InputLayout *layout;
    ID3D11Buffer *cb, *vb;
    ID3D11RasterizerState *rs, *wire;
    ID3D11Texture2D *depth;
    ID3D11DepthStencilView *dsv;
    ID3D11DepthStencilState *depthState;
    ID3D11ShaderResourceView *seedViews[3], *transportView;
    ID3D11RenderTargetView *seedTargets[12];
    ID3D11UnorderedAccessView *orderedSeeds[4];
    ID3D11UnorderedAccessView *resolveOutput;
    ID3D11ShaderResourceView *resolveSurface, *resolveDepth;
    bool resolveTried, resolveActive;
    ID3D11UnorderedAccessView *transportOutput;
    ID3D11Buffer *transportConstants;
    unsigned depthW, depthH;
    ID3D11Query *stats[kRing];
    bool statsIssued[kRing];
    D3D11_QUERY_DATA_PIPELINE_STATISTICS geometryStats;
    ID3D11Query *disjoint[kRing], *stamp[kRing][kSurfaceEndStamp+1];
    bool timingIssued[kRing], worldTiming[kRing];
    unsigned timingFrame[kRing], latestTiming;
    float fftMs, surfaceMs, seedMs, traceMs, composeMs;
    ID3D11SamplerState *wrap, *clamp;
    ID3D11Query *query[kRing][kMaxPlanes];
    bool issued[kRing][kMaxPlanes];
    unsigned queryEpoch[kRing][kMaxPlanes];
    unsigned madeFor, vbVersion, frame;
    bool made;
} G;
mw::Waves g_waves;
mw::ReflectionCopy g_refl;
mml::environment::ImageEmitter g_emitter;
unsigned g_drawn, g_reflDrawn;
unsigned g_skyDrawn;
unsigned g_offscreen, g_reflectionRejected;
float g_visibilityPose[16];
bool g_haveVisibilityPose;
unsigned g_visibilityEpoch;

bool VisibleWaterPlane(const mw::Plane &plane, const float vp[16], float projX, float projY);
void WaterViewProjection(const MmlViewCamera &camera, float vp[16]);
float g_rview[16], g_rproj[16];   // the last reflection's (status)
LARGE_INTEGER g_t0, g_freq;
mw::WaveClock g_waveClock;
bool g_captureClock=false;
bool g_resolveRequested=true;
bool g_waveBoundsRequested=true;
float g_captureSeconds=0;
int CaptureClock(const char *args,MmlReply *reply,void *) {
    if(args&&strstr(args,"\"release\"")&&strstr(args,"true"))g_captureClock=false;
    else if(args)if(const char *key=strstr(args,"\"seconds\"")){
        const char *colon=strchr(key,':');char *end=nullptr;
        const double seconds=colon ? std::strtod(colon+1,&end) : -1;
        if(!colon||end==colon+1||!std::isfinite(seconds)||seconds<0||seconds>1e6)return 2;
        g_captureSeconds=static_cast<float>(seconds);g_captureClock=true;
    }
    char out[120];snprintf(out,sizeof out,"{\"held\":%s,\"seconds\":%.6f}",g_captureClock?"true":"false",g_captureSeconds);
    H->reply_append(reply,out,static_cast<size_t>(-1));return 0;
}
int ResolveMode(const char *args,MmlReply *reply,void *) {
    if(args&&strstr(args,"\"on\""))g_resolveRequested=ArgumentTrue(args,"\"on\"");
    if(args&&strstr(args,"\"wave_bounds\""))g_waveBoundsRequested=ArgumentTrue(args,"\"wave_bounds\"");
    char out[160];snprintf(out,sizeof out,"{\"on\":%s,\"active\":%s,\"wave_bounds\":%s,\"wave_bounds_active\":%s}",
        g_resolveRequested?"true":"false",G.resolveActive?"true":"false",g_waveBoundsRequested?"true":"false",G.waveBoundsActive?"true":"false");
    H->reply_append(reply,out,static_cast<size_t>(-1));return 0;
}
double ClockSeconds() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart - g_t0.QuadPart) / g_freq.QuadPart;
}
float g_previousVP[16], g_previousEye[4];
bool g_haveHistory;
unsigned g_temporalDrawn;

using mw::SafeRelease;

void ReleaseTransport() {
    for(auto &view:G.seedViews)SafeRelease(view);
    for(auto &target:G.seedTargets)SafeRelease(target);
    for(auto &output:G.orderedSeeds)SafeRelease(output);
    SafeRelease(G.resolveOutput);SafeRelease(G.resolveSurface);SafeRelease(G.resolveDepth);
    G.resolveTried=G.resolveActive=false;
    SafeRelease(G.transportView);
    SafeRelease(G.transportOutput);
}

void Release() {
    g_hardwareProducer.Release();g_hardwareWorker=0;
    g_waterline.reset();
    g_waterlineGpu.Release();SafeRelease(G.worldDs);SafeRelease(G.seedDs);
    g_sceneGpu.Release();SafeRelease(G.worldPs);SafeRelease(G.resolvePs);SafeRelease(G.geometryPs);SafeRelease(G.radiancePs);
    SafeRelease(G.rayProbe);SafeRelease(G.blendProbe);
    SafeRelease(G.seedPs);SafeRelease(G.seedOrderedPs);SafeRelease(G.transportCs);SafeRelease(G.transportConstants);ReleaseTransport();
    SafeRelease(G.waveBoundsTiles);SafeRelease(G.waveBoundsHierarchy);SafeRelease(G.waveBoundsShore);
    SafeRelease(G.waveBoundsView);SafeRelease(G.waveBoundsWrite);G.waveBoundsActive=false;
    g_emitter.Release();
    SafeRelease(G.paintedSky); SafeRelease(G.skyOut); SafeRelease(G.skyView); SafeRelease(G.skyTexture);
    G.skyW = G.skyH = 0; G.skyReady = false;
    g_haveHistory = false;
    SafeRelease(G.vs); SafeRelease(G.hs); SafeRelease(G.ds); SafeRelease(G.ps);
    SafeRelease(G.layout); SafeRelease(G.cb); SafeRelease(G.vb);
    SafeRelease(G.rs); SafeRelease(G.wire); SafeRelease(G.wrap); SafeRelease(G.clamp);
    SafeRelease(G.dsv); SafeRelease(G.depth); SafeRelease(G.depthState);
    G.depthW = G.depthH = 0;
    for (auto &q : G.stats) SafeRelease(q);
    for (auto &q : G.disjoint) SafeRelease(q);
    for (auto &row : G.stamp) for (auto &q : row) SafeRelease(q);
    memset(G.timingIssued, 0, sizeof G.timingIssued);
    G.latestTiming=0;
    memset(G.statsIssued, 0, sizeof G.statsIssued);
    for (auto &ring : G.query)
        for (auto &q : ring) SafeRelease(q);
    memset(G.issued, 0, sizeof G.issued);
    G.made = false;
    G.vbVersion = 0;
    g_waves.Release();
    g_refl.Release();
}

float Lin(float c) { return std::pow(c, 2.2f); }

// "#rrggbb" into linear light
void Colour(const char *key, const char *def, float out[3]) {
    const char *c = H->config_string(M, key, def);
    if (*c == '#') c++;
    unsigned v = static_cast<unsigned>(strtoul(c, nullptr, 16));
    out[0] = Lin(((v >> 16) & 255) / 255.0f);
    out[1] = Lin(((v >> 8) & 255) / 255.0f);
    out[2] = Lin((v & 255) / 255.0f);
}

bool g_shadowBridgeFailed=false;
void UpdateShadowRequest();
void Forget() {
    g_haveHistory = false;
    g_haveVisibilityPose = false;
    g_water.Clear();
    for (auto &p : g_pixels) p = ~0ull;
    g_asked = false;
    g_nextAsk = 0;
    UpdateShadowRequest();
}

// The game's water shown (and its shade) or hidden from every drawing.
void ShowWater(bool show) {
    if (show && g_hide) { H->geom_show(M, g_hide); g_hide = 0; }
    if (show && g_hideShade) { H->geom_show(M, g_hideShade); g_hideShade = 0; }
    if (!show && !g_hide) {
        g_hide = H->geom_hide(M, S.texture.c_str(), MML_GEOM_FRAME | MML_GEOM_VIEWS);
        if (!S.shade.empty()) g_hideShade = H->geom_hide(M, S.shade.c_str(), MML_GEOM_FRAME | MML_GEOM_VIEWS);
        Forget();
    }
}

void UpdateShadowRequest() {
    if (!MML_HAS(H, shader_shadows)) return;
    MmlShadowSettings sh{2048, 120.0f, 0.0f};
    const auto *settings = S.on && S.shadows && !g_shadowBridgeFailed ? &sh : nullptr;
    const int source = S.shadowSource == 0 ? MML_SHADOW_SUN : MML_SHADOW_SCENE;
    if (MML_HAS(H, shader_shadows_fit) && !g_water.planes.empty()) {
        float minY = g_water.planes[0].level, maxY = minY;
        for (const auto &p : g_water.planes) { minY = std::min(minY, p.level); maxY = std::max(maxY, p.level); }
        const float bound = std::max(0.05f, 6 * std::sqrt(g_spectrum.heightVar) * std::fabs(S.waves) * (1 + S.chop));
        if (MML_HAS(H, shader_shadows_surface))
            H->shader_shadows_surface(M, settings, source, g_water.tris.data(), static_cast<unsigned>(g_water.tris.size()/9), bound);
        else H->shader_shadows_fit(M, settings, source, minY - bound, maxY + bound);
    } else if (MML_HAS(H, shader_shadows_from)) H->shader_shadows_from(M, settings, source);
    else H->shader_shadows(M, settings);
}

void ReadSettings() {
    g_haveHistory = false;   // height/material changes are not the preceding surface
    g_haveVisibilityPose = false; // changed displacement can expose water hidden from the preceding queries
    g_waveClock.Advance(ClockSeconds(), S.waveSpeed);
    float wind = S.wind, from = S.windFrom, fetch = S.fetch, smallRipples = S.smallRipples, width = S.waveWidth, height = S.waveHeight;
    std::string tex = H->config_string(M, "texture", mw::kGameWater);
    std::string shade = H->config_string(M, "shade", mw::kGameShade);
    S.on = H->config_number(M, "on", 1) != 0;
    S.geometry = H->config_number(M, "geometry", 1) != 0;
    S.waveHeight = static_cast<float>(H->config_number(M, "wave_height", 6.5));
    if (!(S.waveHeight >= 0)) S.waveHeight = 0;
    if (S.waveHeight > 8) S.waveHeight = 8;
    S.reflection = H->config_number(M, "reflection", 1) != 0;
    S.shadows = H->config_number(M, "shadows", 1) != 0;
    S.shadowSource = H->config_number(M, "shadow_source", 0) == 1 ? 1 : 0;
    S.shadowSoftness = static_cast<float>(H->config_number(M, "shadow_softness", 1));
    if (!(S.shadowSoftness >= 0)) S.shadowSoftness = 0;
    if (S.shadowSoftness > 8) S.shadowSoftness = 8;
    S.wind = static_cast<float>(H->config_number(M, "wind", 5));
    S.windFrom = static_cast<float>(H->config_number(M, "wind_from", 210));
    S.fetch = static_cast<float>(H->config_number(M, "fetch", 800));
    S.waveWidth = static_cast<float>(H->config_number(M, "wave_width", 4.75));
    if (!std::isfinite(S.waveWidth)) S.waveWidth = 4.75f;
    S.waveWidth = std::clamp(S.waveWidth, 0.25f, 8.0f);
    S.waveSpeed = static_cast<float>(H->config_number(M, "wave_speed", 1));
    if (!std::isfinite(S.waveSpeed)) S.waveSpeed = 1;
    S.waveSpeed = std::clamp(S.waveSpeed, 0.0f, 4.0f);
    S.waves = static_cast<float>(H->config_number(M, "waves", 1));
    S.smallRipples = static_cast<float>(H->config_number(M, "small_ripples", .65));
    if (!std::isfinite(S.smallRipples)) S.smallRipples = .65f;
    S.smallRipples = std::clamp(S.smallRipples, 0.0f, 2.0f);
    S.reflectionSoftness = static_cast<float>(H->config_number(M, "reflection_softness", .25));
    if (!std::isfinite(S.reflectionSoftness)) S.reflectionSoftness = .25f;
    S.reflectionSoftness = std::clamp(S.reflectionSoftness, 0.0f, 2.0f);
    S.reflectionStrength = static_cast<float>(H->config_number(M, "reflection_strength", 1));
    if (!std::isfinite(S.reflectionStrength)) S.reflectionStrength = 1;
    S.reflectionStrength = std::clamp(S.reflectionStrength, 0.0f, 1.0f);
    S.clarity = static_cast<float>(H->config_number(M, "clarity", 2));
    S.depth = static_cast<float>(H->config_number(M, "depth", 10));
    if (!(S.depth >= 0.1f)) S.depth = 0.1f;
    if (S.depth > 1000) S.depth = 1000;
    S.glitter = static_cast<float>(H->config_number(M, "glitter", .2));
    S.sunRadius = static_cast<float>(H->config_number(M, "sun_radius", 18));
    if (!(S.sunRadius >= 0.5f)) S.sunRadius = 0.5f;
    if (S.sunRadius > 60) S.sunRadius = 60;
    S.foam = std::clamp(static_cast<float>(H->config_number(M, "foam", 1.1)), 0.0f, 2.0f);
    S.scattering = static_cast<float>(H->config_number(M, "scattering", 0.4));
    S.phase = static_cast<float>(H->config_number(M, "phase", 0.75));
    if (S.phase < 0) S.phase = 0;
    if (S.phase > 0.95f) S.phase = 0.95f;
    S.crests = static_cast<float>(H->config_number(M, "crests", 1));
    S.chop = static_cast<float>(H->config_number(M, "chop", 1));
    S.debug = static_cast<int>(H->config_number(M, "debug", 0));
    Colour("colour", "#343531", S.colour);
    Colour("tint", "#6e9c80", S.tint);
    if (wind != S.wind || from != S.windFrom || fetch != S.fetch || smallRipples != S.smallRipples || width != S.waveWidth || height != S.waveHeight || g_spectrum.h0.empty()) {
        g_spectrum = mw::MakeSpectrum(S.wind, S.windFrom, S.fetch, 0.0012f, 4, 0.0043f, S.smallRipples, 0.25f, false,
                                     {S.waveWidth,S.waveHeight});
        g_specVersion++;
    }
    if (tex != S.texture || shade != S.shade) { ShowWater(true); S.texture = tex; S.shade = shade; }
    ShowWater(!S.on);
    UpdateShadowRequest();
}

void OnCapture(const MmlGeomCapture *c, void *) {
    g_asked = false;
    g_captures++;
    bool changed = false;
    for (unsigned i = 0; i < c->group_count; i++) {
        const MmlGeomGroup &gr = c->groups[i];
        changed = g_water.Add(gr.visual, gr.group, c->tris + static_cast<size_t>(gr.first) * 9, gr.count) || changed;
    }
    if (!changed) return;
    g_water.Sort();
    UpdateShadowRequest();
    for (auto &p : g_pixels) p = ~0ull;   // (the planes changed: their counts start again)
}

void Ask() {
    if (g_asked || !g_hide) return;
    g_asked = H->geom_capture(M, S.texture.c_str(), OnCapture, nullptr) != 0;
    g_nextAsk = GetTickCount() + (g_water.planes.empty() ? 1000 : 3000);
}

// The reflection: the camera mirrored in the plane the screen showed most of at the last count (none on the screen:
// not drawn). Only what is above the water is drawn (the sky is not cut: the loader).
int PlaceReflection(MmlViewCamera *c, void *) {
    g_reflPlane = -1;
    if (!S.on) return 0;
    float vp[16]; WaterViewProjection(*c, vp);
    bool moved = !g_haveVisibilityPose || memcmp(g_visibilityPose, c->world, sizeof g_visibilityPose) != 0;
    memcpy(g_visibilityPose, c->world, sizeof g_visibilityPose); g_haveVisibilityPose = true;
    if (moved) { ++g_visibilityEpoch; for (auto &pixels : g_pixels) pixels = ~0ull; }
    if (!S.reflection || WorldMaterialMode(S.debug)) return 0;
    int best = -1;
    unsigned long long most = 0;
    for (size_t p = 0; p < g_water.planes.size(); p++) {
        if (c->world[13] < g_water.planes[p].level + 0.05f ||
            !VisibleWaterPlane(g_water.planes[p], vp, 2 / (c->tan_left + c->tan_right), 2 / (c->tan_up + c->tan_down))) continue;
        // Old zero samples are not evidence about a new camera. The conservative geometry test permits recovery
        // immediately, while an unchanged view can still use the asynchronous occlusion result.
        if (!moved && !g_pixels[p]) continue;
        auto pixels = std::max<unsigned long long>(g_pixels[p], moved ? 1 : 0);
        if (best < 0 || pixels > most) best = static_cast<int>(p), most = pixels;
    }
    if (best < 0) { g_reflectionRejected++; return 0; }
    g_reflLevel = g_water.planes[best].level;
    mw::MirrorCamera(c->world, c->clip, g_reflLevel);
    c->flags |= MML_VIEW_CLIP;
    g_reflPlane = best;
    g_reflDrawn++;
    return 1;
}

void Mul44(const float *a, const float *b, float *out) {   // out = a * b (row vectors)
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}

// out = m^-1 (4x4, Gauss-Jordan with partial pivoting); false: singular
bool Inverse44(const float *m, float *out) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        for (int j = 0; j < 8; j++) std::swap(a[c][j], a[p][j]);
        double inv = 1 / a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = static_cast<float>(a[i][j + 4]);
    return true;
}

void WaterViewProjection(const MmlViewCamera &c, float vp[16]) {
    const float *w = c.world;
    float view[16] = {w[0],w[4],w[8],0, w[1],w[5],w[9],0, w[2],w[6],w[10],0,
        -(w[12]*w[0]+w[13]*w[1]+w[14]*w[2]), -(w[12]*w[4]+w[13]*w[5]+w[14]*w[6]),
        -(w[12]*w[8]+w[13]*w[9]+w[14]*w[10]), 1};
    float lr = c.tan_left + c.tan_right, ud = c.tan_up + c.tan_down;
    float z = c.far_z / (c.far_z - c.near_z);
    float proj[16] = {2/lr,0,0,0, 0,2/ud,0,0, (c.tan_left-c.tan_right)/lr,(c.tan_down-c.tan_up)/ud,z,1,
        0,0,-c.near_z*z,0};
    Mul44(view, proj, vp);
}

// The same expanded homogeneous side/eye tests as WaterFactors, applied to the original triangles. Every clipped
// 16 m patch is inside one of them, so rejecting an entire original triangle cannot remove a surviving patch.
// No far-plane test: the GPU path deliberately keeps that case conservative too.
bool VisibleWaterPlane(const mw::Plane &plane, const float vp[16], float projX, float projY) {
    float strength = S.wind >= 0.1f ? std::fabs(S.waves) : 0;
    float bound = 6 * std::sqrt(g_spectrum.heightVar) * strength * (1 + S.chop);
    float pad = std::max(bound, 0.1f) * std::max(projX, projY) * 2 + 0.01f;
    for (unsigned n = 0; n < plane.count; n++) {
        bool outside[5] = {true,true,true,true,true};
        const float *t = g_water.tris.data() + size_t(plane.first + n) * 9;
        for (int p = 0; p < 3; p++) {
            float x=t[p*3], y=t[p*3+1], z=t[p*3+2];
            float cx=x*vp[0]+y*vp[4]+z*vp[8]+vp[12], cy=x*vp[1]+y*vp[5]+z*vp[9]+vp[13];
            float cw=x*vp[3]+y*vp[7]+z*vp[11]+vp[15];
            const float d[5] = {cw,cx+cw,cw-cx,cy+cw,cw-cy};
            for (int side=0; side<5; side++) outside[side] = outside[side] && d[side] < -pad;
        }
        if (!outside[0] && !outside[1] && !outside[2] && !outside[3] && !outside[4]) return true;
    }
    return false;
}

bool Ok(HRESULT hr, const char *what) {
    if (SUCCEEDED(hr)) return true;
    H->log(M, MML_LOG_ERROR, "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return false;
}

bool MakeTransport(ID3D11Device *device,unsigned width,unsigned height,bool ordered) {
    using Microsoft::WRL::ComPtr;
    if(G.transportView)return true;
    D3D11_TEXTURE2D_DESC desc{};
    G.depth->GetDesc(&desc);
    desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.ArraySize=4;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    if(ordered)desc.BindFlags|=D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11ShaderResourceView> views[std::size(G.seedViews)];
    ComPtr<ID3D11RenderTargetView> targets[std::size(G.seedTargets)];
    ComPtr<ID3D11UnorderedAccessView> orderedViews[std::size(G.orderedSeeds)];
    // Same twelve full-precision targets, with smaller independent allocations.
    for(UINT group=0;group<std::size(views);group++){
        ComPtr<ID3D11Texture2D> texture;
        if(!Ok(device->CreateTexture2D(&desc,nullptr,&texture),"water transport texture")||
           !Ok(device->CreateShaderResourceView(texture.Get(),nullptr,&views[group]),"water transport read view"))return false;
        if(ordered&&!Ok(device->CreateUnorderedAccessView(texture.Get(),nullptr,&orderedViews[group]),"water ordered seed view"))return false;
        for(UINT slice=0;slice<desc.ArraySize;slice++){
            if(ordered&&(group!=1||slice!=0))continue; // coverage clear only
            D3D11_RENDER_TARGET_VIEW_DESC rt{};
            rt.Format=desc.Format;
            rt.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            rt.Texture2DArray.FirstArraySlice=slice;
            rt.Texture2DArray.ArraySize=1;
            if(!Ok(device->CreateRenderTargetView(texture.Get(),&rt,&targets[group*desc.ArraySize+slice]),"water transport render target"))return false;
        }
    }
    if(ordered){
        desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_R32_FLOAT;
        desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> depth;
        if(!Ok(device->CreateTexture2D(&desc,nullptr,&depth),"water ordered seed depth")||
           !Ok(device->CreateUnorderedAccessView(depth.Get(),nullptr,&orderedViews[3]),"water ordered depth view"))return false;
    }
    const uint64_t bytes=uint64_t(width)*height*3*4*sizeof(float);
    if(!bytes||bytes>UINT_MAX)return false;
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth=static_cast<UINT>(bytes);
    bufferDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride=4*sizeof(float);
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> resultView;
    ComPtr<ID3D11UnorderedAccessView> resultOutput;
    if(!Ok(device->CreateBuffer(&bufferDesc,nullptr,&buffer),"water reflection buffer")||
       !Ok(device->CreateShaderResourceView(buffer.Get(),nullptr,&resultView),"water reflection read view")||
       !Ok(device->CreateUnorderedAccessView(buffer.Get(),nullptr,&resultOutput),"water reflection write view"))return false;
    G.transportView=resultView.Detach();G.transportOutput=resultOutput.Detach();
    for(size_t group=0;group<std::size(views);group++)G.seedViews[group]=views[group].Detach();
    for(size_t slice=0;slice<std::size(targets);slice++)G.seedTargets[slice]=targets[slice].Detach();
    for(size_t view=0;view<std::size(orderedViews);view++)G.orderedSeeds[view]=orderedViews[view].Detach();
    return true;
}

bool MakeResolve(ID3D11Device *device) {
    using Microsoft::WRL::ComPtr;
    if(G.resolveTried)return G.resolveOutput!=nullptr;
    G.resolveTried=true;
    ComPtr<ID3D11Resource> depth;
    G.orderedSeeds[3]->GetResource(&depth);
    D3D11_TEXTURE2D_DESC desc{};
    G.depth->GetDesc(&desc);
    desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> surface;
    ComPtr<ID3D11ShaderResourceView> depthView,surfaceView;
    ComPtr<ID3D11UnorderedAccessView> surfaceOutput;
    if(!Ok(device->CreateShaderResourceView(depth.Get(),nullptr,&depthView),"water resolve depth view")||
       !Ok(device->CreateTexture2D(&desc,nullptr,&surface),"water resolve surface")||
       !Ok(device->CreateShaderResourceView(surface.Get(),nullptr,&surfaceView),"water resolve surface view")||
       !Ok(device->CreateUnorderedAccessView(surface.Get(),nullptr,&surfaceOutput),"water resolve surface output"))return false;
    G.resolveDepth=depthView.Detach();G.resolveSurface=surfaceView.Detach();G.resolveOutput=surfaceOutput.Detach();
    return true;
}

void MakeWaveBounds(ID3D11Device *device) {
    using Microsoft::WRL::ComPtr;
    // Three absolute displacements per cascade, shore uncertainty tiles, and
    // the six min/max levels for each current 256-square displacement layer.
    constexpr UINT entries=9+33*33+3*2*1365;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth=entries*sizeof(uint32_t);desc.StructureByteStride=sizeof(uint32_t);
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11UnorderedAccessView> output;
    ComPtr<ID3D11ComputeShader> tiles,hierarchy,shore;
    if(!Ok(device->CreateBuffer(&desc,nullptr,&buffer),"water bounds buffer")||
       !Ok(device->CreateShaderResourceView(buffer.Get(),nullptr,&view),"water bounds view")||
       !Ok(device->CreateUnorderedAccessView(buffer.Get(),nullptr,&output),"water bounds output")||
       !Ok(device->CreateComputeShader(kCsWaterBoundsTiles,sizeof kCsWaterBoundsTiles,nullptr,&tiles),"water bounds tiles")||
       !Ok(device->CreateComputeShader(kCsWaterBoundsHierarchy,sizeof kCsWaterBoundsHierarchy,nullptr,&hierarchy),"water bounds hierarchy")||
       !Ok(device->CreateComputeShader(kCsWaterBoundsShore,sizeof kCsWaterBoundsShore,nullptr,&shore),"water bounds shore"))return;
    G.waveBoundsView=view.Detach();G.waveBoundsWrite=output.Detach();
    G.waveBoundsTiles=tiles.Detach();G.waveBoundsHierarchy=hierarchy.Detach();G.waveBoundsShore=shore.Detach();
}

bool Make(ID3D11Device *d) {
    Release();
    D3D11_INPUT_ELEMENT_DESC el{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 480;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;   // (the captured triangles' winding is the game's)
    rd.DepthClipEnable = TRUE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = 16;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_SAMPLER_DESC cd = sd;
    cd.AddressU = cd.AddressV = cd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    bool ok = Ok(d->CreateVertexShader(kVsWater, sizeof kVsWater, nullptr, &G.vs), "VsWater") &&
              Ok(d->CreateHullShader(kHsWater, sizeof kHsWater, nullptr, &G.hs), "HsWater") &&
              Ok(d->CreateDomainShader(kDsWater, sizeof kDsWater, nullptr, &G.ds), "DsWater") &&
              Ok(d->CreatePixelShader(kPsWater, sizeof kPsWater, nullptr, &G.ps), "PsWater") &&
              Ok(d->CreatePixelShader(kPsWaterWorld,sizeof kPsWaterWorld,nullptr,&G.worldPs),"PsWaterWorld") &&
              Ok(d->CreatePixelShader(kPsWaterSeed,sizeof kPsWaterSeed,nullptr,&G.seedPs),"PsWaterSeed") &&
              Ok(d->CreateComputeShader(kCsWaterTransport,sizeof kCsWaterTransport,nullptr,&G.transportCs),"CsWaterTransport") &&
              Ok(d->CreateDomainShader(kDsWaterWorld,sizeof kDsWaterWorld,nullptr,&G.worldDs),"DsWaterWorld") &&
              Ok(d->CreateDomainShader(kDsWaterSeed,sizeof kDsWaterSeed,nullptr,&G.seedDs),"DsWaterSeed") &&
              Ok(d->CreatePixelShader(kPsWaterGeometry,sizeof kPsWaterGeometry,nullptr,&G.geometryPs),"PsWaterGeometry") &&
              Ok(d->CreatePixelShader(kPsWaterRadiance,sizeof kPsWaterRadiance,nullptr,&G.radiancePs),"PsWaterRadiance") &&
              Ok(d->CreateComputeShader(kCsWaterWorldRay,sizeof kCsWaterWorldRay,nullptr,&G.rayProbe),"CsWaterWorldRay") &&
              Ok(d->CreateComputeShader(kCsWaterWorldBlend,sizeof kCsWaterWorldBlend,nullptr,&G.blendProbe),"CsWaterWorldBlend") &&
              Ok(d->CreateInputLayout(&el, 1, kVsWater, sizeof kVsWater, &G.layout), "the layout") &&
              Ok(d->CreateBuffer(&bd, nullptr, &G.cb), "the constants") &&
              Ok(d->CreateRasterizerState(&rd, &G.rs), "the rasterizer state") &&
              Ok(d->CreateSamplerState(&sd, &G.wrap), "a sampler") && Ok(d->CreateSamplerState(&cd, &G.clamp), "a sampler") &&
              Ok(d->CreateComputeShader(kCsPaintedSky, sizeof kCsPaintedSky, nullptr, &G.paintedSky), "the painted sky shader");
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
    if(ok&&SUCCEEDED(d->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2,&options,sizeof options))&&options.ROVsSupported){
        Ok(d->CreatePixelShader(kPsWaterSeedOrdered,sizeof kPsWaterSeedOrdered,nullptr,&G.seedOrderedPs),"PsWaterSeedOrdered");
        Ok(d->CreatePixelShader(kPsWaterResolve,sizeof kPsWaterResolve,nullptr,&G.resolvePs),"PsWaterResolve");
        if(G.seedOrderedPs)MakeWaveBounds(d);
    }
    bd.ByteWidth=16;
    ok=ok&&Ok(d->CreateBuffer(&bd,nullptr,&G.transportConstants),"the transport constants");
    ok = ok && g_emitter.Make(d,kCsEnvironmentEmitter,sizeof kCsEnvironmentEmitter);
    rd.FillMode = D3D11_FILL_WIREFRAME;
    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE; depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ok = ok && Ok(d->CreateRasterizerState(&rd, &G.wire), "the wireframe state") &&
         Ok(d->CreateDepthStencilState(&depth, &G.depthState), "the water's depth state");
    if (ok && !g_waves.Make(d, {kCsSpectrum, sizeof kCsSpectrum}, {kCsFftRows, sizeof kCsFftRows},
                            {kCsFftCols, sizeof kCsFftCols}, {kCsResolve, sizeof kCsResolve})) {
        H->log(M, MML_LOG_ERROR, "the ripples' kernels and textures could not be made");
        ok = false;
    }
    D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
    for (auto &ring : G.query)
        for (auto &q : ring) ok = ok && Ok(d->CreateQuery(&qd, &q), "a query");
    qd.Query = D3D11_QUERY_PIPELINE_STATISTICS;
    for (auto &q : G.stats) ok = ok && Ok(d->CreateQuery(&qd, &q), "the geometry statistics");
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    for (auto &q : G.disjoint) ok = ok && Ok(d->CreateQuery(&qd, &q), "the water GPU clock");
    qd.Query = D3D11_QUERY_TIMESTAMP;
    for (auto &row : G.stamp) for (auto &q : row) ok = ok && Ok(d->CreateQuery(&qd, &q), "the water GPU timestamp");
    G.made = ok;
    return ok;
}

// Restore the painted sky's radiance before filtering it. Ordinary scene geometry is copied unchanged; clouds
// modulate the source, rather than an independent analytic disc being added after the reflection was filtered.
bool PaintSky(ID3D11Device *d, ID3D11DeviceContext *ctx, const MmlGfxFrame *f, const MmlGfxView *refl) {
    G.skyReady = false;
    if (!refl || S.glitter <= 0) return false;
    const unsigned w = g_refl.Width(), h = g_refl.Height();
    if (!G.skyTexture || G.skyW != w || G.skyH != h) {
        SafeRelease(G.skyOut); SafeRelease(G.skyView); SafeRelease(G.skyTexture);
        G.skyW = G.skyH = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 0; td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = td.Format; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (!Ok(d->CreateTexture2D(&td, nullptr, &G.skyTexture), "the HDR sky reflection") ||
            !Ok(d->CreateShaderResourceView(G.skyTexture, nullptr, &G.skyView), "the HDR sky view") ||
            !Ok(d->CreateUnorderedAccessView(G.skyTexture, &ud, &G.skyOut), "the HDR sky output")) return false;
        G.skyW = w; G.skyH = h;
    }
    ID3D11Buffer *cbs[] = {static_cast<ID3D11Buffer *>(f->constants), G.cb};
    ID3D11ShaderResourceView *src[] = {g_refl.View(), static_cast<ID3D11ShaderResourceView *>(refl->depth)};
    ctx->CSSetConstantBuffers(0, 2, cbs);
    ctx->CSSetShaderResources(4, 2, src);
    ctx->CSSetUnorderedAccessViews(0, 1, &G.skyOut, nullptr);
    ctx->CSSetShader(G.paintedSky, nullptr, 0);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    ID3D11ShaderResourceView *none[2] = {}; ID3D11UnorderedAccessView *noUav = nullptr;
    ctx->CSSetShaderResources(4, 2, none);
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    ctx->GenerateMips(G.skyView);
    G.skyReady = true; g_skyDrawn++;
    return true;
}

// Each plane's pixels on the screen from its query issued kRing frames ago (if the GPU has it: never waited for).
void Count(ID3D11DeviceContext *ctx, unsigned slot) {
    for (size_t p = 0; p < g_water.planes.size(); p++) {
        if (!G.issued[slot][p]) continue;
        UINT64 n;
        if (ctx->GetData(G.query[slot][p], &n, sizeof n, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            if (G.queryEpoch[slot][p] == g_visibilityEpoch) g_pixels[p] = n;
            G.issued[slot][p] = false;
        }
    }
}

// Choose an actual native triangle point, independent of either capture camera, for a material diagnostic.
bool SelectNativeProbe(const mml::scene::Snapshot &snapshot){
    using namespace mml::ray;unsigned tested=0,skip=g_sampleSkip;memset(g_sampleRejected,0,sizeof g_sampleRejected);g_sampleUnsupported=0;
    const auto nodes=snapshot.geometry.Nodes();const auto triangles=snapshot.geometry.Triangles();
    for(const auto &instance:snapshot.geometry.instances){
        if(!instance.id||instance.id>snapshot.sources.size())continue;const auto &source=snapshot.sources[instance.id-1];
        if(g_sampleVisual && source.visual!=g_sampleVisual)continue;
        auto world=[&](Vec3 p,float w){const auto *m=source.world;return Vec3{p.x*m[0]+p.y*m[4]+p.z*m[8]+w*m[12],p.x*m[1]+p.y*m[5]+p.z*m[9]+w*m[13],p.x*m[2]+p.y*m[6]+p.z*m[10]+w*m[14]};};
        for(unsigned n=instance.root;n<nodes[instance.root].escape;n++){
            const auto &node=nodes[n];for(unsigned j=0;j<node.count;j++){
                const auto &t=triangles[node.first+j];const auto &m=snapshot.materials[instance.Material(t)];
                if(g_sampleCutout && (!m.alphaTest||m.blend))continue;
                if(g_sampleBlend && !m.blend)continue;
                if(g_sampleAdditive && (!m.blend||m.sourceBlend!=5||m.destinationBlend!=2))continue;
                if(m.unsupported||(m.flags&14)){g_sampleRejected[0]++;g_sampleUnsupported|=m.unsupported;continue;}
                if(!m.textureKeys[0]||m.textureIds[0]>=UINT32_MAX-1){g_sampleRejected[1]++;continue;}
                if(m.stages[0].min_filter>3||m.stages[0].mag_filter>2){g_sampleRejected[2]++;continue;}
                const auto a=world(t.e1,0),b=world(t.e2,0),center=world(t.p+t.e1*g_sampleBary[0]+t.e2*g_sampleBary[1],1);
                Vec3 normal{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};const float length=std::sqrt(normal.x*normal.x+normal.y*normal.y+normal.z*normal.z);
                if(!(length>1e-8f))continue;normal=normal*((m.cull==D3DCULL_CW?-1.0f:1.0f)/length);const auto origin=center+normal*.05f,direction=normal*-1;
                bool unknown=false;for(const auto &u:snapshot.geometry.unknown)if(origin.x>=u.lo.x&&origin.x<=u.hi.x&&origin.y>=u.lo.y&&origin.y<=u.hi.y&&origin.z>=u.lo.z&&origin.z<=u.hi.z){unknown=true;break;}
                if(unknown){g_sampleRejected[3]++;continue;}
                if(++tested>256)return false;
                const auto cpu=ReferenceGeometry(snapshot.geometry,snapshot.coverage,origin,direction,.001f,.1f);
                if(cpu.status!=1||cpu.instance!=instance.id){g_sampleRejected[4]++;continue;}
                if(skip){skip--;continue;}
                g_sampleRejected[5]++;
                g_probeRay[0]=origin.x;g_probeRay[1]=origin.y;g_probeRay[2]=origin.z;g_probeRay[3]=.001f;
                g_probeRay[4]=direction.x;g_probeRay[5]=direction.y;g_probeRay[6]=direction.z;g_probeRay[7]=g_sampleRange;return true;
            }
        }
    }return false;
}

// Blocking readback is confined to an explicit one-ray diagnostic, never the normal frame path.
bool ProbeRay(ID3D11Device *device,ID3D11DeviceContext *context,unsigned generation) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Buffer> input,output,read;
    ComPtr<ID3D11ShaderResourceView> inputView;
    ComPtr<ID3D11UnorderedAccessView> outputView;
    D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof g_probeRay;desc.StructureByteStride=16;
    desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    D3D11_SUBRESOURCE_DATA initial{g_probeRay,0,0};
    if(!Ok(device->CreateBuffer(&desc,&initial,&input),"ray probe input") ||
       !Ok(device->CreateShaderResourceView(input.Get(),nullptr,&inputView),"ray probe input view"))return false;
    desc.Usage=D3D11_USAGE_DEFAULT;desc.ByteWidth=desc.StructureByteStride=sizeof(WorldProbe);desc.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    if(!Ok(device->CreateBuffer(&desc,nullptr,&output),"ray probe output") ||
       !Ok(device->CreateUnorderedAccessView(output.Get(),nullptr,&outputView),"ray probe output view"))return false;
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(!Ok(device->CreateBuffer(&desc,nullptr,&read),"ray probe readback") || !g_sceneGpu.BindCS(context,g_scene.Cycle(),generation))return false;
    auto *srv=inputView.Get();auto *uav=outputView.Get();
    context->CSSetShaderResources(22,1,&srv);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    context->CSSetShader(g_probeRay[9]>.5f?G.blendProbe:G.rayProbe,nullptr,0);context->Dispatch(1,1,1);
    srv=nullptr;uav=nullptr;context->CSSetShaderResources(22,1,&srv);context->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
    g_sceneGpu.UnbindCS(context);context->CSSetShader(nullptr,nullptr,0);context->CopyResource(read.Get(),output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if(!Ok(context->Map(read.Get(),0,D3D11_MAP_READ,0,&mapped),"ray probe map"))return false;
    memcpy(&g_worldProbe,mapped.pData,sizeof g_worldProbe);g_rayResult=g_worldProbe.hit;context->Unmap(read.Get(),0);return true;
}

void CollectTiming(ID3D11DeviceContext *ctx,unsigned slot) {
    if(!G.timingIssued[slot])return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
    if(ctx->GetData(G.disjoint[slot],&clock,sizeof clock,D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)return;
    UINT64 stamp[kSurfaceEndStamp+1]{};
    for(unsigned i=0;i<=kSurfaceEndStamp;i++){
        if(i>1&&i<kSurfaceEndStamp&&!G.worldTiming[slot])continue;
        if(ctx->GetData(G.stamp[slot][i],&stamp[i],sizeof stamp[i],D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK)return;
    }
    G.timingIssued[slot]=false;
    if(clock.Disjoint||!clock.Frequency||G.timingFrame[slot]<=G.latestTiming)return;
    G.latestTiming=G.timingFrame[slot];
    const auto milliseconds=[&](unsigned first,unsigned last){return float(double(stamp[last]-stamp[first])*1000/clock.Frequency);};
    G.fftMs=milliseconds(0,1);
    G.surfaceMs=milliseconds(1,kSurfaceEndStamp);
    G.seedMs=G.traceMs=0;
    if(G.worldTiming[slot]){
        G.seedMs=milliseconds(1,2);
        G.traceMs=milliseconds(2,3);
    }
    G.composeMs=milliseconds(G.worldTiming[slot]?kSurfaceEndStamp-1:1,kSurfaceEndStamp);
}

void WaterlineLevels(std::vector<float> &levels) {
    levels.clear();
    for(const auto &plane:g_water.planes)levels.push_back(plane.level);
}

void PrepareFacetRanges(const mml::scene::Snapshot &snapshot,PreparedWaterline &prepared){
    using namespace mml::ray;
    const auto nodes=snapshot.geometry.Nodes();
    const auto &instances=snapshot.geometry.instances;
    const auto triangleCount=snapshot.geometry.Triangles().size();
    auto ranges=std::make_shared<std::vector<PreparedWaterline::FacetRange>>();
    ranges->reserve(instances.size());
    uint32_t next=0;
    for(uint32_t i=0;i<instances.size();i++){
        const auto &instance=instances[i];
        if(instance.flags&(BillboardFacing|BillboardY))continue;
        // The builder stores leaf triangles in preorder, contiguously per mesh.
        if(instance.root>=nodes.size())return;
        const auto end=nodes[instance.root].escape;
        if(end<=instance.root||end>nodes.size())return;
        uint32_t first=instance.root;
        while(first<end&&!nodes[first].count)++first;
        const auto &last=nodes[end-1];
        if(first==end||!last.count||uint64_t(last.first)+last.count>triangleCount||last.first<nodes[first].first)return;
        const uint32_t count=last.first+last.count-nodes[first].first;
        if(uint64_t(next)+count>64u*65535u)return;
        ranges->push_back({next,count,nodes[first].first,i});next+=count;
    }
    if(!next||uint64_t(next)+instances.size()-ranges->size()>64u*65535u)return;
    const uint32_t triangles=next;
    for(uint32_t i=0;i<instances.size();i++)
        if(instances[i].flags&(BillboardFacing|BillboardY))ranges->push_back({next++,1,UINT32_MAX,i});
    prepared.facetRanges=std::move(ranges);prepared.facetCycle=snapshot.cycle;prepared.facetFrame=snapshot.nativeFrame;
    prepared.triangles=triangles;prepared.billboards=next-triangles;
}

void PrepareWaterline(const mml::scene::Snapshot &snapshot,void *context) noexcept {
    auto &prepared=*static_cast<PreparedWaterline *>(context);
    const double started=ClockSeconds();
    try{
        auto model=std::make_shared<mml::scene::WaterlineModel>();
        if(mml::scene::BuildWaterline(snapshot,prepared.levels,*model))prepared.model=std::move(model);
    }catch(...){prepared.model.reset();}
    prepared.milliseconds=(ClockSeconds()-started)*1000;
    if(prepared.prepareFacets){
        try{PrepareFacetRanges(snapshot,prepared);}catch(...){prepared.facetRanges.reset();}
    }
}

bool HardwareTransport(const MmlGfxFrame *frame,ID3D11DeviceContext *context){
    g_hardwareLastUsed=false;
    if(!g_hardwareRequested||!g_hardwareRequest||!g_hardwareReply||!g_hardwareStatus)return false;
    const double started=ClockSeconds();
    auto run=[&]()->bool{
        river::HardwareStatus status{};uint64_t version=0;
        if(H->channel_read(g_hardwareStatus,&status,sizeof status,&version)!=sizeof status||status.size!=sizeof status||!status.ready||!status.worker){g_hardwareError=status.error;return false;}
        auto *device=static_cast<ID3D11Device *>(frame->device);
        if(g_hardwareProducer.Generation()!=frame->generation||g_hardwareWorker!=status.worker){
            if(!g_hardwareProducer.Initialize(device,frame->generation,kCsTransferWords,kCsTransferOutput)){g_hardwareError=g_hardwareProducer.Error();return false;}
            g_hardwareWorker=status.worker;
        }
        const auto *snapshot=g_scene.Current(g_scene.Cycle());if(!snapshot)return false;
        river::HardwareRequest work{};
        if(g_hardwareFacets){
            const auto &prepared=g_preparedWaterline;
            if(!prepared.facetRanges||prepared.facetCycle!=snapshot->cycle||prepared.facetFrame!=snapshot->nativeFrame)return false;
            mml::gfx::ImmutableBuffer ranges;
            const HRESULT made=ranges.MakeOwned(device,prepared.facetRanges,&g_hardwareRanges);
            if(FAILED(made)){g_hardwareError=made;return false;}
            ranges.Swap(g_hardwareRanges);
            work.triangles=prepared.triangles;work.billboards=prepared.billboards;
        }
        if(!g_hardwareProducer.Begin(snapshot->cycle,snapshot->nativeFrame,frame->width,frame->height,UINT(snapshot->geometry.instances.size()),20))return false;
        struct Bindings {
            ID3D11DeviceContext *context;
            ID3D11ComputeShader *shader;
            ID3D11ShaderResourceView *views[36]{};
            ID3D11Buffer *constants[6]{};
            ID3D11SamplerState *samplers[2]{};
            ID3D11UnorderedAccessView *output=nullptr;
            Bindings(ID3D11DeviceContext *c,ID3D11ComputeShader *s):context(c),shader(s){
                c->CSGetShaderResources(0,36,views);c->CSGetConstantBuffers(0,6,constants);c->CSGetSamplers(2,2,samplers);c->CSGetUnorderedAccessViews(0,1,&output);
            }
            ~Bindings(){
                context->CSSetShader(shader,nullptr,0);context->CSSetShaderResources(0,1,views);
                context->CSSetConstantBuffers(0,1,constants);context->CSSetUnorderedAccessViews(0,1,&output,nullptr);
                for(auto *view:views)if(view)view->Release();for(auto *constant:constants)if(constant)constant->Release();
                for(auto *sampler:samplers)if(sampler)sampler->Release();if(output)output->Release();
            }
        } bindings(context,G.transportCs);
        const unsigned buffers[]{12,16,17,18,19,20,21,23,24,25,26,27,28,29,30,31,32};
        const unsigned images[]{2,6,7,8,11,33,34,35};
        bool ready=true;g_hardwareFailedBinding=0;
        double checkpoint=ClockSeconds();
        for(unsigned slot:buffers)if(!g_hardwareProducer.Buffer(slot,bindings.views[slot],slot!=12)){g_hardwareFailedBinding=slot;ready=false;break;}
        if(ready&&work.triangles&&!g_hardwareProducer.Buffer(38,g_hardwareRanges.view,true)){g_hardwareFailedBinding=38;ready=false;}
        g_hardwareParts[0]=(ClockSeconds()-checkpoint)*1000;checkpoint=ClockSeconds();
        if(ready)for(unsigned slot:{0u,1u,3u,4u})if(!g_hardwareProducer.Constant(100+slot,bindings.constants[slot])){g_hardwareFailedBinding=100+slot;ready=false;break;}
        g_hardwareParts[1]=(ClockSeconds()-checkpoint)*1000;checkpoint=ClockSeconds();
        if(ready)for(unsigned slot:images)if(!g_hardwareProducer.Image(slot,bindings.views[slot],slot==2)){g_hardwareFailedBinding=slot;ready=false;break;}
        g_hardwareParts[2]=(ClockSeconds()-checkpoint)*1000;checkpoint=ClockSeconds();
        if(ready)ready=g_hardwareProducer.Sampler(2,bindings.samplers[0])&&g_hardwareProducer.Sampler(3,bindings.samplers[1])&&
            g_hardwareProducer.Output(frame->width,frame->height,3);
        g_hardwareParts[3]=(ClockSeconds()-checkpoint)*1000;
        if(!ready){g_hardwareError=g_hardwareProducer.Error();g_hardwareProducer.Cancel();return false;}
        mml::gfx::TransferReply previous{};uint64_t seen=0;H->channel_read(g_hardwareReply,&previous,sizeof previous,&seen);
        checkpoint=ClockSeconds();
        if(!g_hardwareProducer.Publish(work.transfer,false)){g_hardwareError=g_hardwareProducer.Error();return false;}
        g_hardwareParts[4]=(ClockSeconds()-checkpoint)*1000;
        if(!H->channel_write(g_hardwareRequest,&work,sizeof work)){g_hardwareProducer.NotDelivered();return false;}
        H->channel_wait(g_hardwareReply,seen,2);
        mml::gfx::TransferReply reply{};
        if(H->channel_read(g_hardwareReply,&reply,sizeof reply,&seen)!=sizeof reply||!mml::gfx::TransferMatches(work.transfer,reply))return false;
        if(reply.status!=uint32_t(mml::gfx::TransferStatus::Submitted)){g_hardwareError=reply.error;g_hardwareProducer.Rejected(reply);return false;}
        checkpoint=ClockSeconds();
        if(!g_hardwareProducer.Read(reply,G.transportOutput)){g_hardwareError=g_hardwareProducer.Error();return false;}
        g_hardwareParts[5]=(ClockSeconds()-checkpoint)*1000;
        g_hardwareError=S_OK;return true;
    };
    g_hardwareLastUsed=run();g_hardwareProducer.FlushInputs();g_hardwareCpuMs=(ClockSeconds()-started)*1000;
    if(g_hardwareLastUsed)g_hardwareUsed++;else g_hardwareFallback++;
    return g_hardwareLastUsed;
}
int HardwareMode(const char *args,MmlReply *reply,void *){
    if(args&&strstr(args,"\"facets\""))g_hardwareFacets=ArgumentTrue(args,"\"facets\"");
    if(args&&strstr(args,"\"on\"")){
        g_hardwareRequested=ArgumentTrue(args,"\"on\"");
        if(!g_hardwareRequested){
            if(g_hardwareRequest){river::HardwareRequest release{};release.transfer.operation=uint32_t(mml::gfx::TransferOperation::Release);H->channel_write(g_hardwareRequest,&release,sizeof release);}
            g_hardwareProducer.Release();g_hardwareRanges.Clear();g_hardwareWorker=0;g_hardwareLastUsed=false;
        }
    }
    if(ArgumentTrue(args,"\"reset\"")){g_hardwareUsed=g_hardwareFallback=0;g_hardwareCpuMs=0;g_hardwareError=S_OK;}
    river::HardwareStatus status{};uint64_t version=0;
    if(g_hardwareStatus)H->channel_read(g_hardwareStatus,&status,sizeof status,&version);
    char out[950];snprintf(out,sizeof out,"{\"on\":%s,\"facets\":%s,\"last_used\":%s,\"used\":%llu,\"fallback\":%llu,\"cpu_ms\":%.3f,\"error\":\"%08X\",\"failed_binding\":%u,\"worker_ready\":%u,\"worker_error\":\"%08X\",\"worker_submitted\":%llu,\"worker_rejected\":%llu,\"facet_reuses\":%u,\"enqueue_ms\":%.3f,\"prepare_ms\":{\"buffers\":%.3f,\"constants\":%.3f,\"images\":%.3f,\"output\":%.3f,\"publish\":%.3f,\"read\":%.3f},\"worker_gpu_ms\":{\"inputs\":%.3f,\"bounds\":%.3f,\"trace\":%.3f,\"pack\":%.3f}}",
        g_hardwareRequested?"true":"false",g_hardwareFacets?"true":"false",g_hardwareLastUsed?"true":"false",static_cast<unsigned long long>(g_hardwareUsed),static_cast<unsigned long long>(g_hardwareFallback),g_hardwareCpuMs,unsigned(g_hardwareError),
        g_hardwareFailedBinding,status.ready,unsigned(status.error),static_cast<unsigned long long>(status.submitted),static_cast<unsigned long long>(status.rejected),status.facetReuses,status.enqueueMs,
        g_hardwareParts[0],g_hardwareParts[1],g_hardwareParts[2],g_hardwareParts[3],g_hardwareParts[4],g_hardwareParts[5],
        status.gpuInputsMs,status.gpuBoundsMs,status.gpuTraceMs,status.gpuPackMs);
    H->reply_append(reply,out,static_cast<size_t>(-1));return 0;
}
void Pass(const MmlGfxFrame *f, void *) {
    if (MML_GFX_HAS(f, in_view) && f->in_view) return;   // (the frame's only)
    G.waveBoundsActive=false;
    const bool worldMaterial=S.on&&WorldMaterialMode(S.debug);
    if(g_scenePending || (S.debug>=22 && S.debug<=26) || worldMaterial) {
        auto *device=static_cast<ID3D11Device *>(f->device);
        auto *context=static_cast<ID3D11DeviceContext *>(f->context);
        g_scene.Complete();const auto snapshot=g_scene.Lease(g_scene.Cycle());
        bool ready=snapshot && (!g_scenePending || !g_autoRay || SelectNativeProbe(*snapshot)) && ((G.madeFor==f->generation && G.made) || Make(device));
        g_sceneUploadMs=0;if(ready){G.madeFor=f->generation;const double started=ClockSeconds();ready=g_sceneGpu.UploadOwned(device,snapshot,f->generation,g_appendPackedGpu);g_sceneUploadMs=(ClockSeconds()-started)*1000;}
        g_waterline.reset();g_waterlineGpu.Invalidate();g_waterlineBuildMs=0;
        if(ready&&snapshot->materialsCollected&&!g_water.planes.empty()){
            auto &prepared=g_preparedWaterline;
            const bool levelsMatch=prepared.levels.size()==g_water.planes.size()&&
                std::equal(prepared.levels.begin(),prepared.levels.end(),g_water.planes.begin(),[](const float &level,const auto &plane){
                    return std::memcmp(&level,&plane.level,sizeof level)==0;
                });
            if(prepared.model&&prepared.model->cycle==snapshot->cycle&&
               prepared.model->nativeFrame==snapshot->nativeFrame&&levelsMatch){
                g_waterline=std::move(prepared.model);g_waterlineBuildMs=prepared.milliseconds;
            }else{
                PreparedWaterline current;
                try{WaterlineLevels(current.levels);PrepareWaterline(*snapshot,&current);}catch(...){}
                g_waterline=std::move(current.model);g_waterlineBuildMs=current.milliseconds;
            }
            if(g_waterline)g_waterlineGpu.UploadOwned(device,g_waterline,f->generation);
        }
        if(g_scenePending){
            g_pagesReady=false;g_textureCount=g_textureMissing=0;g_textureBytes=g_pageBytes=0;
            if(ready && snapshot->materialsCollected){
                g_textureCount=static_cast<unsigned>(snapshot->textures.size());
                g_textureMissing=snapshot->unavailableTextures;g_textureBytes=snapshot->textureBytes;
                g_pagesReady=g_sceneGpu.HasMaterials();g_pageBytes=g_sceneGpu.PageBytes();
            }
        }
        if(g_scenePending) {
            g_sceneSuccess=false;g_rayResult=RayResult{};g_worldProbe={};g_probeNativeMaterial={};g_attributesAgree=false;g_hitVisual=0;g_cpuAgrees=false;g_cpuStatus=3;g_cpuDistance=0;
            g_sceneFrame=snapshot?snapshot->nativeFrame:0;
            g_probeCache={};if(snapshot)g_probeCache={snapshot->meshBuilt,snapshot->meshReused,snapshot->textureCopied,snapshot->textureReused,snapshot->cachedMeshBytes,snapshot->cachedTextureBytes,0};
            g_probeCache.uploadError=g_sceneGpu.Error();g_probeCache.uploadStage=g_sceneGpu.ErrorStage();
            g_probeCache.gpuReused=g_sceneGpu.ReusedBuffers();g_probeCache.gpuBytes=g_sceneGpu.UploadedBytes();g_probeCache.uploadMs=g_sceneUploadMs;
            if(snapshot){g_probeCache.collectMs=snapshot->nativeCollectMs;g_probeCache.copyMs=snapshot->sourceCopyMs;g_probeCache.buildMs=snapshot->meshBuildMs;
                g_probeCache.textureMs=snapshot->textureMs;g_probeCache.instancesMs=snapshot->instanceBuildMs;}
            g_sceneTriangles=snapshot?static_cast<unsigned>(snapshot->Surfaces().size()):0;
            g_sceneMissing=snapshot?static_cast<unsigned>(snapshot->geometry.unknown.size()):0;
            if(ready)g_sceneSuccess=ProbeRay(device,context,f->generation);
            if(g_sceneSuccess){
                const auto cpu=mml::ray::ReferenceGeometry(snapshot->geometry,snapshot->coverage,
                    {g_probeRay[0],g_probeRay[1],g_probeRay[2]},{g_probeRay[4],g_probeRay[5],g_probeRay[6]},g_probeRay[3],g_probeRay[7]);
                g_cpuStatus=cpu.status;g_cpuDistance=cpu.distance;
                g_cpuAgrees=cpu.status==g_rayResult.status && std::abs(cpu.distance-g_rayResult.distance)<.002f &&
                    (cpu.status!=1 || (cpu.instance==g_rayResult.instance && cpu.primitive==g_rayResult.primitive &&
                    std::abs(cpu.bary[0]-g_rayResult.bary[0])<.002f && std::abs(cpu.bary[1]-g_rayResult.bary[1])<.002f));
            }
            if(g_sceneSuccess && g_rayResult.status==1 && g_rayResult.instance>0 && g_rayResult.instance<=snapshot->sources.size()){
                g_hitVisual=snapshot->sources[g_rayResult.instance-1].visual;
                g_probeCache.revision=snapshot->meshRevisions[g_rayResult.instance-1];
            }
            if(g_sceneSuccess && g_rayResult.status==1 && g_rayResult.primitive<snapshot->Surfaces().size()){
                g_probeNativeMaterial=snapshot->materials[g_rayResult.material];
                const auto &surface=snapshot->Surfaces()[g_rayResult.primitive];const float weights[3]={1-g_rayResult.bary[0]-g_rayResult.bary[1],g_rayResult.bary[0],g_rayResult.bary[1]};
                g_attributesAgree=g_worldProbe.attributes[1]==1 && g_worldProbe.attributes[0]==surface.material;
                const unsigned shifts[4]={16,8,0,24};
                for(unsigned c=0;c<4;c++){float uv=0,color=0;for(unsigned v=0;v<3;v++){
                    uv+=surface.uv[v][c]*weights[v];color+=float((surface.color[v]>>shifts[c])&255)/255*weights[v];}
                    g_attributesAgree=g_attributesAgree&&std::abs(uv-g_worldProbe.uv[c])<2e-5f&&std::abs(color-g_worldProbe.diffuse[c])<2e-5f;}
            }
            g_scenePending=false;g_sceneCaptures++;
            if(!worldMaterial&&(S.debug<22 || S.debug>26)){if(g_keepSceneCache)g_scene.DropSnapshot();else g_scene.Release();g_sceneGpu.Release();}
        }
        // An unavailable current diagnostic never masquerades as an old scene or the ordinary image.
        if(S.debug>=22 && S.debug<=26 && !ready)return;
        if(worldMaterial&&!ready)return;
    }
    if (!S.on) return;
    g_sky.Update(*f);
    if (!g_asked && GetTickCount() >= g_nextAsk) Ask();
    if (g_water.planes.empty()) return;
    float visibilityVp[16]; Mul44(f->view, f->proj, visibilityVp);
    bool possible = false;
    for (const auto &plane : g_water.planes)
        possible = possible || VisibleWaterPlane(plane, visibilityVp, f->proj[0], f->proj[5]);
    if (!possible) {
        g_offscreen++; g_haveHistory = false; G.skyReady = false;
        // Full draw and FFT skipped; moving back makes the conservative test positive without awaiting a query.
        return;
    }
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (G.madeFor != f->generation || !G.made) {
        if (!Make(d)) return;
        G.madeFor = f->generation;
    }
    if (G.vbVersion != g_water.version) {
        SafeRelease(G.vb);
        memset(G.issued, 0, sizeof G.issued);
        g_patches.clear(); g_patchPlanes.clear();
        for (const auto &p : g_water.planes) {
            unsigned first = static_cast<unsigned>(g_patches.size() / 9);
            mw::WaterPatches(g_water.tris.data() + size_t(p.first) * 9, p.count, g_patches);
            g_patchPlanes.push_back({p.level, first, static_cast<unsigned>(g_patches.size() / 9) - first});
        }
        if (g_patches.empty()) return;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(g_patches.size() * sizeof(float));
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{g_patches.data(), 0, 0};
        if (!Ok(d->CreateBuffer(&bd, &init, &G.vb), "the water's triangles")) return;
        G.vbVersion = g_water.version;
    }
    unsigned slot = G.frame++ % kRing;
    Count(ctx, slot);
    D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
    if (G.statsIssued[slot] && ctx->GetData(G.stats[slot], &stats, sizeof stats,
                                         D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
        G.geometryStats = stats; G.statsIssued[slot] = false;
    }
    if (G.depthW != f->width || G.depthH != f->height) {
        ReleaseTransport();
        SafeRelease(G.dsv); SafeRelease(G.depth);
        G.depthW = G.depthH = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = f->width; td.Height = f->height; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        // In Quality/Performance the viewport is smaller than the actual effects/MRT texture. D3D11 requires the
        // depth target to match the attachment dimensions, not the render-size viewport.
        ID3D11Resource *targetResource = nullptr;
        static_cast<ID3D11RenderTargetView *>(f->target)->GetResource(&targetResource);
        if (targetResource) {
            D3D11_TEXTURE2D_DESC color{};
            static_cast<ID3D11Texture2D *>(targetResource)->GetDesc(&color);
            td.Width = color.Width; td.Height = color.Height;
            targetResource->Release();
        }
        td.Format = DXGI_FORMAT_D32_FLOAT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (!Ok(d->CreateTexture2D(&td, nullptr, &G.depth), "the water's depth buffer") ||
            !Ok(d->CreateDepthStencilView(G.depth, nullptr, &G.dsv), "the water's depth view")) return;
        G.depthW = f->width; G.depthH = f->height;
    }
    if(worldMaterial&&!G.transportView){
        bool ordered=G.seedOrderedPs&&MakeTransport(d,f->width,f->height,true);
        if(!ordered&&!MakeTransport(d,f->width,f->height,false))return;
        H->log(M,MML_LOG_INFO,"water ray seed raster: %s",ordered?"one ordered pass":"two render-target passes");
    }
    double now = ClockSeconds();
    float time = static_cast<float>(now);
    float waveTime = static_cast<float>(g_waveClock.Advance(now, S.waveSpeed));
    // Deterministic native regression captures only. Normal clock/lighting/wave behavior is unchanged.
    if(g_captureClock)time=waveTime=g_captureSeconds;
    bool seen = false;
    for (size_t p = 0; p < g_water.planes.size(); p++) seen = seen || g_pixels[p];
    CollectTiming(ctx,slot);
    const bool timing = !G.timingIssued[slot];
    if (timing) { ctx->Begin(G.disjoint[slot]); ctx->End(G.stamp[slot][0]); }
    if (seen) g_waves.Update(d, ctx, g_spectrum, g_specVersion, waveTime);
    if (timing) ctx->End(G.stamp[slot][1]);

    // the reflection drawn for this frame
    const MmlGfxView *refl = nullptr;
    if (MML_GFX_HAS(f, views) && g_reflPlane >= 0)
        for (unsigned i = 0; i < f->view_count; i++)
            if (f->views[i].handle == g_view && f->views[i].fresh) refl = &f->views[i];
    if (refl && !g_refl.Update(d, ctx, static_cast<ID3D11Texture2D *>(refl->color_texture))) refl = nullptr;

    float cb[120] = {};
    cb[116]=S.reflection?kWorldPathMetres:0.0f;
    cb[117]=kWorldPathMetres; // independent finite world-contact region, even with object reflection switched off
    Mul44(f->view, f->proj, cb);
    if (refl) {
        Mul44(refl->view, refl->proj, cb + 16);
        if (!Inverse44(cb + 16, cb + 64)) refl = nullptr;
    }
    if (refl) {
        cb[32] = refl->view[2], cb[33] = refl->view[6], cb[34] = refl->view[10], cb[35] = refl->view[14];
        memcpy(g_rview, refl->view, sizeof g_rview);
        memcpy(g_rproj, refl->proj, sizeof g_rproj);
    }
    cb[36] = g_reflLevel;
    cb[37] = S.wind >= 0.1f ? S.waves : 0;
    cb[38] = refl ? 1.0f : 0.0f;
    cb[39] = time;
    for (int c = 0; c < mw::kCascades; c++) cb[40 + c] = 1 / g_spectrum.patch[c];
    cb[43] = g_spectrum.rest;
    float clarity = S.clarity > 0.05f ? S.clarity : 0.05f;
    for (int c = 0; c < 3; c++) {
        cb[44 + c] = -std::log(S.tint[c] > 1e-4f ? S.tint[c] : 1e-4f) / clarity;
        cb[48 + c] = S.colour[c];
    }
    cb[47] = S.depth;
    cb[51] = S.foam;
    cb[52] = S.glitter;
    cb[53] = refl && g_refl.Encoded() ? 1.0f : 0.0f;
    cb[54] = static_cast<float>(S.debug);
    cb[55] = S.chop;
    cb[56] = static_cast<float>(g_refl.Width() ? g_refl.Width() : f->width);
    cb[57] = static_cast<float>(g_refl.Height() ? g_refl.Height() : f->height);
    cb[58] = 1 / cb[56];
    cb[59] = 1 / cb[57];
    float strength = S.wind >= 0.1f ? S.waves : 0;
    cb[60] = S.scattering > 0 ? S.scattering : 0;
    cb[61] = S.phase;
    cb[62] = S.crests;
    cb[63] = 2 * std::sqrt(g_spectrum.heightVar) * strength;   // the crests' height: 2 deviations of the heights
    cb[80] = 0.00465f * S.shadowSoftness;   // tan of the sun's angular radius (0.266 degrees), scaled by the setting
    cb[81] = std::cos(S.sunRadius * 0.01745329252f); // outer angular support of the painted sun's glow
    cb[82] = S.reflectionSoftness;
    cb[83] = S.reflectionStrength;
    cb[84] = 1; cb[85] = S.geometry ? 1.0f : 0.0f; // gravity height is baked into the spectrum; fine ripples stay independent
    cb[86] = 6 * std::sqrt(g_spectrum.heightVar) * strength * (1 + S.chop);
    const bool temporal = MML_GFX_HAS(f, temporal_target) && f->temporal_target;
    memcpy(cb + 88, g_previousVP, sizeof g_previousVP);
    memcpy(cb + 104, g_previousEye, sizeof g_previousEye);
    if (MML_GFX_HAS(f, jitter)) { cb[108] = f->jitter[0]; cb[109] = f->jitter[1]; }
    cb[110] = g_haveHistory && seen ? 1.0f : 0.0f; cb[111] = temporal ? 1.0f : 0.0f;
    ctx->UpdateSubresource(G.cb, 0, nullptr, cb, 0, 0);
    // Keep the old reconstruction only as an explicit diagnostic; it is not incident light in the new material.
    const bool paintedSky = S.debug == 16 && PaintSky(d, ctx, f, refl);
    G.skyReady = paintedSky;
    cb[87] = paintedSky ? 1.0f : 0.0f;
    cb[112]=static_cast<float>(g_sky.Resolution());cb[113]=g_sky.View()?1.0f:0.0f;
    cb[114]=static_cast<float>(S.shadowSource);
    cb[115]=S.shadowSource==0 && f->k->sun_seen[3]>.5f &&
        g_emitter.Update(ctx,g_sky.View(),g_sky.Captures(),f->k->sun_seen,cb[81]) ? 1.0f : 0.0f;
    ctx->UpdateSubresource(G.cb, 0, nullptr, cb, 0, 0);

    UINT stride = 12, offset = 0;
    ctx->IASetInputLayout(G.layout);
    ctx->IASetVertexBuffers(0, 1, &G.vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ID3D11Buffer *cbs[2] = {static_cast<ID3D11Buffer *>(f->constants), G.cb};
    ctx->VSSetConstantBuffers(0, 2, cbs);
    ctx->HSSetConstantBuffers(0, 2, cbs);
    ctx->DSSetConstantBuffers(0, 2, cbs);
    ctx->PSSetConstantBuffers(0, 2, cbs);
    ctx->VSSetShader(G.vs, nullptr, 0);
    ctx->HSSetShader(G.hs, nullptr, 0);
    ctx->DSSetShader(worldMaterial?G.worldDs:G.ds, nullptr, 0);
    const bool geometryDiagnostic=S.debug>=22 && S.debug<=26 && g_sceneGpu.BindPS(ctx,g_scene.Cycle(),f->generation);
    if(S.debug>=22 && S.debug<=26 && !geometryDiagnostic)return;
    const bool worldBound=worldMaterial&&g_sceneGpu.BindCS(ctx,g_scene.Cycle(),f->generation);
    if(worldMaterial&&!worldBound)return;
    if(worldMaterial&&(!g_waterlineGpu.BindPS(ctx,g_scene.Cycle(),f->generation)||
                       !g_waterlineGpu.BindDS(ctx,g_scene.Cycle(),f->generation)||
                       !g_waterlineGpu.BindCS(ctx,g_scene.Cycle(),f->generation))){
        g_sceneGpu.UnbindCS(ctx);g_waterlineGpu.UnbindPS(ctx);g_waterlineGpu.UnbindDS(ctx);g_waterlineGpu.UnbindCS(ctx);return;
    }
    if(S.debug==27)cb[54]=0; // explicit accepted-shader comparison, never an automatic fallback
    ctx->UpdateSubresource(G.cb,0,nullptr,cb,0,0);
    auto *pixelShader=worldBound?G.worldPs:G.ps;
    if(geometryDiagnostic)pixelShader=S.debug>=25?G.radiancePs:G.geometryPs;
    ctx->PSSetShader(pixelShader,nullptr,0);
    ID3D11ShaderResourceView *srv[9] = {refl ? g_refl.View() : nullptr,
                                        refl ? static_cast<ID3D11ShaderResourceView *>(refl->depth) : nullptr,
                                        g_waves.Slopes(), g_waves.Shape(), g_waves.Displacement(), g_waves.PreviousDisplacement(),
                                        paintedSky ? G.skyView : (refl ? g_refl.View() : nullptr),g_sky.View(),g_emitter.View()};
    ctx->PSSetShaderResources(4, 9, srv);
    ctx->DSSetShaderResources(4, 9, srv);
    ID3D11SamplerState *samplers[2] = {G.wrap, G.clamp};
    ctx->PSSetSamplers(2, 2, samplers);
    ctx->DSSetSamplers(2, 2, samplers);
    ctx->RSSetState(S.debug == 14 ? G.wire : G.rs);
    ID3D11RenderTargetView *target = nullptr;
    ID3D11DepthStencilView *oldDepth = nullptr;
    ID3D11DepthStencilState *oldState = nullptr; UINT oldRef = 0;
    ctx->OMGetRenderTargets(1, &target, &oldDepth);
    ctx->OMGetDepthStencilState(&oldState, &oldRef);
    ctx->OMSetDepthStencilState(G.depthState, 0);
    const bool resolve=worldBound&&S.debug==0&&g_resolveRequested&&G.resolvePs&&G.orderedSeeds[3]&&f->full_screen&&MakeResolve(d);
    G.resolveActive=resolve;
    auto drawGeometry=[&]{
        const bool record=!G.statsIssued[slot];
        if(record)ctx->Begin(G.stats[slot]);
        for(size_t p=0;p<g_water.planes.size();p++){
            const bool query=!G.issued[slot][p];
            if(query)ctx->Begin(G.query[slot][p]);
            ctx->Draw(g_patchPlanes[p].count*3,g_patchPlanes[p].first*3);
            if(query){
                ctx->End(G.query[slot][p]);
                G.issued[slot][p]=true;G.queryEpoch[slot][p]=g_visibilityEpoch;
            }
        }
        if(record){ctx->End(G.stats[slot]);G.statsIssued[slot]=true;}
    };
    if(worldBound){
        // The raster quad supplies per-facet differentials, including helper
        // lanes at silhouettes. The compute stage never samples a reflected image.
        ctx->DSSetShader(resolve?G.worldDs:G.seedDs,nullptr,0);
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend;
        float blendFactor[4];UINT sampleMask;
        ctx->OMGetBlendState(&blend,blendFactor,&sampleMask);
        ctx->OMSetBlendState(nullptr,nullptr,sampleMask);
        ctx->CSSetConstantBuffers(0,2,cbs);
        ctx->PSSetConstantBuffers(5,1,&G.transportConstants);
        ctx->CSSetConstantBuffers(5,1,&G.transportConstants);
        ctx->CSSetShaderResources(4,9,srv);
        auto *shadow=MML_GFX_HAS(f,sun_shadow)?static_cast<ID3D11ShaderResourceView *>(f->sun_shadow):nullptr;
        ctx->CSSetShaderResources(2,1,&shadow);
        ctx->CSSetSamplers(2,2,samplers);
        ctx->CSSetShader(G.transportCs,nullptr,0);
        const float empty[4]{};
        ID3D11ShaderResourceView *noView=nullptr;
        ID3D11UnorderedAccessView *noOutput=nullptr;
        ctx->ClearRenderTargetView(G.seedTargets[4],empty);
        auto drawSeed=[&]{
            for(size_t p=0;p<g_water.planes.size();p++)
                ctx->Draw(g_patchPlanes[p].count*3,g_patchPlanes[p].first*3);
        };
        if(G.orderedSeeds[0]){
            G.waveBoundsActive=g_waveBoundsRequested&&G.waveBoundsWrite&&g_waterline&&
                g_waterline->shape[0]&&g_waterline->shape[1]&&g_waterline->shape[0]<=528&&g_waterline->shape[1]<=528;
            if(G.waveBoundsActive){
                const UINT clear[4]{};
                ctx->ClearUnorderedAccessViewUint(G.waveBoundsWrite,clear);
                ctx->CSSetUnorderedAccessViews(0,1,&G.waveBoundsWrite,nullptr);
                ctx->CSSetShader(G.waveBoundsTiles,nullptr,0);ctx->Dispatch(mw::kN/8,mw::kN/8,mw::kCascades);
                ctx->CSSetShader(G.waveBoundsHierarchy,nullptr,0);ctx->Dispatch(mw::kCascades,1,1);
                ctx->CSSetShader(G.waveBoundsShore,nullptr,0);
                ctx->Dispatch((g_waterline->shape[0]+15)/16,(g_waterline->shape[1]+15)/16,1);
                ctx->CSSetUnorderedAccessViews(0,1,&noOutput,nullptr);
                ctx->CSSetShader(G.transportCs,nullptr,0);
                ctx->PSSetShaderResources(13,1,&G.waveBoundsView);
            }
            const float farDepth[4]={1,1,1,1};
            ctx->ClearUnorderedAccessViewFloat(G.orderedSeeds[3],farDepth);
            ctx->PSSetShader(G.seedOrderedPs,nullptr,0);
            const unsigned seedConstants[4]={f->width,f->height,0,(resolve?1u:0u)|(G.waveBoundsActive?2u:0u)};
            ctx->UpdateSubresource(G.transportConstants,0,nullptr,seedConstants,0,0);
            ID3D11UnorderedAccessView *seeds[]={G.orderedSeeds[0],G.orderedSeeds[1],G.orderedSeeds[2],G.orderedSeeds[3],G.resolveOutput};
            const UINT count=resolve?5:4;
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,count,seeds,nullptr);
            if(resolve)drawGeometry();else drawSeed();
            ID3D11UnorderedAccessView *noSeeds[std::size(seeds)]{};
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(0,nullptr,nullptr,0,count,noSeeds,nullptr);
            if(G.waveBoundsActive)ctx->PSSetShaderResources(13,1,&noView);
        }else{
            ctx->PSSetShader(G.seedPs,nullptr,0);
            for(unsigned pass=0;pass<2;pass++){
                const unsigned sampleConstants[4]={f->width,f->height,pass,0};
                ctx->UpdateSubresource(G.transportConstants,0,nullptr,sampleConstants,0,0);
                ctx->ClearDepthStencilView(G.dsv,D3D11_CLEAR_DEPTH,1,0);
                ctx->OMSetRenderTargets(pass?4:8,G.seedTargets+8*pass,G.dsv);
                drawSeed();
            }
            ctx->OMSetRenderTargets(0,nullptr,nullptr);
        }
        if(timing)ctx->End(G.stamp[slot][2]);
        ctx->CSSetShaderResources(33,static_cast<UINT>(std::size(G.seedViews)),G.seedViews);
        const bool hardware=HardwareTransport(f,ctx);
        for(unsigned sample=0;!hardware&&sample<kTransportSamples;sample++){
            const unsigned sampleConstants[4]={f->width,f->height,sample,0};
            ctx->UpdateSubresource(G.transportConstants,0,nullptr,sampleConstants,0,0);
            ctx->CSSetUnorderedAccessViews(0,1,&G.transportOutput,nullptr);
            ctx->Dispatch((f->width+7)/8,(f->height+7)/8,1);
            ctx->CSSetUnorderedAccessViews(0,1,&noOutput,nullptr);
        }
        if(timing)ctx->End(G.stamp[slot][3]);
        ID3D11ShaderResourceView *noSeeds[std::size(G.seedViews)]{};
        ctx->CSSetShaderResources(33,static_cast<UINT>(std::size(noSeeds)),noSeeds);
        ID3D11ShaderResourceView *noInputs[9]{};
        ctx->CSSetShaderResources(4,9,noInputs);
        ctx->CSSetShaderResources(2,1,&noView);
        ctx->CSSetShader(nullptr,nullptr,0);
        ID3D11Buffer *noConstants=nullptr;
        ctx->CSSetConstantBuffers(5,1,&noConstants);
        g_sceneGpu.UnbindCS(ctx);
        g_waterlineGpu.UnbindCS(ctx);
        ctx->OMSetBlendState(blend.Get(),blendFactor,sampleMask);
        ctx->DSSetShader(G.worldDs,nullptr,0);
        ctx->PSSetShader(G.worldPs,nullptr,0);
    }
    ID3D11RenderTargetView *targets[2] = {target, temporal ? static_cast<ID3D11RenderTargetView *>(f->temporal_target) : nullptr};
    ctx->OMSetRenderTargets(temporal ? 2 : 1, targets, G.dsv);
    if(worldBound)ctx->PSSetShaderResources(33,1,&G.transportView);
    ctx->ClearDepthStencilView(G.dsv, D3D11_CLEAR_DEPTH, 1, 0);
    if(resolve){
        ID3D11ShaderResourceView *inputs[]={G.seedViews[0],G.seedViews[1],G.resolveSurface,G.resolveDepth};
        ctx->PSSetShaderResources(34,static_cast<UINT>(std::size(inputs)),inputs);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(static_cast<ID3D11VertexShader *>(f->full_screen),nullptr,0);
        ctx->HSSetShader(nullptr,nullptr,0);ctx->DSSetShader(nullptr,nullptr,0);
        ctx->PSSetShader(G.resolvePs,nullptr,0);
        ctx->Draw(3,0);
        ID3D11ShaderResourceView *noResolve[std::size(inputs)]{};
        ctx->PSSetShaderResources(34,static_cast<UINT>(std::size(noResolve)),noResolve);
    }else drawGeometry();
    if(timing){
        ctx->End(G.stamp[slot][kSurfaceEndStamp]);ctx->End(G.disjoint[slot]);
        G.timingIssued[slot]=true;G.worldTiming[slot]=worldBound;G.timingFrame[slot]=G.frame;
    }
    ctx->OMSetRenderTargets(1, &target, oldDepth);
    ctx->OMSetDepthStencilState(oldState, oldRef);
    SafeRelease(target); SafeRelease(oldDepth); SafeRelease(oldState);
    ctx->HSSetShader(nullptr, nullptr, 0); ctx->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView *none[9] = {};
    ctx->PSSetShaderResources(4, 9, none);
    ctx->DSSetShaderResources(4, 9, none);
    if(geometryDiagnostic)g_sceneGpu.UnbindPS(ctx);
    if(worldBound){
        ctx->PSSetShaderResources(33,1,none);g_waterlineGpu.UnbindPS(ctx);g_waterlineGpu.UnbindDS(ctx);
        ID3D11Buffer *noConstants=nullptr;ctx->PSSetConstantBuffers(5,1,&noConstants);
    }
    memcpy(g_previousVP, cb, sizeof g_previousVP);
    g_previousEye[0] = f->k->eye[0]; g_previousEye[1] = f->k->eye[1]; g_previousEye[2] = f->k->eye[2];
    g_haveHistory = seen;
    if (temporal) g_temporalDrawn++;
    g_drawn++;
}

bool GraphicsFailed(){
    return MML_HAS(H,gfx_state)&&H->gfx_state(M,nullptr,0)==0;
}

void OnEvent(const MmlEvent *ev, void *) {
    if(ev->type==MML_EVENT_RENDER_BEGIN && ev->world){
        g_scene.Begin();g_preparedWaterline.model.reset();g_preparedWaterline.levels.clear();g_preparedWaterline.facetRanges.reset();
        g_sceneGpu.Invalidate();g_waterline.reset();g_waterlineGpu.Invalidate();
        const bool failed=GraphicsFailed();
        if(failed!=g_shadowBridgeFailed){
            g_shadowBridgeFailed=failed;
            UpdateShadowRequest();
        }
        if(failed){
            if(MML_HAS(H,scene_residency))H->scene_residency(M,nullptr,nullptr);
            return;
        }
        const bool normal=S.on&&WorldMaterialMode(S.debug)&&!g_scenePending;
        if(normal&&MML_HAS(H,scene_residency_camera))H->scene_residency_camera(M,kWorldSourceRadius);
        else if(MML_HAS(H,scene_residency)){
            const bool requested=g_scenePending||(S.debug>=22&&S.debug<=26);
            const float lo[3]={g_sceneBounds.lo.x,g_sceneBounds.lo.y,g_sceneBounds.lo.z},hi[3]={g_sceneBounds.hi.x,g_sceneBounds.hi.y,g_sceneBounds.hi.z};
            H->scene_residency(M,requested?lo:nullptr,requested?hi:nullptr);
        }return;
    }
    if(ev->type==MML_EVENT_RENDER_WORLD && ev->world){
        if(GraphicsFailed())return;
        const bool normal=S.on&&WorldMaterialMode(S.debug)&&!g_scenePending;
        bool ready=true;if(MML_HAS(H,scene_residency_state)){
            MmlSceneResidencyState state{};state.size=sizeof state;ready=H->scene_residency_state(M,&state)&&state.ready;
            if(normal&&ready)g_sceneBounds={{state.lo[0],state.lo[1],state.lo[2]},{state.hi[0],state.hi[1],state.hi[2]}};
        }
        if(ready&&(normal||g_scenePending || (S.debug>=22 && S.debug<=26))){
            uint32_t flags=MML_SCENE_COLLECT_BILLBOARDS;
            if(normal&&MML_HAS(H,scene_draw_collect_features))
                flags|=H->scene_draw_collect_features(M)&MML_SCENE_COLLECT_XZ_DISK;
            mml::scene::SnapshotBuilder::Prepared prepare=nullptr;
            if(normal&&g_deferPacked&&!g_water.planes.empty()){
                // The graphics pass checks the exact levels again if a later capture replaced them.
                g_scene.Invalidate();
                try{
                    WaterlineLevels(g_preparedWaterline.levels);
                    g_preparedWaterline.prepareFacets=g_hardwareRequested&&g_hardwareFacets;
                    prepare=PrepareWaterline;
                }catch(...){}
            }
            g_scene.Collect(H,M,g_sceneBounds,normal||g_sceneMaterials||(S.debug>=23 && S.debug<=26),true,true,normal,
                normal&&g_retainPacked,flags,normal&&g_deferPacked,prepare,&g_preparedWaterline,g_stageMeshes);
        }
        return;
    }
    if (ev->type == MML_EVENT_CONFIG_CHANGED) { ReadSettings(); g_sky.Active(S.on); return; }
    if(ev->type==MML_EVENT_DEVICE_RESET_BEGIN || ev->type==MML_EVENT_MISSION_LOADED || ev->type==MML_EVENT_GAME_LOADED){g_scene.Release();g_sceneGpu.Release();g_waterline.reset();g_waterlineGpu.Release();}
    if(ev->type==MML_EVENT_GAME_LOADED){g_scenePending=false;g_haveHistory=false;}
    if (ev->type == MML_EVENT_DEVICE_RESET_BEGIN) g_sky.Reset();
    if (ev->type == MML_EVENT_MISSION_LOADED) { Forget();g_sky.Reset(); }
}

int Status(const char *, MmlReply *r, void *) {
    // A late bridge may stop invoking Pass. Read completed queries through
    // their owning device even then, without asking it to flush.
    if(G.disjoint[0]){
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        G.disjoint[0]->GetDevice(&device);
        device->GetImmediateContext(&context);
        for(unsigned slot=0;slot<kRing;slot++)CollectTiming(context.Get(),slot);
    }
    MmlFrameStats fs{};
    fs.size = sizeof fs;
    if (!MML_HAS(H, frame_stats) || !H->frame_stats(M, &fs)) fs.frames = 0;
    std::string planes;
    for (size_t p = 0; p < g_water.planes.size(); p++) {
        char one[128];
        snprintf(one, sizeof one, "%s{\"level\":%.3f,\"triangles\":%u,\"pixels\":%lld}", p ? "," : "",
                 g_water.planes[p].level, g_water.planes[p].count,
                 g_pixels[p] == ~0ull ? -1ll : static_cast<long long>(g_pixels[p]));
        planes += one;
    }
    std::string seen = "null";
    float to[3];
    if (MML_HAS(H, scene_sun_seen) && H->scene_sun_seen(M, to)) {
        char one[96];
        snprintf(one, sizeof one, "[%.3f,%.3f,%.3f]", to[0], to[1], to[2]);
        seen = one;
    }
    std::string mats = "[";   // the last reflection's view, then its projection (row-major, row vectors)
    for (int i = 0; i < 32; i++) {
        char one[32];
        snprintf(one, sizeof one, "%s%.4f", i ? "," : "", i < 16 ? g_rview[i] : g_rproj[i - 16]);
        mats += one;
    }
    mats += "]";
    const auto *snapshot=g_scene.Current(g_scene.Cycle());
    char out[4608];
    snprintf(out, sizeof out,
             "{\"on\":%s,\"texture\":\"%s\",\"shade\":\"%s\",\"hidden\":[%d,%d],\"groups\":%u,\"captures\":%u,"
             "\"planes\":[%s],\"reflection\":{\"plane\":%d,\"level\":%.3f,\"drawn\":%u,\"mips\":%u,\"srgb\":%s},"
             "\"painted_sky\":{\"ready\":%s,\"frames\":%u},"
             "\"lighting_rebuild\":{\"environment_ready\":%s,\"environment_captures\":%u,\"object_path\":\"world geometry per segment (debug27 legacy comparison)\",\"total_object_path_m\":%.0f},"
             "\"scene_preview\":{\"debug\":%d,\"mode\":\"%s\",\"ready\":%s,\"source_frame\":%llu,\"triangles\":%zu,\"blas_triangles\":%zu,\"packed_blas_triangles\":%zu,\"stable_blas_triangles\":%zu,\"materials\":%zu,\"missing\":%zu,\"mesh_built\":%u,\"mesh_reused\":%u,\"texture_copied\":%u,\"texture_reused\":%u,\"gpu_reused\":%u,\"gpu_appended\":%u,\"gpu_uploaded_bytes\":%zu,\"gpu_texture_uploaded_bytes\":%zu,\"gpu_geometry_uploaded_bytes\":%zu,\"gpu_material_uploaded_bytes\":%zu,\"upload_ms\":%.3f,\"error\":\"%08X\",\"stage\":\"%s\"},"
             "\"scene_cpu\":{\"collect_ms\":%.3f,\"copy_ms\":%.3f,\"texture_ms\":%.3f,\"mesh_main_ms\":%.3f,\"mesh_staged_ms\":%.3f,\"pack_ms\":%.3f,\"staged_draw_ms\":%.3f,\"packed_geometry_ms\":%.3f,\"placement_ms\":%.3f,\"prune_ms\":%.3f,\"recycled_archive_bytes\":%zu,\"reused_prefix_bytes\":%zu,\"finalize_wait_ms\":%.3f,\"staged_draws\":%u,\"staged_meshes\":%u,\"staged_mesh_peak_bytes\":%zu,\"worker_builds\":%u,\"worker_jobs_on_caller\":%u,\"worker_instances\":%u,\"instances_reused\":%u,\"top_refits\":%u,\"worker_top_items\":%u,\"worker_wait_ms\":%.3f,\"mesh_cache_bytes\":%zu,\"texture_cache_bytes\":%zu},"
             "\"visibility\":{\"surface_skipped\":%u,\"reflection_skipped\":%u},"
             "\"waves\":{\"wind\":%.1f,\"from\":%.0f,\"fetch\":%.0f,\"peak_m\":%.2f,\"slope_var_drawn\":%.4f,"
             "\"slope_var_rest\":%.4f,\"height_dev_m\":%.4f},\"optics\":{\"scattering\":%.2f,\"phase\":%.2f,\"crests\":%.2f,\"depth\":%.2f},"
             "\"geometry\":{\"on\":%s,\"height\":%.2f,\"patches\":%u,\"vertices\":%llu,\"triangles\":%llu},"
             "\"wave_controls\":{\"width\":%.3f,\"speed\":%.3f,\"time\":%.6f},"
             "\"material\":{\"reflection_strength\":%.3f,\"reflection_softness\":%.3f},"
             "\"temporal\":{\"drawn\":%u,\"history\":%s},\"gpu\":{\"fft_ms\":%.3f,\"surface_ms\":%.3f,\"seed_ms\":%.3f,\"trace_ms\":%.3f,\"compose_ms\":%.3f},"
             "\"sun_seen\":%s,\"shadow_source\":\"%s\",\"reflection_matrices\":%s,\"drawn\":%u,"
             "\"frame\":{\"avg_ms\":%.2f,\"p95_ms\":%.2f,\"mod_ms\":%.3f,\"views_ms\":%.2f,\"bridge_ms\":%.2f}}",
             S.on ? "true" : "false", S.texture.c_str(), S.shade.c_str(), g_hide, g_hideShade,
             static_cast<unsigned>(g_water.groups.size()), g_captures, planes.c_str(), g_reflPlane, g_reflLevel,
             g_reflDrawn, g_refl.Mips(), g_refl.Encoded() ? "false" : "true", G.skyReady ? "true" : "false", g_skyDrawn,
             g_sky.View()?"true":"false",g_sky.Captures(),kWorldPathMetres,
             S.debug,S.debug==26?"raw native incident radiance; not final water":"diagnostic/off",
             g_sceneGpu.Ready(g_scene.Cycle(),G.madeFor)?"true":"false",
             static_cast<unsigned long long>(snapshot?snapshot->nativeFrame:0),
             snapshot?size_t(snapshot->primitiveCount):0,snapshot?snapshot->liveBlasTriangles:0,snapshot?snapshot->geometry.Triangles().size():0,
             snapshot?snapshot->StableTriangles():0,
             snapshot?snapshot->materials.size():0,snapshot?snapshot->geometry.unknown.size():0,
             snapshot?snapshot->meshBuilt:0,snapshot?snapshot->meshReused:0,snapshot?snapshot->textureCopied:0,snapshot?snapshot->textureReused:0,
             g_sceneGpu.ReusedBuffers(),g_sceneGpu.AppendedBuffers(),g_sceneGpu.UploadedBytes(),g_sceneGpu.TextureUploadedBytes(),g_sceneGpu.GeometryUploadedBytes(),
             g_sceneGpu.MaterialUploadedBytes(),g_sceneUploadMs,
             unsigned(g_sceneGpu.Error()),g_sceneGpu.ErrorStage(),
             snapshot?snapshot->nativeCollectMs:0.,snapshot?snapshot->sourceCopyMs:0.,snapshot?snapshot->textureMs:0.,snapshot?snapshot->meshBuildMs:0.,snapshot?snapshot->stagedMeshMs:0.,snapshot?snapshot->instanceBuildMs:0.,
             snapshot?snapshot->stagedDrawMs:0.,snapshot?snapshot->packedGeometryMs:0.,snapshot?snapshot->placementMs:0.,snapshot?snapshot->pruneMs:0.,
             snapshot?snapshot->recycledArchiveBytes:0,snapshot?snapshot->reusedPrefixBytes:0,
             snapshot?snapshot->finalizeWaitMs:0.,
             snapshot?snapshot->stagedDraws:0,snapshot?snapshot->stagedMeshes:0,snapshot?snapshot->stagedMeshPeakBytes:0,
             snapshot?snapshot->workerMeshBuilds:0,snapshot?snapshot->workerJobsOnCaller:0,snapshot?snapshot->workerInstances:0,
             snapshot?snapshot->instancesReused:0,snapshot?snapshot->geometry.topRefits:0,
             snapshot?snapshot->workerTopItems:0,snapshot?snapshot->workerWaitMs:0.,snapshot?snapshot->cachedMeshBytes:0,snapshot?snapshot->cachedTextureBytes:0,
             g_offscreen, g_reflectionRejected, S.wind, S.windFrom, S.fetch,
             g_spectrum.peak, g_spectrum.drawn, 2 * g_spectrum.rest, std::sqrt(g_spectrum.heightVar), S.scattering, S.phase,
             S.crests, S.depth, S.geometry ? "true" : "false", S.waveHeight, static_cast<unsigned>(g_patches.size() / 9),
             G.geometryStats.DSInvocations, G.geometryStats.CPrimitives,
             S.waveWidth, S.waveSpeed, g_waveClock.time, S.reflectionStrength, S.reflectionSoftness,
             g_temporalDrawn, g_haveHistory ? "true" : "false",
             G.fftMs, G.surfaceMs, G.seedMs, G.traceMs, G.composeMs,
             seen.c_str(), S.shadowSource == 0 ? "sun" : "scene", mats.c_str(), g_drawn, fs.avg_ms, fs.p95_ms, fs.mod_ms,
             fs.views_ms, fs.bridge_ms);
    std::string result=out;result.pop_back();
    MmlSceneResidencyState residency{};residency.size=sizeof residency;
    const bool residence=MML_HAS(H,scene_residency_state)&&H->scene_residency_state(M,&residency);
    char region[300];snprintf(region,sizeof region,",\"source_residency\":{\"supported\":%s,\"requested\":%u,\"ready\":%u,\"required\":%u,\"available\":%u,\"source_current\":%s}}",
        residence?"true":"false",residency.requested,residency.ready,residency.required,residency.available,snapshot?"true":"false");
    result+=region;result.pop_back();char line[420];
    snprintf(line,sizeof line,",\"waterline\":{\"ready\":%s,\"elements\":%zu,\"definite\":%u,\"uncertain\":%u,\"nodes\":%zu,\"build_ms\":%.3f,\"gpu_ready\":%s,\"gpu_error\":\"%08X\",\"gpu_stage\":\"%s\",\"uncertain_sources\":[",
        g_waterline?"true":"false",g_waterline?g_waterline->elements.size():0,g_waterline?g_waterline->definite:0,g_waterline?g_waterline->uncertain:0,g_waterline?g_waterline->nodes.size():0,g_waterlineBuildMs,
        g_waterlineGpu.Ready(g_scene.Cycle(),G.madeFor)?"true":"false",unsigned(g_waterlineGpu.Error()),g_waterlineGpu.ErrorStage());
    result+=line;if(g_waterline)for(size_t i=0;i<g_waterline->uncertainSources.size();i++){
        const auto &s=g_waterline->uncertainSources[i];char info[380];
        snprintf(info,sizeof info,"%s{\"visual\":\"0x%08llX\",\"material\":%u,\"alpha_test\":%u,\"alpha_function\":%u,\"reference\":%u,\"blend\":%u,\"count\":%u,\"at\":[%.4f,%.4f,%.4f]}",
            i?",":"",static_cast<unsigned long long>(s.visual),s.material,s.alphaTest,s.alphaFunction,s.alphaReference,s.blend,s.count,s.at[0],s.at[1],s.at[2]);result+=info;
    }result+="]}}";H->reply_append(r,result.c_str(),result.size());
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, geom_capture)) return 1;   // a loader without the game's geometry
    if(!MML_HAS(H,scene_residency_camera)||!MML_HAS(H,scene_draw_billboard)){
        host->log(mod,MML_LOG_ERROR,"water reflections require current native scene collection/residency support; update the loader");return 4;
    }
    QueryPerformanceCounter(&g_t0);
    QueryPerformanceFrequency(&g_freq);
    g_water.maxPlanes = kMaxPlanes;
    ReadSettings();
    if(MML_HAS(H,channel_wait)){
        g_hardwareRequest=H->channel_open(M,"hardware_request",sizeof(river::HardwareRequest));
        g_hardwareReply=H->channel_open(M,"hardware_reply",sizeof(mml::gfx::TransferReply));
        g_hardwareStatus=H->channel_open(M,"hardware_status",sizeof(river::HardwareStatus));
    }
    if(!g_sky.Create(host,mod,512)) {
        host->log(mod,MML_LOG_ERROR,"water lighting requires native sky-only scene-view support; update the loader");
        return 4;
    }
    g_sky.Active(S.on);
    g_pass = H->gfx_pass(M, -120, Pass, nullptr);   // before the upscaler: the water and its mask enter DLSS with their frame
    g_view = H->scene_view(M, 0, 0, PlaceReflection, nullptr);
    H->subscribe(M, MML_EVENT_CONFIG_CHANGED, OnEvent, nullptr);
    H->subscribe(M, MML_EVENT_MISSION_LOADED, OnEvent, nullptr);
    H->subscribe(M, MML_EVENT_DEVICE_RESET_BEGIN, OnEvent, nullptr);
    H->subscribe(M,MML_EVENT_RENDER_BEGIN,OnEvent,nullptr);
    H->subscribe(M,MML_EVENT_GAME_LOADED,OnEvent,nullptr);
    H->subscribe(M,MML_EVENT_RENDER_WORLD,OnEvent,nullptr);
    H->command(M, "status", "the water: its planes and their pixels, the reflection, the waves, the frame's cost",
               Status, nullptr);
    H->command(M,"capture_clock","diagnostic only: hold both wave/foam time at seconds, or release true; no setting is saved",CaptureClock,nullptr);
    H->command(M,"resolve_mode","compare the second geometry pass with on false or the original seed with wave_bounds false; unsaved",ResolveMode,nullptr);
    H->command(M,"hardware_mode","diagnostic only: opt-in same-frame DXR worker, no setting is saved",HardwareMode,nullptr);
    H->command(M,"packing_mode","temporary comparisons: on retains CPU data, gpu retains GPU buffers, defer overlaps owned CPU finalization",PackingMode,nullptr);
    H->command(M,"scene_probe","geometry diagnostic only: capture true, box [lo3,hi3], ray [origin3,direction3,tMax], or release true",SceneProbe,nullptr);
    return g_pass ? 0 : 3;
}

MML_EXPORT void mml_mod_unload(MmlMod *) { g_scene.Release();g_sceneGpu.Release();g_sky.Release();Release(); }
