#pragma once
#include "solar_os_jet_commands.h"
#define JET_PARTICLE_FIELDS(X) \
 X(world_scale,FLOAT,1,0.015625,16,NULL) \
 X(additive,BOOL,0,0,1,NULL) \
 X(seed,INT,1,1,4294967295.0,NULL) \
 X(enabled,BOOL,1,0,1,NULL) \
 X(name,NAME,0,0,0,NULL)
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_PART_##name,
enum { JET_PARTICLE_FIELDS(JET_FIELD_ENUM) JET_PART_COUNT };
#undef JET_FIELD_ENUM
#define JET_EMIT_FIELDS(X) \
 X(kind,ENUM,0,0,1,"sparks|splash") \
 X(x,FLOAT,0,-4096,4096,NULL) \
 X(y,FLOAT,0,-4096,4096,NULL) \
 X(z,FLOAT,0,-4096,4096,NULL) \
 X(nx,FLOAT,0,-1,1,NULL) \
 X(ny,FLOAT,1,-1,1,NULL) \
 X(nz,FLOAT,0,-1,1,NULL) \
 X(speed,FLOAT,100,0,4096,NULL) \
 X(count,INT,24,0,200,NULL) \
 X(vx,FLOAT,0,-4096,4096,NULL) \
 X(vy,FLOAT,0,-4096,4096,NULL) \
 X(vz,FLOAT,0,-4096,4096,NULL) \
 X(tx,FLOAT,0,-1,1,NULL) \
 X(ty,FLOAT,0,-1,1,NULL) \
 X(tz,FLOAT,1,-1,1,NULL) \
 X(sx,FLOAT,1,-1,1,NULL) \
 X(sy,FLOAT,0,-1,1,NULL) \
 X(sz,FLOAT,0,-1,1,NULL) \
 X(ux,FLOAT,0,-1,1,NULL) \
 X(uy,FLOAT,1,-1,1,NULL) \
 X(uz,FLOAT,0,-1,1,NULL)
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_EMIT_##name,
enum { JET_EMIT_FIELDS(JET_FIELD_ENUM) JET_EMIT_COUNT };
#undef JET_FIELD_ENUM
#define JET_FLARE_FIELDS(X) \
 X(x,FLOAT,0,-1,1,NULL) \
 X(y,FLOAT,0,-1,1,NULL) \
 X(z,FLOAT,1,-1,1,NULL) \
 X(fade_speed,FLOAT,4,0,1024,NULL) \
 X(enabled,BOOL,1,0,1,NULL) \
 X(name,NAME,0,0,0,NULL)
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_FLARE_##name,
enum { JET_FLARE_FIELDS(JET_FIELD_ENUM) JET_FLARE_COUNT };
#undef JET_FIELD_ENUM
#define JET_ELEMENT_FIELDS(X) \
 X(material,INT,0,0,4294967295.0,NULL) \
 X(width,INT,32,1,1024,NULL) \
 X(height,INT,32,1,1024,NULL) \
 X(axis,FLOAT,0,-2,2,NULL) \
 X(alpha,FLOAT,1,0,1,NULL) \
 X(blend,ENUM,1,0,1,"replace|add") \
 X(scale,INT,1,1,16,NULL) \
 X(flip_x,BOOL,0,0,1,NULL) \
 X(flip_y,BOOL,0,0,1,NULL) \
 X(mirror_x,BOOL,0,0,1,NULL) \
 X(mirror_y,BOOL,0,0,1,NULL) \
 X(z_order,INT,100,-32768,32767,NULL)
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_ELEM_##name,
enum { JET_ELEMENT_FIELDS(JET_FIELD_ENUM) JET_ELEM_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_VALUE(name,kind,initial,low,high,choices) {#name,JET_##kind,initial,low,high,choices},
#define JET_POSTFX_FIELDS(X) \
 X(fxaa,BOOL,0,0,1,NULL) X(bloom,BOOL,0,0,1,NULL) X(crt,INT,0,0,255,NULL) \
 X(motion_blur,INT,0,0,100,NULL) X(chromatic,INT,0,0,32,NULL) X(pixelate,INT,1,1,32,NULL) X(cel_bits,INT,0,0,8,NULL)
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_FX_##name,
enum { JET_POSTFX_FIELDS(JET_FIELD_ENUM) JET_FX_COUNT };
#undef JET_FIELD_ENUM
static const solar_os_jet_field_t jet_particle_fields[]={JET_PARTICLE_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_emit_fields[]={JET_EMIT_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_flare_fields[]={JET_FLARE_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_element_fields[]={JET_ELEMENT_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_postfx_fields[]={JET_POSTFX_FIELDS(JET_FIELD_VALUE)};
#undef JET_FIELD_VALUE
