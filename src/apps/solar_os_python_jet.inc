#if SOLAR_OS_PACKAGE_SERVICE_JET
typedef struct {
    mp_obj_base_t base; uint32_t generation;
    mp_obj_t shader_functions[SOLAR_OS_JET_MAX_SHADERS],shader_error;
    uint32_t shader_ids[SOLAR_OS_JET_MAX_SHADERS];
} python_jet_t;
static bool python_jet_in_callback;
static void python_jet_callback_guard(void) {
    if(python_jet_in_callback)mp_raise_ValueError(MP_ERROR_TEXT("Jet calls are unavailable inside a shader"));
}
static const mp_obj_type_t solaros_jet_scene_type;
static bool python_jet_active(void) {
    return python_runtime_state && __atomic_load_n(&python_runtime_state->jet_busy,__ATOMIC_ACQUIRE);
}
static bool python_jet_drawing(void) {
    return python_runtime_state && __atomic_load_n(&python_runtime_state->jet_count,__ATOMIC_ACQUIRE)!=0;
}
static void python_jet_lock(void) {
    while(__atomic_exchange_n(&python_runtime_state->jet_busy,true,__ATOMIC_ACQ_REL)) vTaskDelay(1);
}
static void python_jet_unlock(void) {
    __atomic_store_n(&python_runtime_state->jet_busy,false,__ATOMIC_RELEASE);
}
static python_jet_t *python_jet_object(mp_obj_t object) {
    if(!mp_obj_is_type(object,&solaros_jet_scene_type)) mp_raise_TypeError(MP_ERROR_TEXT("expected a Jet scene"));
    return MP_OBJ_TO_PTR(object);
}
static bool python_jet_live(const python_jet_t *self) {
    return python_runtime_state && python_runtime_state->jet && self->generation==python_runtime_state->jet_generation;
}
static solar_os_jet_t *python_jet_scene(mp_obj_t object) {
    python_jet_callback_guard();
    python_jet_t *self=python_jet_object(object);
    if(!python_jet_live(self)) mp_raise_ValueError(MP_ERROR_TEXT("Jet scene is closed"));
    return python_runtime_state->jet;
}
static esp_err_t python_jet_mode(bool active) {
    __atomic_store_n(&python_runtime_state->jet_ack,false,__ATOMIC_RELEASE);
    python_event_t event={.type=PYTHON_EVENT_JET_MODE,.attr=active};
    if(!python_send_event(&event)) return ESP_ERR_INVALID_STATE;
    while(!__atomic_load_n(&python_runtime_state->jet_ack,__ATOMIC_ACQUIRE)) {
        if(python_should_cancel(NULL)) return ESP_ERR_INVALID_STATE;
        vTaskDelay(1);
    }
    return ESP_OK;
}
static void python_jet_destroy(void) {
    if(!python_runtime_state) return;
    python_jet_lock();
    while(solar_os_jet_close(&python_runtime_state->jet)!=ESP_OK) vTaskDelay(pdMS_TO_TICKS(20));
    __atomic_store_n(&python_runtime_state->jet_count,0,__ATOMIC_RELEASE);
    python_jet_unlock();
}
static void python_jet_wait(void) {
    while(__atomic_load_n(&python_runtime_state->jet_suspended,__ATOMIC_ACQUIRE) && !python_should_cancel(NULL))
        vTaskDelay(pdMS_TO_TICKS(10));
    if(python_should_cancel(NULL)) mp_raise_type(&mp_type_KeyboardInterrupt);
}
#define PYTHON_JET_CALL(expression) do { \
    for(;;) { \
        python_jet_wait(); python_jet_lock(); \
        if(!__atomic_load_n(&python_runtime_state->jet_suspended,__ATOMIC_ACQUIRE)) break; \
        python_jet_unlock(); \
    } \
    esp_err_t jet_error=(expression); python_jet_unlock(); \
    if(python_should_cancel(NULL)) mp_raise_type(&mp_type_KeyboardInterrupt); \
    python_check_esp(jet_error); \
} while(0)
static void python_jet_args(size_t count,const mp_obj_t *args,mp_map_t *kwargs,
    const char *const *names,unsigned size,mp_obj_t *values) {
    if(count>size) mp_raise_TypeError(MP_ERROR_TEXT("too many Jet arguments"));
    if(kwargs) for(size_t i=0;i<kwargs->alloc;++i) {
        if(!mp_map_slot_is_filled(kwargs,i)) continue;
        size_t length; const char *key=mp_obj_str_get_data(kwargs->table[i].key,&length);
        unsigned n=0; for(;n<size && (strlen(names[n])!=length || memcmp(key,names[n],length));++n) {}
        if(n==size) mp_raise_TypeError(MP_ERROR_TEXT("unknown Jet argument"));
    }
    for(unsigned i=0;i<size;++i) {
        values[i]=python_kw_value(kwargs,names[i]);
        if(i<count) {
            if(values[i]!=MP_OBJ_NULL) mp_raise_TypeError(MP_ERROR_TEXT("duplicate Jet argument"));
            values[i]=args[i];
        }
    }
}
static int32_t python_jet_integer(mp_obj_t value,int32_t low,int32_t high,int32_t fallback) {
    mp_int_t n=value==MP_OBJ_NULL ? fallback : mp_obj_get_int(value);
    if(n<low || n>high) mp_raise_ValueError(MP_ERROR_TEXT("Jet argument out of range"));
    return n;
}
static uint32_t python_jet_color(mp_obj_t value) {
    return python_jet_integer(value,0,0xffffff,0xffffff);
}
static solar_os_jet_pose_t python_jet_pose(mp_obj_t *v) {
    return (solar_os_jet_pose_t){
        python_jet_integer(v[0],-4096,4096,0),python_jet_integer(v[1],-4096,4096,0),
        python_jet_integer(v[2],-4096,4096,0),python_jet_integer(v[3],INT32_MIN,INT32_MAX,0),
        python_jet_integer(v[4],INT32_MIN,INT32_MAX,0),python_jet_integer(v[5],INT32_MIN,INT32_MAX,0)};
}
static mp_obj_t solaros_jet_scene_close(mp_obj_t object) {
    python_jet_t *self=python_jet_object(object);
    if(python_jet_live(self)) {
        python_jet_callback_guard();
        bool foreground=solar_os_jet_uses_gfx(python_runtime_state->jet,python_app.session_gfx);
        python_jet_lock();
        esp_err_t err=solar_os_jet_close(&python_runtime_state->jet);
        if(err==ESP_OK) __atomic_store_n(&python_runtime_state->jet_count,0,__ATOMIC_RELEASE);
        python_jet_unlock(); python_check_esp(err);
        if(foreground && !python_app.stop_requested && python_app.events) (void)python_jet_mode(false);
    }
    memset(self->shader_functions,0,sizeof(self->shader_functions));self->shader_error=MP_OBJ_NULL;
    memset(self->shader_ids,0,sizeof(self->shader_ids));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_close_obj,solaros_jet_scene_close);
