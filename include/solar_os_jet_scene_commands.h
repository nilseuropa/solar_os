#pragma once
#include "solar_os_jet_commands.h"
#define JET_PRIMITIVE_FIELDS(X) \
 X(kind,ENUM,0,0,9,"cube|grid|plane|pyramid|sphere|capsule|cylinder|billboard|quad|debug_cube") \
 X(width,INT,150,1,4096,NULL) X(height,INT,150,1,4096,NULL) X(depth,INT,150,1,4096,NULL) \
 X(radius,INT,75,1,4096,NULL) X(segments,INT,12,3,32,NULL) \
 X(rows,INT,4,2,32,NULL) X(cols,INT,4,2,32,NULL) X(caps,BOOL,1,0,1,NULL) \
 X(u_scale,INT,1,1,32,NULL) X(v_scale,INT,1,1,32,NULL) \
 X(material,INT,0,0,4294967295.0,NULL) X(material2,INT,0,0,4294967295.0,NULL) \
 X(color,INT,16777215,0,16777215,NULL) X(per_cell_uv,BOOL,0,0,1,NULL) X(name,NAME,0,0,0,NULL)
#define JET_OBJECT_FIELDS(X) \
 X(x,INT,0,-4096,4096,NULL) X(y,INT,0,-4096,4096,NULL) X(z,INT,0,-4096,4096,NULL) \
 X(rx,INT,0,-360000,360000,NULL) X(ry,INT,0,-360000,360000,NULL) X(rz,INT,0,-360000,360000,NULL) \
 X(scale_x,FLOAT,1,0.015625,16,NULL) X(scale_y,FLOAT,1,0.015625,16,NULL) X(scale_z,FLOAT,1,0.015625,16,NULL) \
 X(enabled,BOOL,1,0,1,NULL) X(culling,ENUM,0,0,2,"back|front|none") \
 X(blend,ENUM,0,0,5,"replace|add|subtract|scale|average|xor") X(billboard,BOOL,0,0,1,NULL) \
 X(depth_test,BOOL,1,0,1,NULL) X(depth_write,BOOL,1,0,1,NULL) X(depth_bias,INT,0,-128,127,NULL) \
 X(precise_sort,BOOL,0,0,1,NULL) X(fade_near,INT,0,0,16384,NULL) X(fade_far,INT,0,0,16384,NULL) \
 X(appear_near,INT,0,0,16384,NULL) X(appear_far,INT,0,0,16384,NULL) X(lod_bias,INT,0,-8,8,NULL) \
 X(lod_persist,BOOL,1,0,1,NULL) X(name,NAME,0,0,0,NULL)
#define JET_OBJECT_ACTION_FIELDS(X) \
 X(action,ENUM,0,0,9,"translate|rotate|look_at|bake_rotation|bake_scale|cache_positions|invalidate_positions|clone|bake_lighting|unbake") \
 X(x,INT,0,-4096,4096,NULL) X(y,INT,0,-4096,4096,NULL) X(z,INT,0,-4096,4096,NULL) \
 X(numerator,INT,1,1,16384,NULL) X(denominator,INT,1,1,16384,NULL)
#define JET_CAMERA_FIELDS(X) \
 X(x,INT,0,-4096,4096,NULL) X(y,INT,0,-4096,4096,NULL) X(z,INT,-500,-4096,4096,NULL) \
 X(rx,INT,0,-360000,360000,NULL) X(ry,INT,0,-360000,360000,NULL) X(rz,INT,0,-360000,360000,NULL) \
 X(fov,INT,60,10,150,NULL) X(near,INT,16,1,4095,NULL) X(far,INT,4096,2,4096,NULL)
#define JET_CAMERA_ACTION_FIELDS(X) \
 X(action,ENUM,0,0,7,"translate|translate_local|translate_local_y|rotate|rotate_local|look_at|look_at_x|look_at_y") \
 X(x,INT,0,-4096,4096,NULL) X(y,INT,0,-4096,4096,NULL) X(z,INT,0,-4096,4096,NULL)
#define JET_POINT_FIELDS(X) \
 X(x,INT,0,-4096,4096,NULL) X(y,INT,0,-4096,4096,NULL) X(z,INT,0,-4096,4096,NULL) \
 X(color,INT,16777215,0,16777215,NULL) X(range_min,INT,0,0,16383,NULL) X(range_max,INT,255,1,16384,NULL) \
 X(enabled,BOOL,1,0,1,NULL) X(name,NAME,0,0,0,NULL)
