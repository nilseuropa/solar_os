#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_module_packages.h"
#include "solar_os_keys.h"

typedef struct { int unused; } solar_os_tui_t;
typedef struct { size_t row, col, height, width; } solar_os_tui_rect_t;
typedef struct { size_t cursor, top; } solar_os_tui_viewport_t;
typedef struct { bool finished; } solar_os_context_t;
typedef struct { int type; struct { unsigned char ch; } data; } solar_os_event_t;
typedef void *TaskHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define SOLAR_OS_APP_STATIC_SRAM_EXCEPTION(s)
#define PKG_TUI_TASK_STACK 16384
#define PKG_TUI_TASK_PRIORITY 2
#define tskNO_AFFINITY 0
#define pdPASS 1
#define SOLAR_OS_TASK_ROLE_FOREGROUND 0
#define SOLAR_OS_TASK_STOP_WAIT_MS 100
#define SOLAR_OS_EVENT_TICK 1
#define SOLAR_OS_EVENT_CHAR 2
#define SOLAR_OS_TUI_ATTR_INVERSE 1
#define SOLAR_OS_TUI_ATTR_NORMAL 0

static solar_os_module_catalog_t saved;
static bool cache_available = true, fail_refresh, fail_worker, wait_ready = true;
static unsigned fetches, installs, removes, bars, loads, cols=80, rows=12;
static uint64_t progress_value, progress_total;
static char popup_title[80], popup_text[320], screen[32][161];
size_t strlcpy(char *d,const char *s,size_t n) { size_t len=strlen(s);if(n)snprintf(d,n,"%s",s);return len; }
const char *esp_err_to_name(esp_err_t e) { (void)e;return "error"; }
static size_t solar_os_tui_cols(solar_os_tui_t *t) { (void)t;return cols; }
static size_t solar_os_tui_screen_content_rows(solar_os_tui_t *t,size_t a,size_t b) { (void)t;return rows-a-b; }
static void solar_os_tui_clear(solar_os_tui_t *t) { (void)t;memset(screen,0,sizeof(screen));*popup_title=*popup_text=0; }
static void solar_os_tui_draw_title(solar_os_tui_t *t,const char *a,const char *b) { (void)t;(void)b;strlcpy(screen[0],a,cols+1); }
static void solar_os_tui_write_cell(solar_os_tui_t *t,size_t row,size_t col,size_t width,const char *text,unsigned attr)
{ (void)t;(void)attr;assert(row<rows && col+width<=cols);strlcpy(screen[row]+col,text,width+1); }
static void solar_os_tui_draw_footer(solar_os_tui_t *t,const char *a,const char *b) { (void)t;strlcpy(screen[rows-1],*a?a:b,cols+1); }
static void solar_os_tui_text_popup(solar_os_tui_t *t,const solar_os_tui_rect_t *b,const char *title,const char *text,solar_os_tui_rect_t *out)
{ (void)t;assert(b->row+b->height<=rows && b->width<=cols);strlcpy(popup_title,title,sizeof(popup_title));strlcpy(popup_text,text,sizeof(popup_text));if(out)*out=*b; }
static void solar_os_tui_progress_bar(solar_os_tui_t *t,size_t r,size_t c,size_t w,const char *label,uint64_t v,uint64_t total,bool known)
{ (void)t;(void)label;(void)known;assert(r<rows && c+w<=cols);bars++;progress_value=v;progress_total=total; }
static void solar_os_tui_set_cursor_visible(solar_os_tui_t *t,bool v) { (void)t;(void)v; }
static void solar_os_tui_refresh(solar_os_tui_t *t) { (void)t; }
static void solar_os_tui_end(solar_os_tui_t *t) { (void)t; }
static esp_err_t solar_os_tui_screen_begin(solar_os_tui_t *t,solar_os_context_t *c) { (void)t;(void)c;return ESP_OK; }
static int solar_os_task_create_pinned_internal(void (*fn)(void*),const char *name,unsigned bytes,void *arg,unsigned priority,void **task,int core,int role)
{ (void)fn;(void)name;(void)bytes;(void)arg;(void)priority;(void)core;(void)role;if(fail_worker)return 0;*task=(void*)1;return pdPASS; }
static void solar_os_task_delete_internal(void *p) { (void)p; }
static bool solar_os_task_wait_done(void *p,volatile bool *done,unsigned ms) { (void)p;(void)done;(void)ms;return wait_ready; }
static void solar_os_context_finish(solar_os_context_t *c,int code,void *p) { (void)code;(void)p;c->finished=true; }
static solar_os_module_catalog_t *copy_catalog(void) { solar_os_module_catalog_t *c=malloc(sizeof(*c));assert(c);*c=saved;return c; }
esp_err_t solar_os_module_catalog_load(solar_os_module_catalog_t **out,char *d,size_t n)
{ (void)d;(void)n;loads++;*out=cache_available?copy_catalog():NULL;return cache_available?ESP_OK:ESP_ERR_NOT_FOUND; }
esp_err_t solar_os_module_catalog_fetch_ex(solar_os_module_catalog_t **out,solar_os_module_package_cancel_fn cancel,void *u,char *d,size_t n)
{ fetches++;*out=NULL;if(fail_refresh || cancel(u)){strlcpy(d,"network unavailable",n);return ESP_FAIL;}*out=copy_catalog();return ESP_OK; }
void solar_os_module_catalog_free(solar_os_module_catalog_t *c) { free(c); }
const char *solar_os_module_type_name(solar_os_module_type_t t) { (void)t;return "module"; }
esp_err_t solar_os_module_package_install(const char *id,const solar_os_module_install_options_t *o,solar_os_module_install_result_t *r,char *d,size_t n)
{ (void)r;(void)d;(void)n;assert(!strcmp(id,"robot"));installs++;o->progress(50,100,o->user);return ESP_OK; }
esp_err_t solar_os_module_package_remove(const char *id,char *d,size_t n) { (void)d;(void)n;assert(!strcmp(id,"robot"));removes++;return ESP_OK; }

