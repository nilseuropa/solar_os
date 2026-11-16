#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_keys.h"
#include "solar_os_zoo.h"

#define ZOO_RECORDS_MAX (SOLAR_OS_ZOO_MODELS_MAX * 2U)
#define ZOO_STACK 16384U
#define SOLAR_OS_APP_STATIC_SRAM_EXCEPTION(s)
#define SOLAR_OS_APP_CLASS_COMMAND 1
#define SOLAR_OS_EVENT_TICK 1
#define SOLAR_OS_EVENT_CHAR 2
#define SOLAR_OS_EVENT_KEY 3
#define SOLAR_OS_INPUT_KEY_RELEASE 0
#define SOLAR_OS_TASK_STOP_WAIT_MS 100
#define SOLAR_OS_TASK_ROLE_FOREGROUND 0
#define SOLAR_OS_TUI_ATTR_INVERSE 1
#define SOLAR_OS_TUI_SCREEN_KEY_NONE 0
#define SOLAR_OS_TUI_SCREEN_KEY_CONSUMED 1
#define SOLAR_OS_TUI_SCREEN_KEY_PASSTHROUGH 2
#define SOLAR_OS_TUI_SCREEN_KEY_TOGGLED 3
#define SOLAR_OS_TUI_INPUT_CANCEL 1
#define SOLAR_OS_TUI_INPUT_SUBMIT 2
#define SOLAR_OS_FILE_SHORTCUT_SEARCH 1
#define SOLAR_OS_FILE_SHORTCUT_FULLSCREEN 2
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY 0
#define pdPASS 1
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
typedef int portMUX_TYPE;
typedef void *TaskHandle_t;
typedef int solar_os_tui_input_action_t;
typedef int solar_os_tui_screen_key_action_t;
typedef int solar_os_file_shortcut_t;
typedef struct { bool screen_active, fullscreen, alt_prefix_pending; } solar_os_tui_t;
typedef struct { size_t cursor, top; } solar_os_tui_viewport_t;
typedef struct { size_t cursor, view; } solar_os_tui_input_state_t;
typedef struct { size_t row, col, height, width; } solar_os_tui_rect_t;
typedef struct { bool finished; int argc; const char **argv; } solar_os_context_t;
typedef struct { uint8_t key, action; } key_event_t;
typedef struct { int type; union { uint8_t ch; key_event_t key; } data; } solar_os_event_t;
typedef int solar_os_shell_io_t;
struct solar_os_zoo { int unused; } zoo;

