#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "solar_os_jet.h"
#include "solar_os_memory.h"
#include "solar_os_native.h"
#include "solar_os_display.h"
#include "solar_os_frame_presenter.h"
#include "jet_fixture.h"
extern const solar_os_jet_api_v1_t *solar_os_jet_engine_api_v1(void);
extern bool solar_os_jet_engine_idle(void);
struct solar_os_gfx { unsigned width,height; uint32_t formats; };
static struct solar_os_gfx gfx;
static unsigned allocations,claims,leases,attempts,presented;
static int fail_at=-1;
static bool installed=true,join_ok=true,release_ok=true,display_busy=false,bad_api=false;
static bool legacy_api;
static bool terminal_blocked;
static solar_os_jet_api_v1_t provider;
static uint8_t image[64*48*2];
struct solar_os_frame_presenter {
    solar_os_frame_presenter_config_t config;
    uint8_t *buffers[3],*writing,*last;
    unsigned index;
    bool active;
    solar_os_frame_presenter_stats_t stats;
};
void *solar_os_memory_alloc(size_t bytes,solar_os_memory_class_t cls,const char *tag) {
    (void)cls;(void)tag;
    if((int)attempts++==fail_at) return NULL;
    void *p=malloc(bytes); if(p) ++allocations; return p;
}
void *solar_os_memory_calloc(size_t count,size_t bytes,solar_os_memory_class_t cls,const char *tag) {
    void *p=solar_os_memory_alloc(count*bytes,cls,tag); if(p) memset(p,0,count*bytes); return p;
}
void solar_os_memory_free(void *p) { if(p) {assert(allocations);--allocations;free(p);} }
size_t strlcpy(char *out,const char *in,size_t size) {
    size_t n=strlen(in); if(size) {size_t copied=n<size-1?n:size-1;memcpy(out,in,copied);out[copied]=0;} return n;
}
int64_t esp_timer_get_time(void) {
    struct timespec t; timespec_get(&t,TIME_UTC); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;
}
const char *esp_err_to_name(esp_err_t error) {(void)error;return "test error";}
size_t solar_os_gfx_width(const solar_os_gfx_t *g) {return g->width;}
size_t solar_os_gfx_height(const solar_os_gfx_t *g) {return g->height;}
bool solar_os_gfx_display_target_name(const solar_os_gfx_t *g,char *name,size_t size) {
    assert(g);strlcpy(name,"display0",size);return true;
}
bool solar_os_display_find_target(const char *name,solar_os_display_target_t *out) {
    if(strcmp(name,"display0")) return false;
    memset(out,0,sizeof(*out));out->frame_formats=gfx.formats;strcpy(out->owner,"session:0");return true;
}
esp_err_t solar_os_display_claim(const char *name,const char *owner,char *detail,size_t size) {
    (void)detail;(void)size;assert(!strcmp(name,"display0") && *owner);
    if(display_busy) return ESP_ERR_INVALID_STATE;
    assert(!claims);++claims;return ESP_OK;
}
esp_err_t solar_os_display_release(const char *name,const char *owner) {
    assert(!strcmp(name,"display0")&&*owner&&claims&&!terminal_blocked);--claims;return ESP_OK;
}
esp_err_t solar_os_display_set_terminal_blocked(const char *name,const char *owner,bool blocked) {
    assert(!strcmp(name,"display0")&&*owner&&claims);terminal_blocked=blocked;return ESP_OK;
}
esp_err_t solar_os_display_open_gfx(const char *name,const char *owner,solar_os_gfx_t **out,char *detail,size_t size) {
    esp_err_t err=solar_os_display_claim(name,owner,detail,size);if(!err)*out=&gfx;return err;
}
int solar_os_native_library_acquire(const char *id,uint32_t abi,uint32_t minimum,uint32_t *lease,const void **api) {
    assert(!strcmp(id,"jet")&&abi==1&&minimum==SOLAR_OS_JET_BASE_SIZE);
    if(!installed) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    assert(!leases);++leases;*lease=1;provider=*solar_os_jet_engine_api_v1();
    if(bad_api) provider.render=NULL;
    if(legacy_api) {provider.struct_size=SOLAR_OS_JET_BASE_SIZE;provider.extensions=(const void *)(uintptr_t)1;}
    *api=&provider;return 0;
}
int solar_os_native_library_release(uint32_t lease) {
    assert(lease==1&&leases==1&&solar_os_jet_engine_idle());
    if(!release_ok) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    --leases;return 0;
}
esp_err_t solar_os_frame_presenter_init(solar_os_frame_presenter_t **out,const solar_os_frame_presenter_config_t *c) {
    assert(c->format==SOLAR_OS_DISPLAY_FORMAT_RGB565&&c->allow_mono_fallback&&c->literal_mono_palette);
    assert(c->request_high_refresh==!(gfx.formats&SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT));
    assert(c->stride==gfx.width*2);
    solar_os_frame_presenter_t *p=solar_os_memory_calloc(1,sizeof(*p),0,"presenter");
    if(!p) return ESP_ERR_NO_MEM;
    p->config=*c;
    for(unsigned i=0;i<3;++i) {
        p->buffers[i]=solar_os_memory_alloc((size_t)c->stride*c->height,0,"frame");
        if(!p->buffers[i]) {solar_os_frame_presenter_deinit(p);return ESP_ERR_NO_MEM;}
    }
    p->writing=p->buffers[0];p->active=true;*out=p;return ESP_OK;
}
esp_err_t solar_os_frame_presenter_acquire(solar_os_frame_presenter_t *p,uint8_t **data,size_t *size) {
    *data=p->writing;*size=(size_t)p->config.stride*p->config.height;return ESP_OK;
}
esp_err_t solar_os_frame_presenter_exchange(solar_os_frame_presenter_t *p,uint8_t **data) {
    assert(p->active&&*data==p->writing&&claims&&leases);
    size_t bytes=(size_t)p->config.stride*p->config.height;assert(bytes<=sizeof(image));
    memcpy(image,*data,bytes);++presented;++p->stats.presented_frames;p->stats.present_us+=17;
    p->last=p->writing;p->index=(p->index+1)%3;p->writing=p->buffers[p->index];*data=p->writing;
    assert(p->writing!=p->last);return ESP_OK;
}
bool solar_os_frame_presenter_redraw(solar_os_frame_presenter_t *p) {return p&&p->active&&p->last;}
esp_err_t solar_os_frame_presenter_resume(solar_os_frame_presenter_t *p) {assert(claims&&leases);p->active=true;return ESP_OK;}
esp_err_t solar_os_frame_presenter_suspend_checked(solar_os_frame_presenter_t *p) {
    if(!p) return ESP_OK;
    if(!join_ok) return ESP_ERR_TIMEOUT;
    p->active=false;return ESP_OK;
}
esp_err_t solar_os_frame_presenter_deinit(solar_os_frame_presenter_t *p) {
    if(!p) return ESP_OK;
    assert(!p->active);for(unsigned i=0;i<3;++i) solar_os_memory_free(p->buffers[i]);
    solar_os_memory_free(p);return ESP_OK;
}
void solar_os_frame_presenter_take_stats(solar_os_frame_presenter_t *p,solar_os_frame_presenter_stats_t *stats) {
    memset(stats,0,sizeof(*stats));if(p) {*stats=p->stats;memset(&p->stats,0,sizeof(p->stats));}
}
void jet_test_reset(bool mono) {
    assert(!allocations&&!claims&&!leases&&solar_os_jet_engine_idle());
    gfx=(struct solar_os_gfx){64,48,mono?SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT:SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT};
    attempts=presented=0;installed=join_ok=release_ok=true;display_busy=bad_api=false;fail_at=-1;
}
solar_os_gfx_t *jet_test_gfx(void) {return &gfx;}
void jet_test_assert_clean(void) {assert(!allocations&&!claims&&!leases&&!terminal_blocked&&solar_os_jet_engine_idle());}
const uint8_t *jet_test_image(size_t *bytes) {*bytes=sizeof(image);return image;}
#ifndef JET_TEST_NO_MAIN
static bool cancel(void *user) {(void)user;return true;}
int main(void) {
    for(unsigned mono=0;mono<2;++mono) {
        jet_test_reset(mono);
        solar_os_jet_options_t options={0};solar_os_jet_t *s=NULL;char detail[96];
        installed=false;assert(solar_os_jet_open(&options,&gfx,&s,detail,sizeof(detail))==ESP_ERR_NOT_FOUND&&!s);
        jet_test_assert_clean();installed=true;
        bad_api=true;assert(solar_os_jet_open(&options,&gfx,&s,detail,sizeof(detail))==ESP_ERR_NOT_SUPPORTED&&!s);
        jet_test_assert_clean();bad_api=false;
        legacy_api=true;
        assert(solar_os_jet_open(&options,&gfx,&s,detail,sizeof(detail))==ESP_OK);
        solar_os_jet_command_t query={.opcode=SOLAR_OS_JET_INFO};solar_os_jet_reply_t reply;
        assert(solar_os_jet_execute(s,&query,&reply)==ESP_ERR_NOT_SUPPORTED);
        assert(solar_os_jet_render(s,NULL,NULL)==ESP_OK);
        assert(solar_os_jet_close(&s)==ESP_OK);jet_test_assert_clean();legacy_api=false;presented=0;
        bool complete=false;
        for(int failure=0;failure<30;++failure) {
            attempts=0;fail_at=failure;
            esp_err_t err=solar_os_jet_open(&options,&gfx,&s,detail,sizeof(detail));
            assert(err==ESP_OK||err==ESP_ERR_NO_MEM);
            assert(solar_os_jet_close(&s)==ESP_OK);jet_test_assert_clean();
            if(attempts<=(unsigned)failure) {complete=true;break;}
        }
        assert(complete);fail_at=-1;
        assert(solar_os_jet_open(&options,&gfx,&s,detail,sizeof(detail))==ESP_OK);
        solar_os_jet_t *other=NULL;
        assert(solar_os_jet_open(&options,&gfx,&other,detail,sizeof(detail))==ESP_ERR_INVALID_STATE&&!other);
        uint32_t cube=0;assert(solar_os_jet_cube(s,150,150,150,0x2192ff,&cube)==ESP_OK&&cube);
        assert(solar_os_jet_render(s,NULL,NULL)==ESP_OK&&presented==1);
        assert(solar_os_jet_render(s,cancel,NULL)==ESP_ERR_TIMEOUT&&presented==1);
        solar_os_jet_status_t stats={0};solar_os_jet_status(s,&stats);
        assert(stats.frames==1&&stats.presented==1&&stats.present_us==17);
        assert(solar_os_jet_suspend(s)==ESP_OK&&!claims&&leases&&!terminal_blocked);
        assert(solar_os_jet_render(s,NULL,NULL)==ESP_ERR_INVALID_STATE);
        display_busy=true;assert(solar_os_jet_resume(s)==ESP_ERR_INVALID_STATE&&!claims);
        display_busy=false;assert(solar_os_jet_resume(s)==ESP_OK&&claims&&terminal_blocked);
        assert(solar_os_jet_render(s,NULL,NULL)==ESP_OK&&presented==2);
        join_ok=false;assert(solar_os_jet_close(&s)==ESP_ERR_TIMEOUT&&s&&claims&&leases&&allocations&&terminal_blocked);
        join_ok=true;release_ok=false;
        assert(solar_os_jet_close(&s)==ESP_ERR_INVALID_STATE&&s&&leases&&allocations&&!claims);
        release_ok=true;assert(solar_os_jet_close(&s)==ESP_OK&&!s);jet_test_assert_clean();
        options.display="display0";
        assert(solar_os_jet_open(&options,NULL,&s,detail,sizeof(detail))==ESP_OK);
        assert(!solar_os_jet_uses_gfx(s,&gfx));
        assert(solar_os_jet_close(&s)==ESP_OK);jet_test_assert_clean();
    }
    puts("Jet frontend lease, exchange, admission failures and lifecycle passed");return 0;
}
#endif
