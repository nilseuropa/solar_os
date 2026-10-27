#if SOLAR_OS_PACKAGE_SERVICE_JET
typedef struct {
    uint32_t generation,shader_ids[SOLAR_OS_JET_MAX_SHADERS];
    int shader_error;
    lua_State *state;
} solua_jet_t;
static bool solua_jet_in_callback;
static void solua_jet_callback_guard(lua_State *L) {
    if(solua_jet_in_callback)luaL_error(L,"Jet calls are unavailable inside a shader");
}
static bool solua_jet_active(void) {
    return solua_runtime_state && __atomic_load_n(&solua_runtime_state->jet_busy,__ATOMIC_ACQUIRE);
}
static bool solua_jet_drawing(void) {
    return solua_runtime_state && __atomic_load_n(&solua_runtime_state->jet_count,__ATOMIC_ACQUIRE)!=0;
}
static void solua_jet_lock(void) {
    while(__atomic_exchange_n(&solua_runtime_state->jet_busy,true,__ATOMIC_ACQ_REL)) vTaskDelay(1);
}
static void solua_jet_unlock(void) { __atomic_store_n(&solua_runtime_state->jet_busy,false,__ATOMIC_RELEASE); }
static bool solua_jet_live(const solua_jet_t *self) {
    return solua_runtime_state && solua_runtime_state->jet && self->generation==solua_runtime_state->jet_generation;
}
static solar_os_jet_t *solua_jet_scene(lua_State *L) {
    solua_jet_callback_guard(L);
    solua_jet_t *self=luaL_checkudata(L,1,"solaros.jet.scene");
    if(!solua_jet_live(self)) luaL_error(L,"Jet scene is closed");
    return solua_runtime_state->jet;
}
static esp_err_t solua_jet_mode(bool active) {
    __atomic_store_n(&solua_runtime_state->jet_ack,false,__ATOMIC_RELEASE);
    solua_event_t event={.type=SOLUA_EVENT_JET_MODE,.attr=active};
    if(!solua_send_event(&event)) return ESP_ERR_INVALID_STATE;
    while(!__atomic_load_n(&solua_runtime_state->jet_ack,__ATOMIC_ACQUIRE)) {
        if(solua_should_cancel(NULL)) return ESP_ERR_INVALID_STATE;
        vTaskDelay(1);
    }
    return ESP_OK;
}
static void solua_jet_destroy(void) {
    if(!solua_runtime_state) return;
    solua_jet_lock();
    while(solar_os_jet_close(&solua_runtime_state->jet)!=ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    __atomic_store_n(&solua_runtime_state->jet_count,0,__ATOMIC_RELEASE);
    solua_jet_unlock();
}
static void solua_jet_wait(lua_State *L) {
    while(__atomic_load_n(&solua_runtime_state->jet_suspended,__ATOMIC_ACQUIRE) && !solua_should_cancel(NULL))
        vTaskDelay(pdMS_TO_TICKS(10));
    if(solua_should_cancel(NULL)) luaL_error(L,"Jet interrupted");
}
#define SOLUA_JET_CALL(expression) do { \
    for(;;) { \
        solua_jet_wait(L); solua_jet_lock(); \
        if(!__atomic_load_n(&solua_runtime_state->jet_suspended,__ATOMIC_ACQUIRE)) break; \
        solua_jet_unlock(); \
    } \
    esp_err_t jet_error=(expression); solua_jet_unlock(); \
    if(solua_should_cancel(NULL)) return luaL_error(L,"Jet interrupted"); \
    solua_check_esp(L,jet_error); \
} while(0)
static int32_t solua_jet_integer(lua_State *L,int index,int32_t low,int32_t high,int32_t fallback) {
    lua_Integer n=luaL_optinteger(L,index,fallback);
    if(n<low || n>high) luaL_error(L,"Jet argument out of range");
    return (int32_t)n;
}
static uint32_t solua_jet_color(lua_State *L,int index,uint32_t fallback) {
    return solua_jet_integer(L,index,0,0xffffff,fallback);
}
static solar_os_jet_pose_t solua_jet_pose(lua_State *L,int first) {
    return (solar_os_jet_pose_t){solua_jet_integer(L,first,-4096,4096,0),
        solua_jet_integer(L,first+1,-4096,4096,0),solua_jet_integer(L,first+2,-4096,4096,0),
        solua_jet_integer(L,first+3,INT32_MIN,INT32_MAX,0),solua_jet_integer(L,first+4,INT32_MIN,INT32_MAX,0),
        solua_jet_integer(L,first+5,INT32_MIN,INT32_MAX,0)};
}
static uint32_t solua_jet_id(lua_State *L,int index) {
    lua_Integer value=luaL_checkinteger(L,index);
    if(value<1 || (uint64_t)value>UINT32_MAX) luaL_error(L,"invalid Jet object");
    return (uint32_t)value;
}
static int solua_jet_scene_close(lua_State *L) {
    solua_jet_t *self=luaL_checkudata(L,1,"solaros.jet.scene");
    if(solua_jet_live(self)) {
        solua_jet_callback_guard(L);
        bool foreground=solar_os_jet_uses_gfx(solua_runtime_state->jet,solua.session_gfx);
        solua_jet_lock(); esp_err_t err=solar_os_jet_close(&solua_runtime_state->jet);
        if(err==ESP_OK) __atomic_store_n(&solua_runtime_state->jet_count,0,__ATOMIC_RELEASE);
        solua_jet_unlock(); solua_check_esp(L,err);
        if(foreground && !solua.stop_requested && solua.events) (void)solua_jet_mode(false);
    }
    lua_getiuservalue(L,1,1);
    for(unsigned i=0;i<SOLAR_OS_JET_MAX_SHADERS;++i) {
        lua_pushboolean(L,0);lua_rawseti(L,-2,i+1);self->shader_ids[i]=0;
    }
    lua_pop(L,1);
    self->shader_error=0;return 0;
}
static int solua_jet_scene_background(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L); uint32_t color=solua_jet_color(L,2,0xffffff);
    SOLUA_JET_CALL(solar_os_jet_background(scene,color)); return 0;
}
static int solua_jet_scene_camera(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_pose_t pose=solua_jet_pose(L,2);
    if(lua_isnoneornil(L,4)) pose.z=-500;
    uint16_t fov=solua_jet_integer(L,8,10,150,60);
    SOLUA_JET_CALL(solar_os_jet_camera(scene,&pose,fov)); return 0;
}
static int solua_jet_scene_light(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    uint32_t ambient=solua_jet_color(L,2,0x5a5a5a),color=solua_jet_color(L,3,0xfff5e6);
    uint16_t intensity=solua_jet_integer(L,4,0,255,220);
    int32_t x=solua_jet_integer(L,5,INT32_MIN,INT32_MAX,35),y=solua_jet_integer(L,6,INT32_MIN,INT32_MAX,40);
    SOLUA_JET_CALL(solar_os_jet_light(scene,ambient,color,intensity,x,y)); return 0;
}
static int solua_jet_scene_cube(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    for(int i=2;i<=4;++i) luaL_checkinteger(L,i);
    uint16_t w=solua_jet_integer(L,2,1,4096,1),h=solua_jet_integer(L,3,1,4096,1),d=solua_jet_integer(L,4,1,4096,1);
    uint32_t color=solua_jet_color(L,5,0xffffff),id=0;
    SOLUA_JET_CALL(solar_os_jet_cube(scene,w,h,d,color,&id)); lua_pushinteger(L,id); return 1;
}
static int solua_jet_scene_mesh(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    luaL_checktype(L,2,LUA_TTABLE); luaL_checktype(L,3,LUA_TTABLE);
    size_t n=lua_rawlen(L,2),t=lua_rawlen(L,3);
    if(!n || n>SOLAR_OS_JET_MAX_VERTICES || !t || t>SOLAR_OS_JET_MAX_TRIANGLES)
        return luaL_error(L,"invalid Jet mesh size");
    uint32_t color=solua_jet_color(L,4,0xffffff),id=0;
    solar_os_jet_vertex_t *v=lua_newuserdatauv(L,n*sizeof(*v),0);
    solar_os_jet_triangle_t *faces=lua_newuserdatauv(L,t*sizeof(*faces),0);
    for(size_t i=0;i<n;++i) {
        lua_rawgeti(L,2,i+1); luaL_checktype(L,-1,LUA_TTABLE);
        if(lua_rawlen(L,-1)!=3) return luaL_error(L,"vertex must have three coordinates");
        int32_t xyz[3];
        for(int k=0;k<3;++k) {
            lua_rawgeti(L,-1,k+1); luaL_checkinteger(L,-1);
            xyz[k]=solua_jet_integer(L,-1,-4096,4096,0); lua_pop(L,1);
        }
        v[i]=(solar_os_jet_vertex_t){xyz[0],xyz[1],xyz[2]}; lua_pop(L,1);
    }
    for(size_t i=0;i<t;++i) {
        lua_rawgeti(L,3,i+1); luaL_checktype(L,-1,LUA_TTABLE);
        if(lua_rawlen(L,-1)!=3) return luaL_error(L,"triangle must have three indices");
        uint16_t indices[3];
        for(int k=0;k<3;++k) {
            lua_rawgeti(L,-1,k+1); luaL_checkinteger(L,-1);
            indices[k]=solua_jet_integer(L,-1,1,n,1)-1; lua_pop(L,1);
        }
        faces[i]=(solar_os_jet_triangle_t){indices[0],indices[1],indices[2]}; lua_pop(L,1);
    }
    SOLUA_JET_CALL(solar_os_jet_mesh(scene,v,n,faces,t,color,&id)); lua_pushinteger(L,id); return 1;
}
static int solua_jet_scene_transform(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L); uint32_t id=solua_jet_id(L,2);
    solar_os_jet_pose_t pose=solua_jet_pose(L,3);
    SOLUA_JET_CALL(solar_os_jet_transform(scene,id,&pose)); return 0;
}
static int solua_jet_scene_remove(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L); uint32_t id=solua_jet_id(L,2);
    SOLUA_JET_CALL(solar_os_jet_remove(scene,id));solua_jet_t *self=lua_touserdata(L,1);
    for(unsigned i=0;i<SOLAR_OS_JET_MAX_SHADERS;++i)if(self->shader_ids[i]==id) {
        lua_getiuservalue(L,1,1);lua_pushboolean(L,0);lua_rawseti(L,-2,i+1);lua_pop(L,1);self->shader_ids[i]=0;
    }
    return 0;
}
static int solua_jet_scene_render(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solua_jet_t *self=lua_touserdata(L,1);self->shader_error=0;self->state=L;
    if(!lua_checkstack(L,64))return luaL_error(L,"Jet shader stack unavailable");
    esp_err_t err;
    SOLUA_JET_CALL((err=solar_os_jet_render(scene,solua_should_cancel,NULL),self->shader_error ? ESP_OK : err));
    if(self->shader_error) {lua_pushvalue(L,self->shader_error);self->shader_error=0;return lua_error(L);}
    return 0;
}
static int solua_jet_scene_status(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L); solar_os_jet_status_t status={0};
    SOLUA_JET_CALL((solar_os_jet_status(scene,&status),ESP_OK));
    lua_newtable(L);