static mp_obj_t solaros_jet_scene_background(mp_obj_t self,mp_obj_t color) {
    solar_os_jet_t *scene=python_jet_scene(self); uint32_t c=python_jet_color(color);
    PYTHON_JET_CALL(solar_os_jet_background(scene,c)); return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_background_obj,solaros_jet_scene_background);
static mp_obj_t solaros_jet_scene_camera(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    const char *names[]={"x","y","z","rx","ry","rz","fov"}; mp_obj_t v[7];
    python_jet_args(count-1,args+1,kwargs,names,7,v);
    if(v[2]==MP_OBJ_NULL) v[2]=MP_OBJ_NEW_SMALL_INT(-500);
    solar_os_jet_pose_t pose=python_jet_pose(v);
    uint16_t fov=python_jet_integer(v[6],10,150,60);
    PYTHON_JET_CALL(solar_os_jet_camera(scene,&pose,fov)); return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_camera_obj,1,solaros_jet_scene_camera);
static mp_obj_t solaros_jet_scene_light(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    const char *names[]={"ambient","color","intensity","azimuth","elevation"}; mp_obj_t v[5];
    python_jet_args(count-1,args+1,kwargs,names,5,v);
    uint32_t ambient=v[0]==MP_OBJ_NULL ? 0x5a5a5a : python_jet_color(v[0]);
    uint32_t color=v[1]==MP_OBJ_NULL ? 0xfff5e6 : python_jet_color(v[1]);
    uint16_t intensity=python_jet_integer(v[2],0,255,220);
    int32_t x=python_jet_integer(v[3],INT32_MIN,INT32_MAX,35),y=python_jet_integer(v[4],INT32_MIN,INT32_MAX,40);
    PYTHON_JET_CALL(solar_os_jet_light(scene,ambient,color,intensity,x,y)); return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_light_obj,1,solaros_jet_scene_light);
