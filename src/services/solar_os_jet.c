#include "solar_os_jet.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "solar_os_display.h"
#include "solar_os_frame_presenter.h"
#include "solar_os_memory.h"
#include "solar_os_native.h"

struct solar_os_jet {
    const solar_os_jet_api_v1_t *api;
    void *engine;
    uint32_t lease;
    solar_os_gfx_t *gfx;
    solar_os_frame_presenter_t *presenter;
    uint16_t *depth;
    uint8_t *writing;
    size_t bytes;
    bool claimed, registered, foreground, paused, terminal_blocked;
    char target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    solar_os_jet_status_t stats;
};
static atomic_bool occupied;
static esp_err_t result(int value) {
    switch(value) {
    case SOLAR_OS_NATIVE_OK: return ESP_OK;
    case SOLAR_OS_NATIVE_ERROR_FAILED: return ESP_FAIL;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: return ESP_ERR_NO_MEM;
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: return ESP_ERR_NOT_FOUND;
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: return ESP_ERR_INVALID_ARG;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: return ESP_ERR_NOT_SUPPORTED;
    case SOLAR_OS_NATIVE_ERROR_INVALID_SIZE: return ESP_ERR_INVALID_SIZE;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: return ESP_ERR_TIMEOUT;
    default: return ESP_ERR_INVALID_STATE;
    }
}
static void collect(solar_os_jet_t *s) {
    solar_os_frame_presenter_stats_t stats = {0};
    solar_os_frame_presenter_take_stats(s->presenter,&stats);
    s->stats.presented += stats.presented_frames; s->stats.dropped += stats.replaced_frames;
    s->stats.present_us += stats.present_us;
    if(stats.last_error!=ESP_OK) s->stats.error=stats.last_error;
}
esp_err_t solar_os_jet_close(solar_os_jet_t **instance) {
    if(!instance) return ESP_ERR_INVALID_ARG;
    solar_os_jet_t *s=*instance;
    if(!s) return ESP_OK;
    esp_err_t err=solar_os_frame_presenter_suspend_checked(s->presenter);
    if(err!=ESP_OK) return err;
    if(s->engine) {
        err=result(s->api->destroy(s->engine)); if(err!=ESP_OK) return err;
        s->engine=NULL;
    }
    if(s->presenter) {
        err=solar_os_frame_presenter_deinit(s->presenter); if(err!=ESP_OK) return err;
        s->presenter=NULL;
    }
    if(s->claimed) {
        if(s->terminal_blocked) {
            err=solar_os_display_set_terminal_blocked(s->target,s->owner,false); if(err!=ESP_OK) return err;
            s->terminal_blocked=false;
        }
        err=solar_os_display_release(s->target,s->owner); if(err!=ESP_OK) return err;
        s->claimed=false;
    }
    if(s->lease) {
        err=result(solar_os_native_library_release(s->lease)); if(err!=ESP_OK) return err;
        s->lease=0;
    }
    solar_os_memory_free(s->depth);
    if(s->registered) atomic_store(&occupied,false);
    solar_os_memory_free(s); *instance=NULL; return ESP_OK;
}
esp_err_t solar_os_jet_open(const solar_os_jet_options_t *o,solar_os_gfx_t *foreground,
    solar_os_jet_t **out,char *detail,size_t size) {
    if(!o || !out || *out || o->fps>30) return ESP_ERR_INVALID_ARG;
    if(detail && size) detail[0]=0;
    solar_os_jet_t *s=solar_os_memory_calloc(1,sizeof(*s),SOLAR_OS_MEMORY_INTERNAL_CRITICAL,"jet.runtime");
    if(!s) return ESP_ERR_NO_MEM;
    esp_err_t err=ESP_ERR_INVALID_STATE;
    bool expected=false;
    if(!atomic_compare_exchange_strong(&occupied,&expected,true)) {
        if(detail && size) snprintf(detail,size,"a Jet scene is already open");
        goto fail;
    }
    s->registered=true;
    char current[SOLAR_OS_DISPLAY_TARGET_NAME_MAX]={0};
    if(foreground) solar_os_gfx_display_target_name(foreground,current,sizeof(current));
    const char *name=o->display ? o->display : current;
    solar_os_display_target_t target;
    if(!name || !*name || strlen(name)>=sizeof(s->target) || !solar_os_display_find_target(name,&target)) {
        err=ESP_ERR_NOT_FOUND; goto fail;
    }
    if(!(target.frame_formats & (SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT|SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT))) {
        err=ESP_ERR_NOT_SUPPORTED; goto fail;
    }
    strlcpy(s->target,name,sizeof(s->target));
    s->foreground=foreground && !strcmp(name,current);
    if(s->foreground) {
        strlcpy(s->owner,target.owner[0] ? target.owner : "jet",sizeof(s->owner));
        err=solar_os_display_claim(name,s->owner,detail,size); s->gfx=foreground;
    } else {
        strlcpy(s->owner,"jet",sizeof(s->owner));
        err=solar_os_display_open_gfx(name,s->owner,&s->gfx,detail,size);
    }
    if(err!=ESP_OK) goto fail;
    s->claimed=true;
    err=solar_os_display_set_terminal_blocked(name,s->owner,true); if(err!=ESP_OK) goto fail;
    s->terminal_blocked=true;
    size_t width=solar_os_gfx_width(s->gfx),height=solar_os_gfx_height(s->gfx);
    if(!width || !height || width%8 || width>1024 || height>1024) {err=ESP_ERR_INVALID_SIZE;goto fail;}
    s->stats.width=width; s->stats.height=height; s->bytes=width*height*2;
    const void *api=NULL;
    err=result(solar_os_native_library_acquire(SOLAR_OS_JET_LIBRARY,SOLAR_OS_JET_ABI,
        SOLAR_OS_JET_BASE_SIZE,&s->lease,&api));
    if(err!=ESP_OK) {
        if(detail && size) snprintf(detail,size,"install compatible jet library first");
        goto fail;
    }
    s->api=api;
    if(!s->api->create || !s->api->destroy || !s->api->background || !s->api->camera ||
        !s->api->light || !s->api->cube || !s->api->mesh || !s->api->transform || !s->api->remove || !s->api->render) {
        err=ESP_ERR_NOT_SUPPORTED; goto fail;
    }
    bool direct=(target.frame_formats & SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT)!=0;
    solar_os_frame_presenter_config_t config={
        .gfx=s->gfx,.format=SOLAR_OS_DISPLAY_FORMAT_RGB565,
        .width=width,.height=height,.stride=width*2,.fit=SOLAR_OS_FRAME_FIT_HEIGHT,
        .preferred_fps=o->fps ? o->fps : (direct ? 30 : 0),
        .allow_mono_fallback=true,.literal_mono_palette=true,
        .request_high_refresh=!direct,.clear_background_on_resume=true,.background_rgb565=0xffff,
    };
    err=solar_os_frame_presenter_init(&s->presenter,&config); if(err!=ESP_OK) goto fail;
    size_t capacity=0;
    err=solar_os_frame_presenter_acquire(s->presenter,&s->writing,&capacity); if(err!=ESP_OK) goto fail;
    if(capacity!=s->bytes) {err=ESP_ERR_INVALID_SIZE;goto fail;}
    s->depth=solar_os_memory_alloc(s->bytes,SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"jet.depth");
    if(!s->depth) {err=ESP_ERR_NO_MEM;goto fail;}
    err=result(s->api->create(width,height,(uint16_t *)s->writing,s->depth,s->bytes,&s->engine));
    if(err!=ESP_OK) goto fail;
    *out=s; return ESP_OK;
fail:
    if(solar_os_jet_close(&s)!=ESP_OK) *out=s;
    return err;
}
bool solar_os_jet_uses_gfx(const solar_os_jet_t *s,const solar_os_gfx_t *gfx) {
    return s && s->foreground && s->gfx==gfx;
}
esp_err_t solar_os_jet_suspend(solar_os_jet_t *s) {
    if(!s) return ESP_ERR_INVALID_ARG;
    esp_err_t err=solar_os_frame_presenter_suspend_checked(s->presenter);
    if(err==ESP_OK) {
        s->paused=true;
        if(s->terminal_blocked) {
            err=solar_os_display_set_terminal_blocked(s->target,s->owner,false);
            if(err!=ESP_OK) return err;
            s->terminal_blocked=false;
        }
        if(s->foreground && s->claimed) {
            err=solar_os_display_release(s->target,s->owner);
            if(err==ESP_OK) s->claimed=false;
        }
    }
    return err;
}
esp_err_t solar_os_jet_resume(solar_os_jet_t *s) {
    if(!s) return ESP_ERR_INVALID_ARG;
    if(!s->paused) { (void)solar_os_frame_presenter_redraw(s->presenter); return ESP_OK; }
    if(!s->claimed) {
        esp_err_t err=solar_os_display_claim(s->target,s->owner,NULL,0);
        if(err!=ESP_OK) return err;
        s->claimed=true;
    }
    if(!s->terminal_blocked) {
        esp_err_t err=solar_os_display_set_terminal_blocked(s->target,s->owner,true);
        if(err!=ESP_OK) return err;
        s->terminal_blocked=true;
    }
    esp_err_t err=solar_os_frame_presenter_resume(s->presenter);
    if(err==ESP_OK) s->paused=false;
    return err;
}
esp_err_t solar_os_jet_background(solar_os_jet_t *s,uint32_t c) {
    return s && s->engine ? result(s->api->background(s->engine,c)) : ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_jet_camera(solar_os_jet_t *s,const solar_os_jet_pose_t *p,uint16_t fov) {
    return s && s->engine ? result(s->api->camera(s->engine,p,fov)) : ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_jet_light(solar_os_jet_t *s,uint32_t a,uint32_t c,uint16_t intensity,int32_t x,int32_t y) {
    return s && s->engine ? result(s->api->light(s->engine,a,c,intensity,x,y)) : ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_jet_cube(solar_os_jet_t *s,uint16_t w,uint16_t h,uint16_t d,uint32_t c,uint32_t *id) {
    return s && s->engine ? result(s->api->cube(s->engine,w,h,d,c,id)) : ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_jet_mesh(solar_os_jet_t *s,const solar_os_jet_vertex_t *v,size_t n,
    const solar_os_jet_triangle_t *t,size_t count,uint32_t c,uint32_t *id) {
    return s && s->engine ? result(s->api->mesh(s->engine,v,n,t,count,c,id)) : ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_jet_transform(solar_os_jet_t *s,uint32_t id,const solar_os_jet_pose_t *p) {
    return s && s->engine ? result(s->api->transform(s->engine,id,p)) : ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_jet_remove(solar_os_jet_t *s,uint32_t id) {
    return s && s->engine ? result(s->api->remove(s->engine,id)) : ESP_ERR_INVALID_STATE;
}
esp_err_t solar_os_jet_render(solar_os_jet_t *s,solar_os_native_cancel_fn cancel,void *user) {
    if(!s || !s->engine || s->paused) return ESP_ERR_INVALID_STATE;
    collect(s);
    if(s->stats.error!=ESP_OK) return s->stats.error;
    int64_t start=esp_timer_get_time();
    esp_err_t err=result(s->api->render(s->engine,(uint16_t *)s->writing,s->bytes,cancel,user));
    if(err!=ESP_OK) return err;
    s->stats.render_us+=esp_timer_get_time()-start;
    err=solar_os_frame_presenter_exchange(s->presenter,&s->writing);
    if(err==ESP_OK) ++s->stats.frames;
    return err;
}
void solar_os_jet_status(solar_os_jet_t *s,solar_os_jet_status_t *stats) {
    if(!s || !stats) return;
    collect(s); *stats=s->stats;
}
esp_err_t solar_os_jet_execute(solar_os_jet_t *s,const solar_os_jet_command_t *command,
    solar_os_jet_reply_t *reply) {
    if(!s || !s->engine) return ESP_ERR_INVALID_STATE;
    if(!command || !reply) return ESP_ERR_INVALID_ARG;
    if(s->api->struct_size<sizeof(*s->api)) return ESP_ERR_NOT_SUPPORTED;
    const solar_os_jet_extensions_v1_t *ext=s->api->extensions;
    if(!ext || ext->abi_version!=1 || ext->struct_size<sizeof(*ext) || !ext->execute)
        return ESP_ERR_NOT_SUPPORTED;
    return result(ext->execute(s->engine,command,reply));
}