/* VIEWPORT */
/* TUI */

static solar_os_context_t ctx;
static void key(uint8_t k) { solar_os_event_t e={.type=SOLAR_OS_EVENT_CHAR,.data.ch=k};assert(pkg_tui_event(&ctx,&e)); }
static void done(void) { pkg_tui_operation_task(NULL);pkg_tui_finish_operation();assert(pkg_tui.result_popup && !pkg_tui.busy); }
static void find_id(const char *id) {
    pkg_tui.collapsed_folders=0;
    for(size_t i=0;i<pkg_tui_visible_count();i++) {
        pkg_tui_node_t node;assert(pkg_tui_node_at(i,&node));
        if(!node.is_folder && !strcmp(pkg_tui.catalog->packages[node.package].id,id)) { pkg_tui.viewport.cursor=i;return; }
    }
    assert(false);
}
int main(void) {
    saved.count=3;saved.signature_verified=true;
    saved.packages[0]=(solar_os_module_package_t){.type=SOLAR_OS_MODULE_TYPE_LIBRARY,.compatible=true};
    strcpy(saved.packages[0].id,"robot");strcpy(saved.packages[0].version,"1");
    saved.packages[1]=(solar_os_module_package_t){.type=SOLAR_OS_MODULE_TYPE_APP,.compatible=true};strcpy(saved.packages[1].id,"hello");
    saved.packages[2]=(solar_os_module_package_t){.type=SOLAR_OS_MODULE_TYPE_JOB,.compatible=true};strcpy(saved.packages[2].id,"worker");
    pkg_tui_state=calloc(1,sizeof(pkg_tui_state_t));assert(pkg_tui_state);
    assert(pkg_tui_start(&ctx)==ESP_OK && loads==1 && !fetches && !pkg_tui.busy);
    assert(strstr(screen[1],"Apps (1)"));
    assert(strstr(screen[5],"Drivers (0)"));
    assert(!pkg_tui_selected(NULL));key('I');assert(!installs && !pkg_tui.prompt_operation);
    key(SOLAR_OS_KEY_DOWN);assert(pkg_tui_selected(NULL));
    key(SOLAR_OS_KEY_LEFT);assert(!pkg_tui_selected(NULL));
    key(SOLAR_OS_KEY_LEFT);assert(pkg_tui_visible_count()==4);
    key(SOLAR_OS_KEY_RIGHT);assert(pkg_tui_visible_count()==5);
    find_id("robot");key('I');assert(!installs && strstr(popup_title,"Install"));
    key(SOLAR_OS_KEY_DOWN);assert(pkg_tui.prompt_operation==PKG_TUI_OPERATION_INSTALL);
    key('\r');assert(!pkg_tui.busy && !installs); /* Enter defaults to No. */
    key('I');key('y');assert(pkg_tui.busy && !installs && !strcmp(popup_title,"Working") && bars);
    pkg_tui_progress(25,100,NULL);pkg_tui_render();assert(progress_value==25 && progress_total==100);
    done();assert(installs==1 && !strcmp(popup_title,"Success") && pkg_tui_selected(NULL));
    key('\r');solar_os_module_package_t selected;assert(pkg_tui_selected(&selected) && selected.installed);
    key('U');assert(!removes && strstr(popup_title,"Remove"));key('n');assert(!removes);
    key('U');key('Y');done();assert(removes==1);key('\r');
    fail_refresh=true;solar_os_module_catalog_t *previous=pkg_tui.catalog;
    key('R');done();assert(pkg_tui.catalog==previous && !strcmp(popup_title,"Failed"));key('\r');
    key('R');key(SOLAR_OS_KEY_ESCAPE);assert(pkg_tui.stop_requested && strstr(popup_text,"Cancelling"));done();
    assert(!strcmp(popup_title,"Cancelled"));key('\r');
    fail_refresh=false;key('R');done();assert(pkg_tui.catalog!=previous && fetches==3);key('\r');
    for(unsigned width=20;width<=80;width+=20) {
        cols=width;rows=8;pkg_tui_render();find_id("robot");key('I');key(SOLAR_OS_KEY_ESCAPE);
    }
    cols=80;rows=12;
    saved.packages[3]=(solar_os_module_package_t){.type=SOLAR_OS_MODULE_TYPE_DRIVER};strcpy(saved.packages[3].id,"driver");saved.count=4;
    key('R');done();key('\r');pkg_tui.collapsed_folders=UINT32_MAX;assert(pkg_tui_visible_count()==4);
    find_id("robot");fail_worker=true;key('I');key('y');assert(!pkg_tui.busy && pkg_tui.result_popup && !strcmp(popup_title,"Failed"));
    key(SOLAR_OS_KEY_APP_EXIT);assert(ctx.finished);
    key('\r');fail_worker=false;key('R');wait_ready=false;
    previous=pkg_tui.catalog;pkg_tui_stop(&ctx);
    assert(!pkg_tui.tui_active && pkg_tui.catalog==previous && !pkg_tui_state_release_ready());
    pkg_tui_operation_task(NULL);assert(pkg_tui_state_release_ready());
    pkg_tui_cleanup();cache_available=false;fail_worker=false;
    assert(pkg_tui_start(&ctx)==ESP_OK && !pkg_tui.busy && !pkg_tui.catalog && fetches==5);
    assert(pkg_tui_visible_count()==4);pkg_tui_cleanup();free(pkg_tui_state);
    puts("pkg TUI: offline open, folders, navigation, confirmation, progress, cancellation and results passed");
}