static mp_obj_t solaros_jet_scene_cube(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    const char *names[]={"width","height","depth","color"}; mp_obj_t v[4];
    python_jet_args(count-1,args+1,kwargs,names,4,v);
    for(unsigned i=0;i<3;++i) if(v[i]==MP_OBJ_NULL) mp_raise_TypeError(MP_ERROR_TEXT("cube dimensions are required"));
    uint16_t w=python_jet_integer(v[0],1,4096,1),h=python_jet_integer(v[1],1,4096,1),d=python_jet_integer(v[2],1,4096,1);
    uint32_t c=python_jet_color(v[3]),id=0;
    PYTHON_JET_CALL(solar_os_jet_cube(scene,w,h,d,c,&id)); return mp_obj_new_int_from_uint(id);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_cube_obj,1,solaros_jet_scene_cube);
static mp_obj_t solaros_jet_scene_mesh(size_t count,const mp_obj_t *args) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    size_t n,t; mp_obj_t *vertices,*triangles;
    mp_obj_get_array(args[1],&n,&vertices); mp_obj_get_array(args[2],&t,&triangles);
    if(!n || n>SOLAR_OS_JET_MAX_VERTICES || !t || t>SOLAR_OS_JET_MAX_TRIANGLES)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid Jet mesh size"));
    uint32_t c=count>3 ? python_jet_color(args[3]) : 0xffffff,id=0;
    solar_os_jet_vertex_t *v=m_new(solar_os_jet_vertex_t,n);
    solar_os_jet_triangle_t *faces=m_new(solar_os_jet_triangle_t,t);
    for(size_t i=0;i<n;++i) {
        mp_obj_t *xyz; mp_obj_get_array_fixed_n(vertices[i],3,&xyz);
        v[i]=(solar_os_jet_vertex_t){python_jet_integer(xyz[0],-4096,4096,0),
            python_jet_integer(xyz[1],-4096,4096,0),python_jet_integer(xyz[2],-4096,4096,0)};
    }
    for(size_t i=0;i<t;++i) {
        mp_obj_t *indices; mp_obj_get_array_fixed_n(triangles[i],3,&indices);
        faces[i]=(solar_os_jet_triangle_t){python_jet_integer(indices[0],0,n-1,0),
            python_jet_integer(indices[1],0,n-1,0),python_jet_integer(indices[2],0,n-1,0)};
    }
    PYTHON_JET_CALL(solar_os_jet_mesh(scene,v,n,faces,t,c,&id));
    m_del(solar_os_jet_vertex_t,v,n); m_del(solar_os_jet_triangle_t,faces,t);
    return mp_obj_new_int_from_uint(id);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_jet_scene_mesh_obj,3,4,solaros_jet_scene_mesh);
static mp_obj_t solaros_jet_scene_transform(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    solar_os_jet_t *scene=python_jet_scene(args[0]);
    const char *names[]={"object","x","y","z","rx","ry","rz"}; mp_obj_t v[7];
    python_jet_args(count-1,args+1,kwargs,names,7,v);
    if(v[0]==MP_OBJ_NULL) mp_raise_TypeError(MP_ERROR_TEXT("object is required"));
    uint32_t id=python_u32_from_obj(v[0]); solar_os_jet_pose_t pose=python_jet_pose(v+1);
    PYTHON_JET_CALL(solar_os_jet_transform(scene,id,&pose)); return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_scene_transform_obj,1,solaros_jet_scene_transform);