#define JET_SCENE_FIELDS(X) \
 X(clear,BOOL,1,0,1,NULL) X(lod_scale,INT,0,0,16384,NULL) X(lod_bias,INT,0,-8,8,NULL) \
 X(water_time,FLOAT,0,0,1000000,NULL) \
 X(texture_lod,BOOL,0,0,1,NULL) X(texture_near,INT,0,0,16384,NULL) X(texture_far,INT,0,0,16384,NULL) \
 X(wireframe,BOOL,0,0,1,NULL) X(background_cache,BOOL,0,0,1,NULL)
#define JET_SPRITE_FIELDS(X) \
 X(x,INT,0,-4096,4096,NULL) X(y,INT,0,-4096,4096,NULL) \
 X(width,INT,32,0,4096,NULL) X(height,INT,32,0,4096,NULL) \
 X(material,INT,0,0,4294967295.0,NULL) X(color,INT,16777215,0,16777215,NULL) \
 X(alpha,INT,255,0,255,NULL) X(z_order,INT,0,-32768,32767,NULL) X(enabled,BOOL,1,0,1,NULL) \
 X(blend,ENUM,0,0,1,"replace|add") X(scale,INT,1,1,16,NULL) \
 X(flip_x,BOOL,0,0,1,NULL) X(flip_y,BOOL,0,0,1,NULL) \
 X(mirror_x,BOOL,0,0,1,NULL) X(mirror_y,BOOL,0,0,1,NULL) X(name,NAME,0,0,0,NULL)
#define JET_OBJ_LOAD_FIELDS(X) \
 X(scale,FLOAT,1,0.0009765625,32,NULL) X(material,INT,0,0,4294967295.0,NULL) \
 X(color,INT,16777215,0,16777215,NULL) X(name,NAME,0,0,0,NULL)
#define JET_INSTANCE_FIELDS(X) \
 X(x,INT,0,-4096,4096,NULL) X(y,INT,0,-4096,4096,NULL) X(z,INT,0,-4096,4096,NULL) \
 X(rx,INT,0,-360000,360000,NULL) X(ry,INT,0,-360000,360000,NULL) X(rz,INT,0,-360000,360000,NULL) \
 X(material,INT,0,0,4294967295.0,NULL) X(materials,HANDLES,0,0,0,NULL)
#define JET_PROTOTYPE_FIELDS(X) X(name,NAME,0,0,0,NULL)
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_INST_##name,
enum { JET_INSTANCE_FIELDS(JET_FIELD_ENUM) JET_INST_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_LOAD_##name,
enum { JET_OBJ_LOAD_FIELDS(JET_FIELD_ENUM) JET_LOAD_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_SPR_##name,
enum { JET_SPRITE_FIELDS(JET_FIELD_ENUM) JET_SPR_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_PRIM_##name,
enum { JET_PRIMITIVE_FIELDS(JET_FIELD_ENUM) JET_PRIM_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_OBJ_##name,
enum { JET_OBJECT_FIELDS(JET_FIELD_ENUM) JET_OBJ_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_OACT_##name,
enum { JET_OBJECT_ACTION_FIELDS(JET_FIELD_ENUM) JET_OACT_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_CAM_##name,
enum { JET_CAMERA_FIELDS(JET_FIELD_ENUM) JET_CAM_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_CACT_##name,
enum { JET_CAMERA_ACTION_FIELDS(JET_FIELD_ENUM) JET_CACT_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_POINT_##name,
enum { JET_POINT_FIELDS(JET_FIELD_ENUM) JET_POINT_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_SCENE_##name,
enum { JET_SCENE_FIELDS(JET_FIELD_ENUM) JET_SCENE_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_VALUE(name,kind,initial,low,high,choices) {#name,JET_##kind,initial,low,high,choices},
static const solar_os_jet_field_t jet_primitive_fields[]={JET_PRIMITIVE_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_object_fields[]={JET_OBJECT_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_object_action_fields[]={JET_OBJECT_ACTION_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_camera_fields[]={JET_CAMERA_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_camera_action_fields[]={JET_CAMERA_ACTION_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_point_fields[]={JET_POINT_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_scene_fields[]={JET_SCENE_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_sprite_fields[]={JET_SPRITE_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_obj_load_fields[]={JET_OBJ_LOAD_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_instance_fields[]={JET_INSTANCE_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_prototype_fields[]={JET_PROTOTYPE_FIELDS(JET_FIELD_VALUE)};
#undef JET_FIELD_VALUE
