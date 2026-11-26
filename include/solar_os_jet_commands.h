#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "solar_os_jet_wire_types_v1.h"

/* Shared control schema for the provider and both interpreters. Payloads are
 * borrowed for one synchronous call; providers copy all retained resources.
 * Numeric slots use doubles so integer handles retain all 32 bits. */
#define SOLAR_OS_JET_MAX_TEXTURES 16
#define SOLAR_OS_JET_MAX_MATERIALS 32
#define SOLAR_OS_JET_MAX_LIGHTS 8
#define SOLAR_OS_JET_MAX_SPRITES 32
#define SOLAR_OS_JET_MAX_PICKS 32
#define SOLAR_OS_JET_MAX_PROTOTYPES 16
#define SOLAR_OS_JET_MAX_INSTANCES 32
#define SOLAR_OS_JET_MAX_PARTICLES 4
#define SOLAR_OS_JET_MAX_FLARES 4
#define SOLAR_OS_JET_MAX_FLARE_ELEMENTS 12
#define SOLAR_OS_JET_MAX_SHADERS 16
typedef struct {uint32_t fields;double values[SOLAR_OS_JET_VALUES];} solar_os_jet_element_t;
typedef struct {int16_t x,y;} solar_os_jet_pick_t;
enum {
    SOLAR_OS_JET_FEATURE_MATERIALS=1, SOLAR_OS_JET_FEATURE_TEXTURES=2,
    SOLAR_OS_JET_FEATURE_GEOMETRY=4, SOLAR_OS_JET_FEATURE_SCENE=8,
    SOLAR_OS_JET_FEATURE_SPRITES=16, SOLAR_OS_JET_FEATURE_PICKING=32,
    SOLAR_OS_JET_FEATURE_ASSETS=64,
    SOLAR_OS_JET_FEATURE_INSTANCING=128,
    SOLAR_OS_JET_FEATURE_PARTICLES=256, SOLAR_OS_JET_FEATURE_FLARES=512,
    SOLAR_OS_JET_FEATURE_POSTFX=1024,
    SOLAR_OS_JET_FEATURE_TILED_TEXTURES=2048,
    SOLAR_OS_JET_FEATURE_SHADERS=4096,
    SOLAR_OS_JET_FEATURE_RENDERER_CONTROLS=8192,
    SOLAR_OS_JET_FEATURE_BACKGROUND_CACHE=16384,
};
typedef struct {
    int32_t x,y,z,u,v,nx,ny,nz;
} solar_os_jet_mesh_vertex_t;
typedef struct { uint16_t a,b,c,pad; uint32_t material; } solar_os_jet_mesh_face_t;
enum { SOLAR_OS_JET_MESH_NORMALS=1 };

enum { JET_INT, JET_FLOAT, JET_BOOL, JET_ENUM, JET_NAME, JET_BYTES, JET_PALETTE, JET_HANDLES };
typedef struct {
    const char *name;
    uint8_t kind;
    double initial,minimum,maximum;
    const char *choices;
} solar_os_jet_field_t;
#define JET_MATERIAL_FIELDS(X) \
 X(color,INT,16777215,0,16777215,NULL) \
 X(shading,ENUM,1,0,6,"flat|gouraud|phong|wireframe|unlit|water|additive") \
 X(texture,INT,0,0,4294967295.0,NULL) \
 X(alpha,INT,255,0,255,NULL) \
 X(diffuse,INT,255,0,255,NULL) \
 X(specular,INT,0,0,255,NULL) \
 X(gloss,INT,0,0,255,NULL) \
 X(emissive,BOOL,0,0,1,NULL) \
 X(perspective,BOOL,1,0,1,NULL) \
 X(water_bias,INT,0,0,255,NULL) \
 X(reflection_max_y,INT,-1,-1,1023,NULL) \
 X(name,NAME,0,0,0,NULL) \
 X(shader,INT,0,0,4294967295.0,NULL)
#define JET_SHADER_FIELDS(X) \
 X(kind,ENUM,0,0,3,"checker|uv|depth|callback") \
 X(color,INT,16777215,0,16777215,NULL) \
 X(secondary,INT,0,0,16777215,NULL) \
 X(scale,INT,8,1,4096,NULL) \
 X(screen_space,BOOL,0,0,1,NULL) \
 X(alpha_key,INT,-1,-1,16777215,NULL) \
 X(name,NAME,0,0,0,NULL)
#define JET_TEXTURE_FIELDS(X) \
 X(format,ENUM,0,0,2,"rgb565|rgb888|index8") \
 X(address,ENUM,0,0,2,"wrap|clamp|zero") \
 X(alpha_key,INT,-1,-1,16777215,NULL) \
 X(screen_space,BOOL,0,0,1,NULL) \
 X(reflection,BOOL,0,0,1,NULL) \
 X(bilinear,BOOL,0,0,1,NULL) \
 X(offset,INT,0,0,255,NULL) \
 X(palette,PALETTE,0,0,0,NULL) \
 X(pixels,BYTES,0,0,0,NULL) \
 X(name,NAME,0,0,0,NULL) \
 X(tiled,BOOL,0,0,1,NULL) \
 X(filter,ENUM,0,0,3,"nearest|bilinear|three_point|cached_bilinear") \
 X(mip_level,INT,0,0,5,NULL)
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_MAT_##name,
enum { JET_MATERIAL_FIELDS(JET_FIELD_ENUM) JET_MAT_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_SHADER_##name,
enum { JET_SHADER_FIELDS(JET_FIELD_ENUM) JET_SHADER_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_ENUM(name,kind,initial,low,high,choices) JET_TEX_##name,
enum { JET_TEXTURE_FIELDS(JET_FIELD_ENUM) JET_TEX_COUNT };
#undef JET_FIELD_ENUM
#define JET_FIELD_VALUE(name,kind,initial,low,high,choices) {#name,JET_##kind,initial,low,high,choices},
static const solar_os_jet_field_t jet_material_fields[]={JET_MATERIAL_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_shader_fields[]={JET_SHADER_FIELDS(JET_FIELD_VALUE)};
static const solar_os_jet_field_t jet_texture_fields[]={JET_TEXTURE_FIELDS(JET_FIELD_VALUE)};
#undef JET_FIELD_VALUE
static inline int solar_os_jet_choice(const char *choices,const char *text,size_t length) {
    int index=0;
    while(choices && *choices) {
        const char *end=strchr(choices,'|');size_t n=end ? (size_t)(end-choices) : strlen(choices);
        if(n==length && !memcmp(choices,text,n)) return index;
        choices=end ? end+1 : NULL;++index;
    }
    return -1;
}
static inline const char *solar_os_jet_choice_at(const char *choices,unsigned index,size_t *length) {
    while(index-- && choices) {const char *end=strchr(choices,'|');choices=end ? end+1 : NULL;}
    if(!choices) {*length=0;return "";}
    const char *end=strchr(choices,'|');*length=end ? (size_t)(end-choices) : strlen(choices);return choices;
}