static mp_obj_t solaros_jet_scene_remove(mp_obj_t self,mp_obj_t object) {
    solar_os_jet_t *scene=python_jet_scene(self); uint32_t id=python_u32_from_obj(object);
    PYTHON_JET_CALL(solar_os_jet_remove(scene,id));
    python_jet_t *owner=python_jet_object(self);
    for(unsigned i=0;i<SOLAR_OS_JET_MAX_SHADERS;++i)if(owner->shader_ids[i]==id) {
        owner->shader_ids[i]=0;owner->shader_functions[i]=MP_OBJ_NULL;
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_jet_scene_remove_obj,solaros_jet_scene_remove);
static mp_obj_t solaros_jet_scene_render(mp_obj_t self) {
    solar_os_jet_t *scene=python_jet_scene(self);
    python_jet_t *owner=python_jet_object(self);owner->shader_error=MP_OBJ_NULL;
    esp_err_t err;
    PYTHON_JET_CALL((err=solar_os_jet_render(scene,python_should_cancel,NULL),owner->shader_error!=MP_OBJ_NULL ? ESP_OK : err));
    if(owner->shader_error!=MP_OBJ_NULL) {
        mp_obj_t exception=owner->shader_error;owner->shader_error=MP_OBJ_NULL;nlr_raise(exception);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_render_obj,solaros_jet_scene_render);
static mp_obj_t solaros_jet_scene_status(mp_obj_t self) {
    solar_os_jet_t *scene=python_jet_scene(self); solar_os_jet_status_t status={0};
    PYTHON_JET_CALL((solar_os_jet_status(scene,&status),ESP_OK));
    mp_obj_t dict=mp_obj_new_dict(0);
#define FIELD(name) python_dict_store_uint(dict,#name,status.name)
    FIELD(frames); FIELD(presented); FIELD(dropped); FIELD(width); FIELD(height);
#undef FIELD
    python_dict_store_int(dict,"error",status.error);
    python_dict_store_u64(dict,"render_us",status.render_us); python_dict_store_u64(dict,"present_us",status.present_us);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_status_obj,solaros_jet_scene_status);
#include "solar_os_python_jet_surface.inc"
#include "solar_os_python_jet_scene.inc"
#include "solar_os_python_jet_overlay.inc"
#include "solar_os_python_jet_assets.inc"
#include "solar_os_python_jet_instances.inc"
#include "solar_os_python_jet_extras.inc"
#include "solar_os_python_jet_postfx.inc"
#include "solar_os_python_jet_shaders.inc"
static mp_obj_t solaros_jet_scene_enter(mp_obj_t self) { (void)python_jet_scene(self); return self; }
MP_DEFINE_CONST_FUN_OBJ_1(solaros_jet_scene_enter_obj,solaros_jet_scene_enter);
static mp_obj_t solaros_jet_scene_exit(size_t count,const mp_obj_t *args) {
    (void)count; return solaros_jet_scene_close(args[0]);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_jet_scene_exit_obj,4,4,solaros_jet_scene_exit);
static const mp_rom_map_elem_t solaros_jet_scene_locals_table[]={
#define METHOD(name) {MP_ROM_QSTR(MP_QSTR_##name),MP_ROM_PTR(&solaros_jet_scene_##name##_obj)}
    METHOD(background),METHOD(camera),METHOD(light),METHOD(cube),METHOD(mesh),METHOD(transform),
    METHOD(remove),METHOD(render),METHOD(status),METHOD(close),
    METHOD(capabilities),METHOD(material),METHOD(material_set),METHOD(material_info),
    METHOD(texture),METHOD(texture_set),METHOD(texture_info),METHOD(assign),METHOD(mesh_data),
    METHOD(sample_texture),
    METHOD(primitive),METHOD(object_set),METHOD(object_info),METHOD(object_action),METHOD(mesh_update),
    METHOD(camera_set),METHOD(camera_info),METHOD(camera_action),METHOD(point_light),METHOD(point_light_set),
    METHOD(point_light_info),METHOD(settings),METHOD(settings_info),METHOD(gradient),METHOD(lod),
    METHOD(sprite),METHOD(sprite_set),METHOD(sprite_info),METHOD(pick),METHOD(picks),
    METHOD(load_obj),METHOD(load_mtl),METHOD(load_bmp),
    METHOD(prototype),METHOD(prototype_info),METHOD(instance),METHOD(instance_add),METHOD(instance_set),
    METHOD(instance_info),METHOD(instance_remove),METHOD(instance_clear),
    METHOD(particles),METHOD(particles_set),METHOD(particles_emit),METHOD(particles_info),METHOD(particles_clear),
    METHOD(flare),METHOD(flare_set),METHOD(flare_info),METHOD(flare_element_set),METHOD(flare_element_info),METHOD(step),
    METHOD(postfx),METHOD(postfx_info),METHOD(postfx_reset),METHOD(postfx_clear),
    METHOD(shader),METHOD(shader_set),METHOD(shader_info),
#undef METHOD
    {MP_ROM_QSTR(MP_QSTR___del__),MP_ROM_PTR(&solaros_jet_scene_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___enter__),MP_ROM_PTR(&solaros_jet_scene_enter_obj)},
    {MP_ROM_QSTR(MP_QSTR___exit__),MP_ROM_PTR(&solaros_jet_scene_exit_obj)},
};
static MP_DEFINE_CONST_DICT(solaros_jet_scene_locals,solaros_jet_scene_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(solaros_jet_scene_type,MP_QSTR_jet_scene,MP_TYPE_FLAG_NONE,
    locals_dict,&solaros_jet_scene_locals);
static mp_obj_t solaros_jet_open(size_t count,const mp_obj_t *args,mp_map_t *kwargs) {
    python_jet_callback_guard();
    const char *names[]={"display","fps"}; mp_obj_t v[2];
    python_jet_args(count,args,kwargs,names,2,v);
    solar_os_jet_options_t options={0};
    if(v[0]!=MP_OBJ_NULL && v[0]!=mp_const_none) {
        size_t n; options.display=mp_obj_str_get_data(v[0],&n);
        if(!n || n>=SOLAR_OS_DISPLAY_TARGET_NAME_MAX || memchr(options.display,0,n))
            mp_raise_ValueError(MP_ERROR_TEXT("invalid display name"));
    }
    options.fps=python_jet_integer(v[1],0,30,0);
    if(python_app.claimed_gfx || python_app.tui_active || python_emotion_drawing() || python_jet_drawing())
        mp_raise_ValueError(MP_ERROR_TEXT("close graphics, TUI, emotion and Jet scenes before opening Jet"));
    python_jet_t *self=mp_obj_malloc_with_finaliser(python_jet_t,&solaros_jet_scene_type);
    memset(self->shader_functions,0,sizeof(self->shader_functions));self->shader_error=MP_OBJ_NULL;
    memset(self->shader_ids,0,sizeof(self->shader_ids));
    self->generation=0;
    char current[SOLAR_OS_DISPLAY_TARGET_NAME_MAX]={0},detail[96]={0};
    if(python_app.session_gfx) solar_os_gfx_display_target_name(python_app.session_gfx,current,sizeof(current));
    bool foreground=*current && (!options.display || !strcmp(current,options.display));
    python_jet_lock();
    bool mode_started=foreground && !__atomic_load_n(&python_runtime_state->jet_suspended,__ATOMIC_ACQUIRE);
    esp_err_t err=foreground && !mode_started ? ESP_ERR_INVALID_STATE : (foreground ? python_jet_mode(true) : ESP_OK);
    if(err==ESP_OK) err=solar_os_jet_open(&options,python_app.session_gfx,&python_runtime_state->jet,detail,sizeof(detail));
    if(python_runtime_state->jet) __atomic_store_n(&python_runtime_state->jet_count,1,__ATOMIC_RELEASE);
    if(err!=ESP_OK && mode_started && !python_runtime_state->jet) (void)python_jet_mode(false);
    if(!++python_runtime_state->jet_generation) ++python_runtime_state->jet_generation;
    self->generation=python_runtime_state->jet_generation;
    python_jet_unlock();
    if(err!=ESP_OK) mp_raise_msg_varg(&mp_type_RuntimeError,MP_ERROR_TEXT("jet: %s (%s)"),detail,esp_err_to_name(err));
    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_jet_open_obj,0,solaros_jet_open);
#undef PYTHON_JET_CALL
#else
static bool python_jet_active(void) { return false; }
static bool python_jet_drawing(void) { return false; }
static void python_jet_destroy(void) {}
#endif