static solar_os_zoo_model_t records[ZOO_RECORDS_MAX];
static bool installed[ZOO_RECORDS_MAX], catalog_locked, fail_worker, fail_refresh;
static size_t count, rows=12, cols=80;
static unsigned fetches, installs, removes, bars, reads, joins;
static char screen[32][161], popup_title[64], popup_text[320];
static uint64_t progress_value, progress_total;
size_t strlcpy(char *d,const char *s,size_t n) { size_t len=strlen(s);if(n)snprintf(d,n,"%s",s);return len; }
const char *esp_err_to_name(esp_err_t e) { return e ? "error" : "ESP_OK"; }
static size_t solar_os_tui_rows(const solar_os_tui_t *t) { (void)t;return rows; }
static size_t solar_os_tui_cols(const solar_os_tui_t *t) { (void)t;return cols; }
static esp_err_t tui_screen_set_fullscreen(solar_os_tui_t *t,bool v) { t->fullscreen=v;return ESP_OK; }
static void solar_os_tui_clear(solar_os_tui_t *t) { (void)t;memset(screen,0,sizeof(screen));*popup_title=*popup_text=0; }
static void solar_os_tui_draw_title(solar_os_tui_t *t,const char *a,const char *b) { (void)t;(void)b;strlcpy(screen[0],a,cols+1); }
static void solar_os_tui_write_cell(solar_os_tui_t *t,size_t r,size_t c,size_t w,const char *s,unsigned a)
{ (void)t;(void)a;assert(r<rows&&c+w<=cols);strlcpy(screen[r]+c,s,w+1); }
static void solar_os_tui_draw_footer(solar_os_tui_t *t,const char *a,const char *b)
{ if(!t->fullscreen)strlcpy(screen[rows-1],*a?a:b,cols+1); }
static void solar_os_tui_text_popup(solar_os_tui_t *t,const solar_os_tui_rect_t *b,const char *title,const char *text,solar_os_tui_rect_t *out)
{ (void)t;assert(b->row+b->height<=rows&&b->col+b->width<=cols);strlcpy(popup_title,title,sizeof(popup_title));strlcpy(popup_text,text,sizeof(popup_text));if(out)*out=*b; }
static void solar_os_tui_progress_bar(solar_os_tui_t *t,size_t r,size_t c,size_t w,const char *s,uint64_t v,uint64_t total,bool known)
{ (void)t;(void)s;(void)known;assert(r<rows&&c+w<=cols);bars++;progress_value=v;progress_total=total; }
static void solar_os_tui_set_cursor_visible(solar_os_tui_t *t,bool v) { (void)t;(void)v; }
static void solar_os_tui_refresh(solar_os_tui_t *t) { (void)t; }
static void solar_os_tui_end(solar_os_tui_t *t) { t->screen_active=false; }
static void solar_os_tui_draw_too_small(solar_os_tui_t *t,const char *s) { (void)t;(void)s; }
static esp_err_t solar_os_tui_screen_begin(solar_os_tui_t *t,solar_os_context_t *c) { (void)c;t->screen_active=true;return ESP_OK; }
static solar_os_tui_input_action_t solar_os_tui_input_key(char *s,size_t capacity,solar_os_tui_input_state_t *i,uint32_t k,size_t w)
{ (void)i;(void)w;size_t n=strlen(s);if(k==SOLAR_OS_KEY_ESCAPE)return SOLAR_OS_TUI_INPUT_CANCEL;if(k==SOLAR_OS_KEY_ENTER)return SOLAR_OS_TUI_INPUT_SUBMIT;if(k>=32&&k<127&&n+1<capacity){s[n]=k;s[n+1]=0;}return 0; }
static void solar_os_tui_draw_input(solar_os_tui_t *t,size_t r,size_t c,size_t w,const char *label,const char *s,solar_os_tui_input_state_t *i,unsigned attr)
{ (void)i;char text[100];snprintf(text,sizeof(text),"%s%s",label,s);solar_os_tui_write_cell(t,r,c,w,text,attr); }
static solar_os_file_shortcut_t solar_os_file_shortcut_from_key_event(const key_event_t *e) { (void)e;return 0; }
static int solar_os_task_create_pinned_internal(void (*f)(void*),const char *n,unsigned bytes,void *arg,unsigned p,void **task,int c,int r)
{ (void)f;(void)n;(void)bytes;(void)arg;(void)p;(void)c;(void)r;if(fail_worker)return 0;*task=(void*)1;catalog_locked=true;return pdPASS; }
static void solar_os_task_delete_internal(void *p) { (void)p; }
static bool solar_os_task_wait_done(void *p,volatile bool *done,unsigned ms) { (void)p;(void)ms;joins++;return *done; }
static void solar_os_context_finish(solar_os_context_t *c,int code,const char *s) { (void)code;(void)s;c->finished=true; }
static int solar_os_context_argc(solar_os_context_t *c) { return c->argc; }
static const char *solar_os_context_argv(solar_os_context_t *c,int i) { return c->argv[i]; }
static void solar_os_context_set_app_class(solar_os_context_t *c,int v) { (void)c;(void)v; }
static solar_os_shell_io_t *solar_os_context_shell_io(solar_os_context_t *c) { (void)c;return NULL; }
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io,const char *s) { (void)io;(void)s; }
static void solar_os_shell_io_printf(solar_os_shell_io_t *io,const char *s,...) { (void)io;(void)s; }
esp_err_t solar_os_zoo_open(solar_os_zoo_t **out) { *out=&zoo;return ESP_OK; }
void solar_os_zoo_close(solar_os_zoo_t *z) { (void)z;assert(!catalog_locked); }
esp_err_t solar_os_zoo_reload(solar_os_zoo_t *z) { (void)z;return ESP_OK; }
size_t solar_os_zoo_count(const solar_os_zoo_t *z) { (void)z;assert(!catalog_locked);return count; }
bool solar_os_zoo_get(const solar_os_zoo_t *z,size_t n,solar_os_zoo_model_t *m)
{ (void)z;assert(!catalog_locked);reads++;if(n>=count)return false;*m=records[n];return true; }
bool solar_os_zoo_installed(const solar_os_zoo_t *z,size_t n) { (void)z;assert(!catalog_locked&&n<count);return installed[n]; }
const char *solar_os_zoo_storage(void) { return "sd"; }
void solar_os_zoo_get_source(char *s,size_t n) { strlcpy(s,"https://example.test/catalog.json",n); }
esp_err_t solar_os_zoo_set_source(const char *s) { (void)s;return ESP_OK; }
esp_err_t solar_os_zoo_set_storage(const char *s) { (void)s;return ESP_OK; }
esp_err_t solar_os_zoo_bundle_path(const solar_os_zoo_t *z,size_t i,char *s,size_t n) { (void)z;(void)i;strlcpy(s,"/sdcard/bundle.json",n);return ESP_OK; }
esp_err_t solar_os_zoo_refresh(solar_os_zoo_t *z,const volatile bool *cancel,solar_os_zoo_progress_fn p,void *u)
{ (void)z;(void)p;(void)u;fetches++;return fail_refresh||*cancel?ESP_FAIL:ESP_OK; }
esp_err_t solar_os_zoo_install(solar_os_zoo_t *z,size_t i,const volatile bool *cancel,solar_os_zoo_progress_fn p,void *u)
{ (void)z;(void)p;(void)u;installs++;if(*cancel)return ESP_FAIL;installed[i]=true;return ESP_OK; }
esp_err_t solar_os_zoo_remove(solar_os_zoo_t *z,size_t i,const volatile bool *cancel,solar_os_zoo_progress_fn p,void *u)
{ (void)z;(void)p;(void)u;removes++;if(*cancel)return ESP_FAIL;installed[i]=false;return ESP_OK; }

