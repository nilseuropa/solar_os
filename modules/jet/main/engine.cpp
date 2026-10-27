#include "solar_os_jet_abi.h"
#include "Scene.hpp"
#include "Primitives.hpp"
#include "ParticleSystem.hpp"
#include "LensFlare.hpp"
#include <algorithm>
#include <memory>
#include <new>
#include <cstring>
#include <cmath>
#include <string_view>

using namespace Renderer;
namespace {
struct Resource {
    uint32_t id=0;
    char name[SOLAR_OS_JET_NAME]={0};
    double values[SOLAR_OS_JET_VALUES]={0};
};
struct TileData {
    std::vector<uint8_t> pages,coarse;
    std::vector<uint16_t> palette,filtered;
    TileTexture view;
};
struct TextureResource:Resource {
    unsigned width=0,height=0;
    std::vector<uint16_t> pixels,palette;
    std::unique_ptr<Texture> value;
    std::unique_ptr<TileData> tiles;
};
struct MaterialResource:Resource {
    Material value;
    MaterialResource() {value.name=name;}
};
struct ShaderResource:Resource,Shader {
    solar_os_jet_shader_callback_t callback={};
    uint32_t calls=0;
    bool failed=false;
    uint16_t getPixel(uint32_t u,uint32_t v,ShaderParameters *params) override;
};
struct LightResource:Resource {
    PointLight value{{0,0,0},{255,255,255}};
};
struct SpriteResource:Resource {
    Material fallback;
    Sprite2D value;
    SpriteResource() {fallback.name=nullptr;}
};
struct MeshResource:Resource {
    Material fallback;
    std::vector<std::unique_ptr<Material>> face_materials;
    Object::SharedMesh value;
};
struct InstancePose {
    uint32_t prototype=0;
    double values[JET_INST_COUNT]={0};
};
struct ParticleResource:Resource {ParticleSystem value;};
struct FlareResource:Resource {
    solar_os_jet_element_t elements[SOLAR_OS_JET_MAX_FLARE_ELEMENTS]={};
    unsigned count=0;
    std::unique_ptr<LensFlare> value;
};
struct PickSnapshot {
    uint32_t object=0,mesh=0,batch=0;
    int32_t instance=-1;
    int32_t triangle=-1,depth=0;
    int16_t x=-1,y=-1;
    bool hit=false;
};
struct Item {
    Material material;
    std::unique_ptr<Object> object;
    uint32_t id;
    char name[SOLAR_OS_JET_NAME]={0};
    std::vector<std::unique_ptr<Material>> face_materials;
    std::vector<InstancePose> instance_poses;
    explicit Item(uint32_t color) : material(uint16_t(((color >> 8) & 0xf800) |
        ((color >> 5) & 0x7e0) | ((color >> 3) & 31))), id(0) {
        material.shadingMode = ShadingMode::GOURAUD;
        material.name=nullptr;
    }
};
struct Engine {
    Camera camera;
    AmbientLight ambient{Color{90,90,90}};
    DirectionalLight light{Vector3{35,40,0}, Color{255,245,230}, 220};
    std::unique_ptr<Item> items[SOLAR_OS_JET_MAX_OBJECTS];
    std::unique_ptr<TextureResource> textures[SOLAR_OS_JET_MAX_TEXTURES];
    std::unique_ptr<MaterialResource> materials[SOLAR_OS_JET_MAX_MATERIALS];
    std::unique_ptr<ShaderResource> shaders[SOLAR_OS_JET_MAX_SHADERS];
    std::unique_ptr<LightResource> lights[SOLAR_OS_JET_MAX_LIGHTS];
    std::unique_ptr<SpriteResource> sprites[SOLAR_OS_JET_MAX_SPRITES];
    std::unique_ptr<MeshResource> prototypes[SOLAR_OS_JET_MAX_PROTOTYPES];
    std::unique_ptr<ParticleResource> particles[SOLAR_OS_JET_MAX_PARTICLES];
    std::unique_ptr<FlareResource> flares[SOLAR_OS_JET_MAX_FLARES];
    std::unique_ptr<PostFX> effects;
    double effect_values[SOLAR_OS_JET_VALUES]={0};
    uint16_t *frame_color=nullptr;
    float frame_dt=1.0f/30;
    bool shader_fault=false;
    bool (*frame_cancel)(void *)=nullptr;
    void *frame_cancel_user=nullptr;
    PickSnapshot picks[SOLAR_OS_JET_MAX_PICKS];
    solar_os_jet_pick_t query_points[SOLAR_OS_JET_MAX_PICKS];
    unsigned pick_count=0,query_count=0;
    std::vector<uint16_t> gradient;
    std::vector<uint16_t> background_color,background_depth,pending_color,pending_depth;
    uint16_t *frame_depth=nullptr;
    bool background_pending=false,background_captured=false;
    double settings[SOLAR_OS_JET_VALUES]={1};
    uint16_t fov=60;
    uint32_t next_id = 0;
    uint16_t width, height;
    Scene scene;
    Engine(uint16_t w, uint16_t h, uint16_t *color, uint16_t *depth)
        : width(w), height(h), scene(color, depth, w, h) {
        frame_depth=depth;
        effect_values[JET_FX_pixelate]=1;
        scene.setCamera(&camera); scene.setAmbientLight(&ambient);
        scene.setDirectionalLight(&light); scene.setBackcolor(0xffff);
        scene.setClearBuffer(true); camera.setPosition(0,0,-500);
        camera.setFOV(int32_t(60),int32_t(w)); camera.nearPlane=16; camera.farPlane=4096;
    }
};
static Engine *active;
static uint16_t rgb565(uint32_t c) { return ((c >> 8) & 0xf800) | ((c >> 5) & 0x7e0) | ((c >> 3) & 31); }
static Color rgb(uint32_t c) { return {uint8_t(c >> 16),uint8_t(c >> 8),uint8_t(c)}; }
static bool coordinate(int32_t c) { return c >= -4096 && c <= 4096; }
static bool pose_valid(const solar_os_jet_pose_t *p) {
    return p && coordinate(p->x) && coordinate(p->y) && coordinate(p->z);
}
#ifdef JET_EXTERNAL_LIBRARY
extern "C" void solar_os_jet_initialize(void);
extern "C" void solar_os_espdl_port_leave_task(void);
#endif
template<class F> static int call(F f) {
    int result;
    try {
#ifdef JET_EXTERNAL_LIBRARY
        solar_os_jet_initialize();
#endif
        result=f();
    }
    catch (const std::bad_alloc &) { result=SOLAR_OS_NATIVE_ERROR_NO_MEMORY; }
    catch (...) { result=SOLAR_OS_NATIVE_ERROR_FAILED; }
#ifdef JET_EXTERNAL_LIBRARY
    solar_os_espdl_port_leave_task();
#endif
    return result;
}
static int create(uint16_t w,uint16_t h,uint16_t *color,uint16_t *depth,size_t bytes,void **out) {
    if (!out || *out || !color || !depth || !w || !h || w%8 || w>1024 || h>1024 ||
        bytes != size_t(w)*h*2 || uintptr_t(color)%2 || uintptr_t(depth)%2)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (active) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    return call([&] { auto scene=std::make_unique<Engine>(w,h,color,depth);
        active=scene.release(); *out=active; return 0; });
}
static int destroy(void *s) {
    if (!s || s!=active) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    return call([&] { delete active; active=nullptr; return 0; });
}
static int background(void *s,uint32_t color) {
    if (s!=active || !s) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (color>0xffffff) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return call([&] { active->scene.setBackcolor(rgb565(color)); return 0; });
}
static int camera(void *s,const solar_os_jet_pose_t *p,uint16_t fov) {
    if (s!=active || !s) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!pose_valid(p) || fov<10 || fov>150) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return call([&] { active->camera.setPosition(p->x,p->y,p->z);
        active->camera.setRotation(p->rx%360,p->ry%360,p->rz%360);
        active->camera.setFOV(int32_t(fov),int32_t(active->width)); active->fov=fov; return 0; });
}
static int light(void *s,uint32_t ambient,uint32_t color,uint16_t intensity,int32_t azimuth,int32_t elevation) {
    if (s!=active || !s) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (ambient>0xffffff || color>0xffffff || intensity>255) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return call([&] { active->ambient.color=rgb(ambient); active->light.color=rgb(color);
        active->light.intensity=intensity; active->light.direction={azimuth%360,elevation%360,0}; return 0; });
}
static int admit(std::unique_ptr<Item> item,uint32_t *out) {
    for (auto &slot:active->items) if (!slot) {
        if (active->next_id==UINT32_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        active->scene.addObject(item->object.get());
        item->id=++active->next_id; *out=item->id; slot=std::move(item); return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
}
static int cube(void *s,uint16_t w,uint16_t h,uint16_t d,uint32_t color,uint32_t *out) {
    if (s!=active || !s) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!out || !w || !h || !d || w>4096 || h>4096 || d>4096 || color>0xffffff)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    return call([&] { auto item=std::make_unique<Item>(color);
        item->object.reset(Primitives::createCube(w,h,d,&item->material));
        return admit(std::move(item),out); });
}
static int mesh(void *s,const solar_os_jet_vertex_t *v,size_t n,
    const solar_os_jet_triangle_t *t,size_t count,uint32_t color,uint32_t *out) {
    if (s!=active || !s) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if (!v || !t || !out || !n || n>SOLAR_OS_JET_MAX_VERTICES || !count ||
        count>SOLAR_OS_JET_MAX_TRIANGLES || color>0xffffff) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    for(size_t i=0;i<n;++i) if(!coordinate(v[i].x)||!coordinate(v[i].y)||!coordinate(v[i].z))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    for(size_t i=0;i<count;++i) if(t[i].a>=n||t[i].b>=n||t[i].c>=n)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return call([&] { auto item=std::make_unique<Item>(color); item->object=std::make_unique<Object>();
        auto &object=*item->object; object.vertices.reserve(count*3); object.triangles.reserve(count);
        for(size_t i=0;i<count;++i) {
            for(auto index:{t[i].a,t[i].b,t[i].c}) object.addVertex({{v[index].x,v[index].y,v[index].z}});
            object.addTriangle(i*3,i*3+1,i*3+2,&item->material);
        }
        object.computeFlatNormals(); object.calculateBoundingBox();
        return admit(std::move(item),out); });
}
static Item *find(uint32_t id) {
    for(auto &item:active->items) if(item && item->id==id) return item.get();
    return nullptr;
}
static int scene_command(const solar_os_jet_command_t &,solar_os_jet_reply_t &);
static int overlay_command(const solar_os_jet_command_t &,solar_os_jet_reply_t &);
static int asset_command(const solar_os_jet_command_t &,solar_os_jet_reply_t &);
static int instance_command(const solar_os_jet_command_t &,solar_os_jet_reply_t &);
static void refresh_instance_poses(Item &);
static int remove_prototype(uint32_t);
static int extras_command(const solar_os_jet_command_t &,solar_os_jet_reply_t &);
static int postfx_command(const solar_os_jet_command_t &,solar_os_jet_reply_t &);
static int shader_command(const solar_os_jet_command_t &,solar_os_jet_reply_t &);
static void process_postfx();
#include "tile_texture.inc"
#include "surface_api.inc"
#include "shader_api.inc"
#include "scene_api.inc"
#include "overlay_api.inc"
#include "asset_api.inc"
#include "instance_api.inc"
#include "extras_api.inc"
#include "postfx_api.inc"
static int transform(void *s,uint32_t id,const solar_os_jet_pose_t *p) {
    if(s!=active || !s) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if(!pose_valid(p)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return call([&] { Item *item=find(id); if(!item) return int(SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
        item->object->setPosition(p->x,p->y,p->z);
        item->object->setRotation(p->rx%360,p->ry%360,p->rz%360); return 0; });
}
static int remove(void *s,uint32_t id) {
    if(s!=active || !s) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    return call([&] { for(auto &item:active->items) if(item && item->id==id) {
        for(auto &other:active->items) if(other) for(auto *lod:other->object->lodMeshes)
            if(lod==item->object.get()) return int(SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
        auto &objects=active->scene.getObjects();
        objects.erase(std::remove(objects.begin(),objects.end(),item->object.get()),objects.end());
        item.reset(); return 0;
    } int err=remove_extras(id);if(err!=SOLAR_OS_NATIVE_ERROR_NOT_FOUND)return err;
      err=remove_prototype(id);if(err!=SOLAR_OS_NATIVE_ERROR_NOT_FOUND)return err;
      err=remove_sprite(id);if(err!=SOLAR_OS_NATIVE_ERROR_NOT_FOUND)return err;
      err=remove_light(id);return err==SOLAR_OS_NATIVE_ERROR_NOT_FOUND ? remove_surface(id) : err; });
}
struct Cancellation {
    solar_os_native_cancel_fn callback;
    void *user;
    bool cancelled;
};
static bool cancelled(void *value) {
    auto &state=*static_cast<Cancellation *>(value);
    state.cancelled=state.cancelled || (state.callback && state.callback(state.user));
    return state.cancelled;
}
static bool frame_stopped(void *value) {return active->shader_fault||cancelled(value);}
static int render(void *s,uint16_t *color,size_t bytes,solar_os_native_cancel_fn cancel,void *user) {
    if(s!=active || !s) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if(!color || uintptr_t(color)%2 || bytes!=size_t(active->width)*active->height*2)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return call([&] {
        Cancellation state{cancel,user,false};
        active->shader_fault=false;
        for(auto &shader:active->shaders)if(shader) {shader->failed=false;shader->calls=0;}
        if(cancelled(&state)) return int(SOLAR_OS_NATIVE_ERROR_TIMEOUT);
        active->background_captured=false;
        extras_hook_state();
        prepare_extras();
        active->frame_cancel=frame_stopped;active->frame_cancel_user=&state;
        active->frame_color=color;
        active->scene.setFramebuffer(color);
        active->scene.setCancellation(frame_stopped,&state);
        try { active->scene.render(); }
        catch (...) { active->scene.setCancellation(nullptr,nullptr);active->frame_cancel=nullptr;active->frame_cancel_user=nullptr;active->frame_color=nullptr;throw; }
        active->scene.setCancellation(nullptr,nullptr);
        active->frame_cancel=nullptr;active->frame_cancel_user=nullptr;
        active->frame_color=nullptr;
        if(cancelled(&state)) return int(SOLAR_OS_NATIVE_ERROR_TIMEOUT);
        for(auto &shader:active->shaders)if(shader&&shader->failed)return int(SOLAR_OS_NATIVE_ERROR_FAILED);
        if(active->background_captured) {
            active->background_color.swap(active->pending_color);
            active->background_depth.swap(active->pending_depth);
            std::vector<uint16_t>().swap(active->pending_color);
            std::vector<uint16_t>().swap(active->pending_depth);
            active->background_pending=false;
            active->scene.setBackgroundCache(active->background_color.data(),active->background_depth.data());
        }
        snapshot_picks();return 0;
    });
}
static unsigned call_busy;
template<auto Function,class... Args> static int serialized(Args... args) {
    if(__atomic_exchange_n(&call_busy,1U,__ATOMIC_ACQUIRE)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    int result=Function(args...);
    __atomic_store_n(&call_busy,0U,__ATOMIC_RELEASE);
    return result;
}
static const solar_os_jet_extensions_v1_t extensions={1,sizeof(extensions),serialized<execute>};
static const solar_os_jet_api_v1_t api={1,sizeof(api),serialized<create>,serialized<destroy>,
    serialized<background>,serialized<camera>,serialized<light>,serialized<cube>,serialized<mesh>,
    serialized<transform>,serialized<remove>,serialized<render>,&extensions};
}
extern "C" const solar_os_jet_api_v1_t *solar_os_jet_engine_api_v1(void) { return &api; }
extern "C" bool solar_os_jet_engine_idle(void) { return active==nullptr && !__atomic_load_n(&call_busy,__ATOMIC_ACQUIRE); }
