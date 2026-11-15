#include "mml/mml.h"
uint32_t kit_environment_features_c(const MmlHost *host,MmlMod *mod) {
    return MML_HAS(host,scene_view_features) ? host->scene_view_features(mod) : 0;
}
unsigned kit_scene_collect_c(const MmlHost *host,MmlMod *mod,const float lo[3],const float hi[3],MmlSceneCollection *out){
    unsigned result=0;MmlSceneCollection small={0};
    if(!MML_HAS(host,scene_draw_collect))return 0;
    if(!host->scene_draw_collect(mod,lo,hi,&small)&&small.size==0)result|=1;
    if(host->scene_draw_collect(mod,lo,hi,out))result|=2;
    return result;
}
unsigned kit_scene_disk_c(const MmlHost *host,MmlMod *mod,const float lo[3],const float hi[3],MmlSceneCollection *out){
    const uint32_t flag=MML_SCENE_COLLECT_XZ_DISK;
    if(!MML_HAS(host,scene_draw_collect_features)||!(host->scene_draw_collect_features(mod)&flag))return 0;
    if(host->scene_draw_collect_ex(mod,lo,hi,flag|0x80000000u,out))return 1;
    return host->scene_draw_collect_ex(mod,lo,hi,flag,out)?3:1;
}
static void kit_scene_owned_increment(void *user){++*(unsigned *)user;}
int kit_scene_owned_c(const MmlHost *host,MmlMod *mod){
    unsigned called=0;
    if(!MML_HAS(host,scene_owned_join))return 0;
    uint64_t first=host->scene_owned_submit(mod,kit_scene_owned_increment,&called);
    if(!first)return 0;
    const uint64_t duplicate=host->scene_owned_submit(mod,kit_scene_owned_increment,&called);
    const int completed=host->scene_owned_join(mod,first,0);
    const int stale=host->scene_owned_join(mod,first,0);
    if(duplicate)host->scene_owned_join(mod,duplicate,1);
    return completed&&!duplicate&&!stale&&called==1;
}
unsigned kit_scene_source_c(const MmlHost *host,MmlMod *mod,const MmlSceneDraw *draw){
    unsigned result=0,i;MmlSceneGeometry geometry={0};
    if(draw&&draw->size>=sizeof(MmlSceneDraw)&&draw->stage_stride>=sizeof(MmlSceneTextureStage))result|=1;
    if(!MML_HAS(host,scene_draw_geometry))return result;
    if(!host->scene_draw_geometry(mod,draw,&geometry)&&geometry.size==0)result|=2;
    geometry.size=sizeof geometry;
    if(!host->scene_draw_geometry(mod,draw,&geometry))return result;
    if(geometry.vertex_stride<sizeof(MmlSceneVertex))return result;
    for(i=0;i<geometry.triangles*3;i++)if(geometry.index[i]>=geometry.vertices)return result;
    return result|4;
}
unsigned kit_scene_billboard_c(const MmlHost *host,MmlMod *mod,const MmlSceneDraw *draw){
    MmlSceneBillboard value={0};unsigned result=0;
    if(!MML_HAS(host,scene_draw_billboard))return 0;
    if(!host->scene_draw_billboard(mod,draw,&value)&&value.size==0)result|=1;
    value.size=sizeof value;
    if(host->scene_draw_billboard(mod,draw,&value)&&value.world[15]==1&&
        ((value.axis==2&&value.mode==0)||(value.axis==1&&value.mode==1)))result|=2;
    return result;
}
unsigned kit_scene_residency_c(const MmlHost *host,MmlMod *mod){
    unsigned result=0;MmlSceneResidencyState state={0};float lo[3]={2,0,0},hi[3]={1,0,0};
    if(!MML_HAS(host,scene_residency_state)||!MML_HAS(host,scene_residency))return 0;
    if(!host->scene_residency_state(mod,&state)&&state.size==0)result|=1;
    state.size=sizeof state;if(host->scene_residency_state(mod,&state))result|=2;
    if(!host->scene_residency(mod,lo,hi))result|=4;
    if(MML_HAS(host,scene_residency_camera)&&!host->scene_residency_camera(mod,-1)&&!host->scene_residency_camera(mod,10001))result|=8;
    return result;
}