#define FIELD(name) solua_set_int(L,-1,#name,status.name)
    FIELD(frames);FIELD(presented);FIELD(dropped);FIELD(width);FIELD(height);FIELD(error);
    FIELD(render_us);FIELD(present_us);
#undef FIELD
    return 1;
}
#include "solar_os_lua_jet_surface.inc"
#include "solar_os_lua_jet_scene.inc"
#include "solar_os_lua_jet_overlay.inc"
#include "solar_os_lua_jet_assets.inc"
#include "solar_os_lua_jet_instances.inc"
#include "solar_os_lua_jet_extras.inc"
#include "solar_os_lua_jet_postfx.inc"
#include "solar_os_lua_jet_shaders.inc"
static int solua_jet_open(lua_State *L) {
    solua_jet_callback_guard(L);
    if(lua_gettop(L)>2) return luaL_error(L,"too many Jet arguments");
    solar_os_jet_options_t options={0};
    if(!lua_isnoneornil(L,1)) {
        size_t n; options.display=luaL_checklstring(L,1,&n);
        if(!n || n>=SOLAR_OS_DISPLAY_TARGET_NAME_MAX || memchr(options.display,0,n))
            return luaL_error(L,"invalid display name");
    }
    options.fps=solua_jet_integer(L,2,0,30,0);
    if(solua.claimed_gfx || solua.tui_active || solua_emotion_drawing() || solua_jet_drawing())
        return luaL_error(L,"close graphics, TUI, emotion and Jet scenes before opening Jet");
    solua_jet_t *self=lua_newuserdatauv(L,sizeof(*self),1);memset(self,0,sizeof(*self));self->state=L;
    lua_createtable(L,SOLAR_OS_JET_MAX_SHADERS,0);
    for(unsigned i=0;i<SOLAR_OS_JET_MAX_SHADERS;++i) {lua_pushboolean(L,0);lua_rawseti(L,-2,i+1);}
    lua_setiuservalue(L,-2,1);
    if(luaL_newmetatable(L,"solaros.jet.scene")) {
#define METHOD(name) solua_set_func(L,-1,#name,solua_jet_scene_##name)
        METHOD(background);METHOD(camera);METHOD(light);METHOD(cube);METHOD(mesh);METHOD(transform);
        METHOD(remove);METHOD(render);METHOD(status);METHOD(close);
        METHOD(capabilities);METHOD(material);METHOD(material_set);METHOD(material_info);
        METHOD(texture);METHOD(texture_set);METHOD(texture_info);METHOD(assign);METHOD(mesh_data);
        METHOD(sample_texture);
        METHOD(primitive);METHOD(object_set);METHOD(object_info);METHOD(object_action);METHOD(mesh_update);
        METHOD(camera_set);METHOD(camera_info);METHOD(camera_action);METHOD(point_light);METHOD(point_light_set);
        METHOD(point_light_info);METHOD(settings);METHOD(settings_info);METHOD(gradient);METHOD(lod);
        METHOD(sprite);METHOD(sprite_set);METHOD(sprite_info);METHOD(pick);METHOD(picks);
        METHOD(load_obj);METHOD(load_mtl);METHOD(load_bmp);
        METHOD(prototype);METHOD(prototype_info);METHOD(instance);METHOD(instance_add);METHOD(instance_set);
        METHOD(instance_info);METHOD(instance_remove);METHOD(instance_clear);
        METHOD(particles);METHOD(particles_set);METHOD(particles_emit);METHOD(particles_info);METHOD(particles_clear);
        METHOD(flare);METHOD(flare_set);METHOD(flare_info);METHOD(flare_element_set);METHOD(flare_element_info);METHOD(step);
        METHOD(postfx);METHOD(postfx_info);METHOD(postfx_reset);METHOD(postfx_clear);
        METHOD(shader);METHOD(shader_set);METHOD(shader_info);
#undef METHOD
        solua_set_func(L,-1,"__gc",solua_jet_scene_close); solua_set_func(L,-1,"__close",solua_jet_scene_close);
        lua_pushvalue(L,-1); lua_setfield(L,-2,"__index");
    }
    lua_setmetatable(L,-2);
    char current[SOLAR_OS_DISPLAY_TARGET_NAME_MAX]={0},detail[96]={0};
    if(solua.session_gfx) solar_os_gfx_display_target_name(solua.session_gfx,current,sizeof(current));
    bool foreground=*current && (!options.display || !strcmp(current,options.display));
    solua_jet_lock();
    bool mode_started=foreground && !__atomic_load_n(&solua_runtime_state->jet_suspended,__ATOMIC_ACQUIRE);
    esp_err_t err=foreground && !mode_started ? ESP_ERR_INVALID_STATE : (foreground ? solua_jet_mode(true) : ESP_OK);
    if(err==ESP_OK) err=solar_os_jet_open(&options,solua.session_gfx,&solua_runtime_state->jet,detail,sizeof(detail));
    if(solua_runtime_state->jet) __atomic_store_n(&solua_runtime_state->jet_count,1,__ATOMIC_RELEASE);
    if(err!=ESP_OK && mode_started && !solua_runtime_state->jet) (void)solua_jet_mode(false);
    if(!++solua_runtime_state->jet_generation) ++solua_runtime_state->jet_generation;
    self->generation=solua_runtime_state->jet_generation;
    solua_jet_unlock();
    if(err!=ESP_OK) return luaL_error(L,"jet: %s (%s)",detail,esp_err_to_name(err));
    return 1;
}
#undef SOLUA_JET_CALL
#else
static bool solua_jet_active(void) { return false; }
static bool solua_jet_drawing(void) { return false; }
static void solua_jet_destroy(void) {}
#endif
