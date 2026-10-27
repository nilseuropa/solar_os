#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "solar_os_jet_abi.h"
typedef intptr_t mp_obj_t;
typedef int mp_map_t;
typedef struct {void *buf;size_t len;} mp_buffer_info_t;
#define MP_OBJ_NULL INTPTR_MIN
#define mp_const_none (INTPTR_MIN+1)
#define MP_ERROR_TEXT(x) x
#define MP_BUFFER_READ 1
#define m_new(type,n) ((type *)calloc(n,sizeof(type)))
static void mp_raise_ValueError(const char *s) {(void)s;abort();}
static const char *mp_obj_str_get_data(mp_obj_t o,size_t *n) {(void)o;*n=0;return "";}
static bool mp_obj_is_true(mp_obj_t o) {return o!=0;}
static double mp_obj_get_float(mp_obj_t o) {return (double)o;}
static intptr_t mp_obj_get_int(mp_obj_t o) {return o;}
static uint32_t python_u32_from_obj(mp_obj_t o) {return (uint32_t)o;}
static uint32_t python_jet_color(mp_obj_t o) {return (uint32_t)o;}
static void mp_get_buffer_raise(mp_obj_t o,mp_buffer_info_t *b,int flags) {(void)o;(void)b;(void)flags;abort();}
static void mp_obj_get_array(mp_obj_t o,size_t *n,mp_obj_t **a) {(void)o;(void)n;(void)a;abort();}
static void python_jet_args(size_t count,const mp_obj_t *args,mp_map_t *kwargs,
    const char *const *names,unsigned size,mp_obj_t *values) {
    (void)kwargs;(void)names;assert(count<=size);
    for(unsigned i=0;i<size;++i)values[i]=i<count ? args[i] : MP_OBJ_NULL;
}
#include "jet_python_options.inc"
int main(void) {
    mp_obj_t args[]={-4096,-1,-450};solar_os_jet_command_t q={0};
    python_jet_options(3,args,NULL,jet_camera_fields,JET_CAM_COUNT,&q);
    assert(q.values[JET_CAM_x]==-4096&&q.values[JET_CAM_y]==-1&&q.values[JET_CAM_z]==-450);
    assert(q.fields==7&&q.values[JET_CAM_near]==16&&q.values[JET_CAM_far]==4096);
    solar_os_jet_field_t handle={"handle",JET_INT,0,0,4294967295.0,NULL};
    args[0]=(mp_obj_t)UINT32_MAX;q=(solar_os_jet_command_t){0};
    python_jet_options(1,args,NULL,&handle,1,&q);assert(q.values[0]==4294967295.0);
    return 0;
}
