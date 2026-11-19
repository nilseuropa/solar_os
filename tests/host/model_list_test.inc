#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_camera.h"

#define SOLAR_OS_INFERENCE_MODELS_MAX 4
typedef int solar_os_shell_io_t;
typedef int solar_os_inference_t;
typedef struct { solar_os_shell_io_t *io; } model_client_t;
typedef struct { bool bundle;const char *bundle_id,*path;unsigned mode,references; } solar_os_inference_model_info_t;
static solar_os_inference_model_info_t model={true,"arithmetic","/sdcard/model.espdl",0,2};
static solar_os_camera_status_t camera;
static unsigned residents,creates,destroys,queries,summaries;
static esp_err_t resident_error,info_error,provider_error,camera_error;
static char output[4096];
static void emit(const char *fmt,...)
{ va_list args;va_start(args,fmt);vsnprintf(output+strlen(output),sizeof(output)-strlen(output),fmt,args);va_end(args); }
static void solar_os_shell_io_table_writeln(solar_os_shell_io_t *io,const char *s) { (void)io;emit("%s\n",s); }
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io,const char *s) { (void)io;emit("%s\n",s); }
static void solar_os_shell_io_write(solar_os_shell_io_t *io,const char *s) { (void)io;emit("%s",s); }
#define solar_os_shell_io_table_printf(io,...) ((void)(io),emit(__VA_ARGS__))
static void solar_os_shell_diag_esp(solar_os_shell_io_t *io,const char *cmd,esp_err_t e,const char *a,const char *b)
{ (void)io;(void)a;(void)b;assert(e);emit("%s: unavailable\n",cmd); }
static bool cancelled(void *u) { (void)u;return false; }
static esp_err_t solar_os_inference_list(uint32_t *ids,size_t cap,size_t *n)
{ assert(cap>=residents);*n=residents;if(residents)ids[0]=7;return resident_error; }
static esp_err_t solar_os_inference_create(bool(*c)(void*),void *u,solar_os_inference_t **out)
{ assert(c==cancelled&&u);creates++;*out=malloc(sizeof(**out));assert(*out);return ESP_OK; }
static void solar_os_inference_destroy(solar_os_inference_t *p) { if(p){destroys++;free(p);} }
static esp_err_t solar_os_inference_info(solar_os_inference_t *p,uint32_t id,const solar_os_inference_model_info_t **out)
{ assert(p&&id==7);*out=&model;return info_error; }
static const char *solar_os_inference_mode_name(unsigned mode) { assert(mode==0);return "auto"; }
esp_err_t solar_os_camera_get_status(solar_os_camera_status_t *out) { *out=camera;return camera_error; }
static esp_err_t solar_os_processor_query(const char *kind,const char *source,const char *what,char **out)
{ assert(!strcmp(kind,"test-ai")&&!strcmp(source,"camera-test")&&!strcmp(what,"slots"));queries++;*out=NULL;if(provider_error)return provider_error;*out=malloc(3);assert(*out);strcpy(*out,"{}");return ESP_OK; }
static void solar_os_memory_free(void *p) { free(p); }
static esp_err_t print_slots(solar_os_shell_io_t *io,const char *json,bool overview)
{ (void)io;assert(json&&overview);summaries++;emit("slot=4 active programmed detector 1.0\n");return ESP_OK; }

/* PRODUCTION */

int main(void) {
    model_client_t state={0};
    assert(!list_resident_models(&state));list_camera_models(NULL);
    assert(strstr(output,"ESP-DL resident models\nNo resident models")&&!creates&&!queries);
    camera.backend_registered=true;list_camera_models(NULL);assert(!queries);
    strcpy(camera.processor,"test-ai");strcpy(camera.processor_source,"camera-test");residents=1;*output=0;
    assert(!list_resident_models(&state));list_camera_models(NULL);
    assert(creates==1&&destroys==1&&queries==1&&summaries==1);
    assert(strstr(output,"7 arithmetic auto refs=2\n  /sdcard/model.espdl\n"));
    assert(strstr(output,"test-ai camera-test model slots")&&strstr(output,"slot=4 active programmed"));
    camera.owner_leased=true;*output=0;
    assert(!list_resident_models(&state));list_camera_models(NULL);
    assert(queries==1&&strstr(output,"7 arithmetic")&&strstr(output,"camera is in use"));
    camera.owner_leased=false;provider_error=ESP_ERR_NOT_FOUND;*output=0;
    assert(!list_resident_models(&state));list_camera_models(NULL);
    assert(queries==2&&strstr(output,"7 arithmetic")&&strstr(output,"inventory unavailable"));
    provider_error=0;resident_error=ESP_ERR_INVALID_STATE;
    assert(list_resident_models(&state)==ESP_ERR_INVALID_STATE);list_camera_models(NULL);assert(queries==3);
    resident_error=0;info_error=ESP_ERR_NOT_FOUND;
    assert(list_resident_models(&state)==ESP_ERR_NOT_FOUND&&creates==destroys);
    camera_error=ESP_FAIL;list_camera_models(NULL);assert(queries==3&&creates==destroys);
    puts("model list: independent backends, provider discovery, busy handling and resident handle compatibility passed");
    return 0;
}