/* WIDGETS */
/* APP */

static solar_os_context_t ctx={.argc=1};
static void key(uint8_t k) { solar_os_event_t e={.type=SOLAR_OS_EVENT_CHAR,.data.ch=k};assert(event(&ctx,&e)); }
static void tick(void) { solar_os_event_t e={.type=SOLAR_OS_EVENT_TICK};assert(event(&ctx,&e)); }
static void complete(void) { worker(&app);catalog_locked=false;tick();assert(!app.busy&&app.result_popup); }
static void select_model(size_t model) {
    for(size_t f=0;f<app.folder_count;f++)app.folders[f].collapsed=false;
    tree_nodes();for(size_t n=0;n<app.node_count;n++)if(app.nodes[n].model==model){app.view.cursor=n;return;}assert(false);
}
int main(void) {
    zoo_state=calloc(1,sizeof(zoo_app_t));assert(zoo_state);count=3;
    for(size_t i=0;i<count;i++) {
        snprintf(records[i].id,sizeof(records[i].id),"model-%zu",i);
        snprintf(records[i].name,sizeof(records[i].name),"Model %zu",i);
        strcpy(records[i].version,"1.0.0");strcpy(records[i].summary,"A long model description for scrolling and clipping checks");
        strcpy(records[i].target,i==2?"hx6538":"esp32s3");strcpy(records[i].backend,i==2?"himax":"espdl");
        records[i].downloadable=true;records[i].compatible=i!=2;
    }
    installed[0]=true;assert(!start(&ctx));assert(!fetches&&app.folder_count==2&&app.node_count==4);
    assert(strstr(screen[1],"- esp32s3")&&strstr(screen[2],"[I]"));
    key(SOLAR_OS_KEY_ENTER);assert(app.node_count==2);key(SOLAR_OS_KEY_RIGHT);assert(app.node_count==4);
    key(SOLAR_OS_KEY_DOWN);key(SOLAR_OS_KEY_ENTER);assert(app.details);key(SOLAR_OS_KEY_ESCAPE);assert(!app.details);
    key(SOLAR_OS_KEY_LEFT);assert(app.view.cursor==0);key(SOLAR_OS_KEY_LEFT);assert(app.node_count==2);
    key(SOLAR_OS_KEY_DOWN);key(SOLAR_OS_KEY_RIGHT);key(SOLAR_OS_KEY_DOWN);key('i');
    assert(app.confirming&&app.prompt_operation==ZOO_INSTALL&&!strcmp(popup_title,"Install bundle"));
    key(SOLAR_OS_KEY_ENTER);assert(!app.confirming&&!installs);
    key('I');key('y');assert(app.busy&&!ready());unsigned before=reads;render();assert(reads==before);
    solar_os_zoo_progress_t p={.bytes=50,.total=100};strcpy(p.stage,"download");progress(&p,&app);tick();
    assert(bars&&progress_value==50&&progress_total==100&&reads==before);complete();
    assert(installs==1&&installed[2]&&!strcmp(popup_title,"Success"));key(SOLAR_OS_KEY_ESCAPE);
    key('u');assert(app.confirming&&!strcmp(popup_title,"Remove bundle"));key('n');assert(!removes);
    key('u');key('y');complete();assert(removes==1&&!installed[2]);key(SOLAR_OS_KEY_ENTER);
    key('t');assert(app.installed_only&&app.folder_count==1&&app.node_count==2);key('t');
    key('/');key('2');assert(app.folder_count==1&&app.node_count==2);key(SOLAR_OS_KEY_ENTER);assert(!app.searching);
    key('/');key(SOLAR_OS_KEY_ESCAPE);assert(!*app.query&&app.folder_count==2);
    fail_refresh=true;key('r');assert(app.busy);complete();assert(fetches==1&&app.node_count==4&&!strcmp(popup_title,"Failed"));key('\r');
    fail_refresh=false;key('r');key(SOLAR_OS_KEY_ESCAPE);assert(app.cancel&&app.busy);stop(&ctx);assert(joins&&!ready());complete();
    assert(!strcmp(popup_title,"Cancelled")&&ready());key('\r');
    fail_worker=true;key('r');assert(!app.busy&&app.result_popup&&!strcmp(popup_title,"Failed"));key('\r');fail_worker=false;
    select_model(2);records[2].downloadable=false;strcpy(records[2].reason,"unsupported target");key('i');
    assert(app.result_popup&&!app.confirming&&strstr(popup_text,"unsupported target"));key('\r');
    for(size_t i=0;i<ZOO_RECORDS_MAX;i++) {
        snprintf(records[i].target,sizeof(records[i].target),"target-%zu",i);strcpy(records[i].backend,"espdl");
    }
    count=ZOO_RECORDS_MAX;tree_rebuild();for(size_t f=0;f<app.folder_count;f++)app.folders[f].collapsed=false;
    tree_nodes();assert(app.folder_count==ZOO_RECORDS_MAX&&app.node_count==2*ZOO_RECORDS_MAX);
    rows=6;cols=24;app.view.cursor=app.node_count-1;render();assert(*screen[rows-2]);
    key(SOLAR_OS_KEY_ALT_PREFIX);key(SOLAR_OS_KEY_ENTER);assert(app.tui.fullscreen&&*screen[rows-1]);
    key('/');assert(strstr(screen[rows-1],"Search:"));key(SOLAR_OS_KEY_ESCAPE);
    rows=12;cols=80;key(SOLAR_OS_KEY_ESCAPE);assert(ctx.finished);cleanup();free(zoo_state);
    puts("Zoo shared tree, modal workflow, offline filtering, geometry and worker ownership passed");
    return 0;
}
