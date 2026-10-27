#include "solar_os_jet_abi.h"
#include "Framebuffer.hpp"
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <vector>
#include <cstring>
extern "C" const solar_os_jet_api_v1_t *solar_os_jet_engine_api_v1(void);
extern "C" bool solar_os_jet_engine_idle(void);
static long requests,fail_at=-1,live;
void *operator new(size_t bytes) {
    if(requests++==fail_at) throw std::bad_alloc();
    void *p=std::malloc(bytes?bytes:1);if(!p)throw std::bad_alloc();++live;return p;
}
void operator delete(void *p) noexcept {if(p){--live;std::free(p);}}
void operator delete(void *p,size_t) noexcept {operator delete(p);}
void *operator new[](size_t n) {return operator new(n);}
void operator delete[](void *p) noexcept {operator delete(p);}
void operator delete[](void *p,size_t) noexcept {operator delete(p);}
void *operator new(size_t n,const std::nothrow_t &) noexcept {try{return operator new(n);}catch(...){return nullptr;}}
void *operator new[](size_t n,const std::nothrow_t &) noexcept {return operator new(n,std::nothrow);}
void operator delete(void *p,const std::nothrow_t &) noexcept {operator delete(p);}
void operator delete[](void *p,const std::nothrow_t &) noexcept {operator delete(p);}
static bool cancellation(void *user) {return ++*static_cast<unsigned *>(user)==4;}
static void background_cache_validation(const solar_os_jet_api_v1_t *api,void *scene,
                                        uint16_t *color,size_t bytes) {
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    q.opcode=SOLAR_OS_JET_INFO;assert(execute()==0&&(r.handle&SOLAR_OS_JET_FEATURE_BACKGROUND_CACHE));
    uint32_t cube=0;assert(api->cube(scene,150,150,150,0xff0000,&cube)==0);
    q={};q.opcode=SOLAR_OS_JET_SCENE_SET;q.fields=1U<<JET_SCENE_background_cache;
    q.values[JET_SCENE_background_cache]=1;assert(execute()==0);
    assert(api->render(scene,color,bytes,nullptr,nullptr)==0);
    std::vector<uint16_t> cached(color,color+bytes/2),other(bytes/2,0);
    assert(api->remove(scene,cube)==0);
    assert(api->render(scene,other.data(),bytes,nullptr,nullptr)==0&&other==cached);
    q={};q.opcode=SOLAR_OS_JET_SCENE_SET;q.fields=1U<<JET_SCENE_background_cache;
    q.values[JET_SCENE_background_cache]=1;assert(execute()==0);
    assert(api->render(scene,color,bytes,[](void *){return true;},nullptr)==SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(api->render(scene,other.data(),bytes,nullptr,nullptr)==0&&other==cached);
    // A changed foreground leaves no trails; geometry behind the captured
    // cube must still be rejected by the retained depth buffer.
    assert(api->cube(scene,100,100,100,0x00ff00,&cube)==0);
    solar_os_jet_pose_t pose={0,0,200,0,0,0};assert(api->transform(scene,cube,&pose)==0);
    assert(api->render(scene,color,bytes,nullptr,nullptr)==0);
    assert(color[24*64+32]==cached[24*64+32]);
    pose.z=-200;assert(api->transform(scene,cube,&pose)==0);
    assert(api->render(scene,color,bytes,nullptr,nullptr)==0&&color[24*64+32]!=cached[24*64+32]);
    assert(api->remove(scene,cube)==0);
    assert(api->render(scene,other.data(),bytes,nullptr,nullptr)==0&&other==cached);
    // Both allocation failures preserve the previous cache and settings.
    for(long failure=0;failure<2;++failure) {
        q={};q.opcode=SOLAR_OS_JET_SCENE_SET;q.fields=1U<<JET_SCENE_background_cache;
        q.values[JET_SCENE_background_cache]=1;fail_at=requests+failure;
        assert(execute()==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);fail_at=-1;
        assert(api->render(scene,other.data(),bytes,nullptr,nullptr)==0&&other==cached);
    }
    q={};q.opcode=SOLAR_OS_JET_SCENE_SET;q.fields=1U<<JET_SCENE_clear;
    assert(execute()==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    q={};q.opcode=SOLAR_OS_JET_SCENE_SET;q.fields=1U<<JET_SCENE_wireframe;q.values[JET_SCENE_wireframe]=1;
    assert(execute()==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    q={};q.opcode=SOLAR_OS_JET_SCENE_SET;q.fields=1U<<JET_SCENE_background_cache;
    assert(execute()==0&&api->render(scene,color,bytes,nullptr,nullptr)==0);
    for(size_t i=0;i<bytes/2;++i)assert(color[i]==0xffff);
    puts("Jet background cache: buffer exchange, depth occlusion, no trails, cancellation and allocation failures passed");
}
static void controls_validation(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) {
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    q.opcode=SOLAR_OS_JET_SCENE_SET;q.fields=(1U<<JET_SCENE_texture_lod)|(1U<<JET_SCENE_clear);
    q.values[JET_SCENE_texture_lod]=1;assert(execute()==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    q={};q.opcode=SOLAR_OS_JET_SCENE_INFO;assert(execute()==0&&r.values[JET_SCENE_clear]==1&&!r.values[JET_SCENE_texture_lod]);
    const uint8_t red[]={248,0,248,0,248,0,248,0};
    q={};q.opcode=SOLAR_OS_JET_TEXTURE;q.values[16]=q.values[17]=2;q.data=red;q.bytes=sizeof(red);
    assert(execute()==0);uint32_t texture=r.handle;
    q={};q.opcode=SOLAR_OS_JET_MATERIAL;q.fields=(1U<<JET_MAT_texture)|(1U<<JET_MAT_color)|(1U<<JET_MAT_shading);
    q.values[JET_MAT_texture]=texture;q.values[JET_MAT_color]=0x00ff00;q.values[JET_MAT_shading]=4;
    assert(execute()==0);uint32_t material=r.handle,cube=0;assert(api->cube(scene,150,150,150,0xffffff,&cube)==0);
    q={};q.opcode=SOLAR_OS_JET_ASSIGN;q.handle=cube;q.values[0]=material;assert(execute()==0);
    assert(api->render(scene,color,bytes,nullptr,nullptr)==0&&Renderer::framebufferDecode(color[24*64+32])==0xf800);
    q={};q.opcode=SOLAR_OS_JET_SCENE_SET;q.fields=(1U<<JET_SCENE_texture_lod)|(1U<<JET_SCENE_texture_far);
    q.values[JET_SCENE_texture_lod]=q.values[JET_SCENE_texture_far]=1;assert(execute()==0);
    assert(api->render(scene,color,bytes,nullptr,nullptr)==0&&Renderer::framebufferDecode(color[24*64+32])==0x07e0);
    q={};q.opcode=SOLAR_OS_JET_SCENE_SET;q.fields=1U<<JET_SCENE_wireframe;q.values[JET_SCENE_wireframe]=1;
    assert(execute()==0&&api->render(scene,color,bytes,nullptr,nullptr)==0&&color[0]==0);
    q={};q.opcode=SOLAR_OS_JET_SCENE_SET;q.fields=(1U<<JET_SCENE_wireframe)|(1U<<JET_SCENE_texture_lod);
    assert(execute()==0&&api->render(scene,color,bytes,nullptr,nullptr)==0);
    assert(color[0]==0xffff&&Renderer::framebufferDecode(color[24*64+32])==0xf800);
}
struct ShaderProbe {unsigned calls=0;bool fail=false;};
static int shader_probe(void *user,uint32_t slot,double u,double v,
    const solar_os_jet_shader_parameters_t *p,uint32_t *color) {
    auto &probe=*static_cast<ShaderProbe *>(user);++probe.calls;
    assert(slot==7&&p&&p->screen_x>=0&&p->screen_x<64&&p->screen_y>=0&&p->screen_y<48);
    assert(u>-2&&u<2&&v>-2&&v<2&&p->depth>0);
    for(unsigned i=0;i<3;++i)assert(std::abs(p->local[i]-p->world[i])<=1);
    *color=0x00ff00;return probe.fail ? SOLAR_OS_NATIVE_ERROR_FAILED : 0;
}
static int shader_chain(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) {
    ShaderProbe probe;solar_os_jet_shader_callback_t callback={shader_probe,&probe,7};
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    q.opcode=SOLAR_OS_JET_SHADER;q.fields=1U<<JET_SHADER_kind;q.values[JET_SHADER_kind]=3;
    q.data=&callback;q.bytes=sizeof(callback);int err=execute();if(err)return err;uint32_t shader=r.handle;
    q={};q.opcode=SOLAR_OS_JET_MATERIAL;q.fields=(1U<<JET_MAT_shader)|(1U<<JET_MAT_shading);
    q.values[JET_MAT_shader]=shader;q.values[JET_MAT_shading]=4;err=execute();if(err)return err;uint32_t mat=r.handle;
    uint32_t cube=0;err=api->cube(scene,150,150,150,0xffffff,&cube);if(err)return err;
    q={};q.opcode=SOLAR_OS_JET_ASSIGN;q.handle=cube;q.values[0]=mat;err=execute();if(err)return err;
    err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;assert(probe.calls>10);
    assert(api->remove(scene,shader)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    probe.fail=true;unsigned before=probe.calls;
    assert(api->render(scene,color,bytes,nullptr,nullptr)==SOLAR_OS_NATIVE_ERROR_FAILED&&probe.calls==before+1);
    probe.fail=false;assert(api->render(scene,color,bytes,nullptr,nullptr)==0);
    for(unsigned kind=0;kind<3;++kind) {
        q={};q.opcode=SOLAR_OS_JET_SHADER_SET;q.handle=shader;q.fields=1U<<JET_SHADER_kind;q.values[JET_SHADER_kind]=kind;
        assert(execute()==0);assert(api->render(scene,color,bytes,nullptr,nullptr)==0);
    }
    assert(api->remove(scene,cube)==0&&api->remove(scene,mat)==0&&api->remove(scene,shader)==0);return 0;
}
static int surface_chain(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) {
    const uint8_t pixels[]={255,0,0,0,255,0,0,0,255,255,255,0};
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    q.opcode=SOLAR_OS_JET_TEXTURE;q.values[16]=q.values[17]=2;q.data=pixels;q.bytes=sizeof(pixels);
    q.fields=1U<<JET_TEX_format;q.values[JET_TEX_format]=1;
    int err=api->extensions->execute(scene,&q,&r);if(err)return err;uint32_t tex=r.handle;
    q={};q.opcode=SOLAR_OS_JET_TEXTURE_SET;q.handle=tex;q.fields=1U<<JET_TEX_offset;q.values[JET_TEX_offset]=1;
    long before=requests;err=api->extensions->execute(scene,&q,&r);
    assert(!err&&requests==before); // Palette/sampling animation must not copy or allocate pixels.
    q={};q.opcode=SOLAR_OS_JET_MATERIAL;q.fields=(1U<<JET_MAT_texture)|(1U<<JET_MAT_shading);
    q.values[JET_MAT_texture]=tex;q.values[JET_MAT_shading]=4;
    err=api->extensions->execute(scene,&q,&r);if(err)return err;uint32_t mat=r.handle,cube=0;
    err=api->cube(scene,150,150,150,0xffffff,&cube);if(err)return err;
    q={};q.opcode=SOLAR_OS_JET_ASSIGN;q.handle=cube;q.values[0]=mat;
    err=api->extensions->execute(scene,&q,&r);if(err)return err;
    err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;
    assert(api->remove(scene,tex)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->remove(scene,mat)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    // Updating a live texture preserves its address and all material references.
    q={};q.opcode=SOLAR_OS_JET_TEXTURE_SET;q.handle=tex;q.data=pixels;q.bytes=sizeof(pixels);
    err=api->extensions->execute(scene,&q,&r);if(err)return err;
    for(unsigned shading=0;shading<7;++shading) {
        q={};q.opcode=SOLAR_OS_JET_MATERIAL_SET;q.handle=mat;q.fields=1U<<JET_MAT_shading;q.values[JET_MAT_shading]=shading;
        err=api->extensions->execute(scene,&q,&r);if(err)return err;
        err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;
    }
    assert(api->remove(scene,cube)==0&&api->remove(scene,mat)==0&&api->remove(scene,tex)==0);
    return 0;
}
static int scene_chain(const solar_os_jet_api_v1_t *api,void *scene) {
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    q.opcode=SOLAR_OS_JET_PRIMITIVE;q.fields=1U<<JET_PRIM_kind;q.values[JET_PRIM_kind]=9;
    int err=execute();if(err)return err;uint32_t object=r.handle;
    q={};q.opcode=SOLAR_OS_JET_OBJECT_ACTION;q.handle=object;
    q.fields=1U<<JET_OACT_action;q.values[JET_OACT_action]=7;
    err=execute();if(err)return err;uint32_t copy=r.handle;
    assert(api->remove(scene,object)==0);
    q.handle=copy;q.values[JET_OACT_action]=3;err=execute();if(err)return err;
    q.values[JET_OACT_action]=5;err=execute();if(err)return err;
    q={};q.opcode=SOLAR_OS_JET_POINT_LIGHT;err=execute();if(err)return err;
    uint32_t light=r.handle;
    uint32_t stops[]={0x102030,0xffffff};q={};q.opcode=SOLAR_OS_JET_GRADIENT;q.data=stops;q.bytes=sizeof(stops);
    err=execute();if(err)return err;
    solar_os_jet_mesh_vertex_t vertices[]={{-20,-20,0,0,0,0,0,-1024},{0,20,0,0,0,0,0,-1024},{20,-20,0,0,0,0,0,-1024}};
    solar_os_jet_mesh_face_t faces[]={{0,1,2,0,0}};
    q={};q.opcode=SOLAR_OS_JET_MESH_DATA;q.handle=copy;q.data=vertices;q.bytes=sizeof(vertices);q.extra=faces;q.extra_bytes=sizeof(faces);
    err=execute();if(err) {
        q={};q.opcode=SOLAR_OS_JET_OBJECT_INFO;q.handle=copy;assert(!execute()&&r.handles[0]==12);
        return err;
    }
    assert(api->remove(scene,copy)==0&&api->remove(scene,light)==0);return 0;
}
static int overlay_chain(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) {
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    q.opcode=SOLAR_OS_JET_SPRITE;q.fields=(1U<<JET_SPR_color)|(1U<<JET_SPR_width)|(1U<<JET_SPR_height);
    q.values[JET_SPR_color]=0xff0000;q.values[JET_SPR_width]=10;q.values[JET_SPR_height]=10;
    int err=execute();if(err)return err;uint32_t sprite=r.handle;
    uint32_t cube;err=api->cube(scene,150,150,150,0xffffff,&cube);if(err)return err;
    solar_os_jet_pick_t point={32,24};q={};q.opcode=SOLAR_OS_JET_PICK;q.data=&point;q.bytes=sizeof(point);
    err=execute();if(err)return err;
    err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;
    assert(reinterpret_cast<uint8_t *>(color)[0]==0xf8&&reinterpret_cast<uint8_t *>(color)[1]==0x00);
    q={};q.opcode=SOLAR_OS_JET_PICK_RESULT;assert(execute()==0&&r.count==1&&r.handle==cube&&r.values[0]==1);
    assert(api->remove(scene,cube)==0);assert(execute()==0&&r.handle==cube); // Last completed frame, never a dangling pointer.
    assert(api->remove(scene,sprite)==0);return 0;
}
static int instance_chain(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) {
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    q.opcode=SOLAR_OS_JET_PRIMITIVE;q.fields=1U<<JET_PRIM_kind;q.values[JET_PRIM_kind]=9;
    int err=execute();if(err)return err;uint32_t source=r.handle;
    q={};q.opcode=SOLAR_OS_JET_PROTOTYPE;q.handle=source;err=execute();if(err)return err;uint32_t prototype=r.handle;
    assert(api->remove(scene,source)==0);
    q={};q.opcode=SOLAR_OS_JET_MATERIAL;err=execute();if(err)return err;uint32_t mat=r.handle;
    q={};q.opcode=SOLAR_OS_JET_INSTANCE;q.values[16]=prototype;q.fields=1U<<JET_INST_x;q.values[JET_INST_x]=-50;
    err=execute();if(err)return err;uint32_t batch=r.handle;
    uint32_t overrides[12];for(auto &id:overrides)id=mat;
    q={};q.opcode=SOLAR_OS_JET_INSTANCE_ADD;q.handle=batch;q.values[16]=prototype;
    q.fields=(1U<<JET_INST_x)|(1U<<JET_INST_materials);q.values[JET_INST_x]=50;q.extra=overrides;q.extra_bytes=sizeof(overrides);
    err=execute();if(err)return err;assert(r.count==1);
    q={};q.opcode=SOLAR_OS_JET_OBJECT_ACTION;q.handle=batch;q.fields=1U<<JET_OACT_action;q.values[JET_OACT_action]=4;
    err=execute();if(err)return err;
    q.values[JET_OACT_action]=7;err=execute();if(err)return err;uint32_t clone=r.handle;
    assert(api->remove(scene,batch)==0&&api->remove(scene,prototype)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->remove(scene,mat)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;
    assert(api->remove(scene,clone)==0&&api->remove(scene,prototype)==0&&api->remove(scene,mat)==0);return 0;
}
static int extras_chain(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) {
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    q.opcode=SOLAR_OS_JET_PARTICLES;int err=execute();if(err)return err;uint32_t pool=r.handle;
    q={};q.opcode=SOLAR_OS_JET_PARTICLES_EMIT;q.handle=pool;long before=requests;
    assert(execute()==0&&r.count==24&&requests==before);
    q={};q.opcode=SOLAR_OS_JET_MATERIAL;err=execute();if(err)return err;uint32_t mat=r.handle;
    solar_os_jet_element_t elements[SOLAR_OS_JET_MAX_FLARE_ELEMENTS]={};
    for(auto &element:elements) {element.fields=1U<<JET_ELEM_material;element.values[JET_ELEM_material]=mat;}
    q={};q.opcode=SOLAR_OS_JET_FLARE;q.data=elements;q.bytes=sizeof(elements);
    err=execute();if(err)return err;uint32_t flare=r.handle;
    assert(api->remove(scene,mat)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    solar_os_jet_pick_t point={32,24};q={};q.opcode=SOLAR_OS_JET_PICK;q.data=&point;q.bytes=sizeof(point);assert(execute()==0);
    q={};q.opcode=SOLAR_OS_JET_STEP;q.values[0]=.05;assert(execute()==0);
    err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;
    q={};q.opcode=SOLAR_OS_JET_PICK_RESULT;assert(execute()==0&&r.count==1&&r.values[0]==0);
    assert(api->remove(scene,flare)==0&&api->remove(scene,mat)==0&&api->remove(scene,pool)==0);return 0;
}
static int postfx_chain(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) {
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    q.opcode=SOLAR_OS_JET_POSTFX_INFO;assert(execute()==0&&r.values[16]==0&&r.values[JET_FX_pixelate]==1);
    q={};q.opcode=SOLAR_OS_JET_POSTFX;q.fields=(1U<<JET_FX_COUNT)-1;
    q.values[JET_FX_fxaa]=q.values[JET_FX_bloom]=1;q.values[JET_FX_crt]=48;
    q.values[JET_FX_motion_blur]=50;q.values[JET_FX_chromatic]=2;q.values[JET_FX_pixelate]=4;q.values[JET_FX_cel_bits]=4;
    int err=execute();if(err) {
        q={};q.opcode=SOLAR_OS_JET_POSTFX_INFO;assert(execute()==0&&r.values[16]==0&&r.values[JET_FX_cel_bits]==0);return err;
    }
    long before=requests;
    for(int frame=0;frame<3;++frame) {err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;}
    assert(requests==before); // Enabled effects reuse their prepared scratch.
    q={};q.opcode=SOLAR_OS_JET_POSTFX_INFO;assert(execute()==0&&r.values[16]==64*48*6+64*10&&r.values[17]);
    q.opcode=SOLAR_OS_JET_POSTFX_CLEAR;assert(execute()==0);
    q.opcode=SOLAR_OS_JET_POSTFX_INFO;assert(execute()==0&&r.values[16]==0&&r.values[JET_FX_cel_bits]==0);return 0;
}
static int tile_chain(const solar_os_jet_api_v1_t *api,void *scene,uint16_t *color,size_t bytes) {
    solar_os_jet_command_t q={};solar_os_jet_reply_t r;
    auto execute=[&] {return api->extensions->execute(scene,&q,&r);};
    uint8_t pixels[]={0,1,2,3};uint32_t palette[]={0xff0000,0x00ff00,0x0000ff,0xffffff};
    q.opcode=SOLAR_OS_JET_TEXTURE;q.data=pixels;q.bytes=sizeof(pixels);q.extra=palette;q.extra_bytes=sizeof(palette);
    q.values[16]=q.values[17]=2;q.fields=(1U<<JET_TEX_format)|(1U<<JET_TEX_tiled);q.values[JET_TEX_format]=2;q.values[JET_TEX_tiled]=1;
    int err=execute();if(err)return err;uint32_t texture=r.handle;
    q={};q.opcode=SOLAR_OS_JET_TEXTURE_SET;q.handle=texture;q.fields=1U<<JET_TEX_offset;q.values[JET_TEX_offset]=1;
    long before=requests;assert(execute()==0&&requests==before); // Uncached palette animation stays allocation-free.
    q={};q.opcode=SOLAR_OS_JET_SAMPLE_TEXTURE;q.handle=texture;assert(execute()==0&&r.values[0]==0x00ff00);
    q={};q.opcode=SOLAR_OS_JET_MATERIAL;q.fields=1U<<JET_MAT_texture;q.values[JET_MAT_texture]=texture;
    err=execute();if(err)return err;uint32_t mat=r.handle,cube;
    err=api->cube(scene,150,150,150,0xffffff,&cube);if(err)return err;
    q={};q.opcode=SOLAR_OS_JET_ASSIGN;q.handle=cube;q.values[0]=mat;assert(execute()==0);
    err=api->render(scene,color,bytes,nullptr,nullptr);if(err)return err;
    q={};q.opcode=SOLAR_OS_JET_TEXTURE_SET;q.handle=texture;q.fields=1U<<JET_TEX_filter;q.values[JET_TEX_filter]=3;
    err=execute();if(err) {
        q={};q.opcode=SOLAR_OS_JET_TEXTURE_INFO;q.handle=texture;
        assert(execute()==0&&r.values[JET_TEX_filter]==0&&r.values[20]==0);return err;
    }
    q={};q.opcode=SOLAR_OS_JET_SAMPLE_TEXTURE;q.handle=texture;assert(execute()==0&&r.values[0]==0x00ff00);
    assert(api->remove(scene,cube)==0&&api->remove(scene,mat)==0&&api->remove(scene,texture)==0);return 0;
}
#include "jet_assets_test.inc"
int main() {
    auto *api=solar_os_jet_engine_api_v1();assert(api->abi_version==1&&api->struct_size==sizeof(*api));
    std::vector<uint16_t> color(64*48),depth(64*48);size_t bytes=color.size()*2;
    long baseline=live;bool complete=false;
    for(long failure=0;failure<150;++failure) {
        requests=0;fail_at=failure;void *scene=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&scene);
        if(!err) {
            uint32_t object=0;err=api->cube(scene,150,150,150,0x2192ff,&object);
            if(!err) err=api->render(scene,color.data(),bytes,nullptr,nullptr);
            assert(api->destroy(scene)==0);
        }
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;void *scene=nullptr,*second=nullptr;
    complete=false;
    for(long failure=0;failure<250;++failure) {
        requests=0;fail_at=failure;void *s=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
        if(!err) {err=surface_chain(api,s,color.data(),bytes);assert(api->destroy(s)==0);}
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;
    complete=false;
    for(long failure=0;failure<250;++failure) {
        requests=0;fail_at=failure;void *s=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
        if(!err) {err=scene_chain(api,s);assert(api->destroy(s)==0);}
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;
    complete=false;
    for(long failure=0;failure<250;++failure) {
        requests=0;fail_at=failure;void *s=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
        if(!err) {err=overlay_chain(api,s,color.data(),bytes);assert(api->destroy(s)==0);}
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;
    complete=false;
    for(long failure=0;failure<250;++failure) {
        requests=0;fail_at=failure;void *s=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
        if(!err) {err=asset_chain(api,s,color.data(),bytes);assert(api->destroy(s)==0);}
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;
    complete=false;
    for(long failure=0;failure<250;++failure) {
        requests=0;fail_at=failure;void *s=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
        if(!err) {err=instance_chain(api,s,color.data(),bytes);assert(api->destroy(s)==0);}
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;
    complete=false;
    for(long failure=0;failure<250;++failure) {
        requests=0;fail_at=failure;void *s=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
        if(!err) {err=extras_chain(api,s,color.data(),bytes);assert(api->destroy(s)==0);}
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;
    complete=false;
    for(long failure=0;failure<250;++failure) {
        requests=0;fail_at=failure;void *s=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
        if(!err) {err=postfx_chain(api,s,color.data(),bytes);assert(api->destroy(s)==0);}
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;
    complete=false;
    for(long failure=0;failure<250;++failure) {
        requests=0;fail_at=failure;void *s=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
        if(!err) {err=tile_chain(api,s,color.data(),bytes);assert(api->destroy(s)==0);}
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;
    complete=false;
    for(long failure=0;failure<250;++failure) {
        requests=0;fail_at=failure;void *s=nullptr;
        int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
        if(!err) {err=shader_chain(api,s,color.data(),bytes);assert(api->destroy(s)==0);}
        assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
        assert(solar_os_jet_engine_idle()&&live==baseline);
        if(requests<=failure){complete=true;break;}
    }
    assert(complete);fail_at=-1;
    // Every constructor must release partially built geometry on allocation failure.
    for(unsigned kind=0;kind<10;++kind) {
        complete=false;
        for(long failure=0;failure<250;++failure) {
            requests=0;fail_at=failure;void *s=nullptr;
            int err=api->create(64,48,color.data(),depth.data(),bytes,&s);
            if(!err) {
                solar_os_jet_command_t q={};solar_os_jet_reply_t r;
                q.opcode=SOLAR_OS_JET_PRIMITIVE;q.fields=1U<<JET_PRIM_kind;q.values[JET_PRIM_kind]=kind;
                err=api->extensions->execute(s,&q,&r);
                if(!err) err=api->render(s,color.data(),bytes,nullptr,nullptr);
                assert(api->destroy(s)==0);
            }
            assert(err==0||err==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
            assert(solar_os_jet_engine_idle()&&live==baseline);
            if(requests<=failure){complete=true;break;}
        }
        assert(complete);fail_at=-1;
    }
    assert(api->create(63,48,color.data(),depth.data(),bytes,&scene)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->create(64,48,color.data(),depth.data(),bytes,&scene)==0);
    assert(api->create(64,48,color.data(),depth.data(),bytes,&second)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE&&!second);
    uint32_t cube=0;assert(api->cube(scene,150,150,150,0x2192ff,&cube)==0);
    solar_os_jet_pose_t pose={-55,0,100,15,30,15};assert(api->transform(scene,cube,&pose)==0);
    assert(api->render(scene,color.data(),bytes,nullptr,nullptr)==0);
    auto expected=color;unsigned callbacks=0;
    assert(api->render(scene,color.data(),bytes,cancellation,&callbacks)==SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(api->render(scene,color.data(),bytes,nullptr,nullptr)==0&&color==expected);
    solar_os_jet_vertex_t vertices[]={{-50,-50,0},{50,-50,0},{0,50,0}};
    solar_os_jet_triangle_t triangle[]={{0,1,3}};uint32_t mesh=0;
    assert(api->mesh(scene,vertices,3,triangle,1,0xff0000,&mesh)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    triangle[0].c=2;assert(api->mesh(scene,vertices,3,triangle,1,0xff0000,&mesh)==0);
    assert(api->remove(scene,mesh)==0&&api->remove(scene,mesh)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->transform(scene,mesh,&pose)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->background(scene,0)==0&&api->background(scene,0x1000000)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->remove(scene,cube)==0);
    assert(api->render(scene,color.data(),bytes,nullptr,nullptr)==0);
    for(auto pixel:color)assert(pixel==0);
    asset_validation(api,scene,color.data(),bytes);
    assert(api->destroy(scene)==0&&api->destroy(scene)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(solar_os_jet_engine_idle());
    scene=nullptr;assert(api->create(64,48,color.data(),depth.data(),bytes,&scene)==0);
    controls_validation(api,scene,color.data(),bytes);assert(api->destroy(scene)==0&&solar_os_jet_engine_idle());
    scene=nullptr;assert(api->create(64,48,color.data(),depth.data(),bytes,&scene)==0);
    background_cache_validation(api,scene,color.data(),bytes);
    assert(api->destroy(scene)==0&&solar_os_jet_engine_idle());
    puts("Jet provider ABI, allocation failures, geometry and cancellation passed");
}
