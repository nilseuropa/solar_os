#define main emotion_runtime_fixture_main
#include "emotion_runtime_test.c"
#undef main
#include <math.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#define SOLAR_OS_PACKAGE_SERVICE_EMOTION 1
#define SOLUA_EVENT_EMOTION_MODE 1
typedef struct { int type, attr; } solua_event_t;
static struct {
    solar_os_emotion_t *emotions[4];
    uint32_t emotion_generation[4], emotion_count;
    bool emotion_busy, emotion_ack, emotion_suspended;
} state, *solua_runtime_state = &state;
static struct {
    bool stop_requested, tui_active;
    void *events;
    solar_os_gfx_t *session_gfx, *claimed_gfx;
} solua;
static const char *esp_err_to_name(int err) {(void)err;return "test error";}
static int solua_check_esp(lua_State *L, int err)
{return err?luaL_error(L,"service error %d",err):0;}
static void solua_set_func(lua_State *L,int index,const char *name,lua_CFunction fn)
{index=lua_absindex(L,index);lua_pushcfunction(L,fn);lua_setfield(L,index,name);}
static void solua_set_int(lua_State *L,int index,const char *name,lua_Integer value)
{index=lua_absindex(L,index);lua_pushinteger(L,value);lua_setfield(L,index,name);}
static bool solua_send_event(const solua_event_t *event)
{assert(event->type==SOLUA_EVENT_EMOTION_MODE);state.emotion_ack=true;return true;}
#include "solar_os_lua_emotion.inc"
typedef struct {int unused;} solar_os_context_t;
#define SOLAR_OS_LOGW(...) ((void)0)
static void solua_drain_events(solar_os_context_t *ctx)
{(void)ctx;state.emotion_busy=false;state.emotion_ack=true;}
#include "emotion_visibility.inc"
static void run(lua_State *L,const char *source)
{if(luaL_dostring(L,source)){fprintf(stderr,"%s\n",lua_tostring(L,-1));abort();}}
int main(void)
{
    terminal_foreground=0xff0000;terminal_background=0x0000ff;
    lua_State *L=luaL_newstate();assert(L);luaL_openlibs(L);
    lua_newtable(L);solua_set_func(L,-1,"open",solua_emotion_open);lua_setglobal(L,"emotion");
    run(L,"f=emotion.open('oled0',false,0x00ffff,0)");
    assert(state.emotions[0]->palette[0]==0 && state.emotions[0]->palette[3]==0x07ff);
    run(L,"f:close(); f=emotion.open('oled0',false,nil,0)");
    assert(state.emotions[0]->palette[0]==0 && state.emotions[0]->palette[3]==0xf800);
    run(L,"f:close()");
    run(L,
        "assert(not pcall(emotion.open)); "
        "assert(not pcall(emotion.open, 'missing')); "
        "assert(not pcall(emotion.open, 'oled0', false, 'invalid')); "
        "assert(not pcall(emotion.open, 'oled0', false, -1)); "
        "assert(not pcall(emotion.open, 'oled0', false, 0x1000000)); "
        "assert(not pcall(emotion.open, 'oled0', false, nil, -1)); "
        "assert(not pcall(emotion.open, 'oled0', false, nil, 0x1000000)); "
        "assert(not pcall(emotion.open, 'oled0', false, nil, nil, 65536)); "
        "f=emotion.open('oled0', true, nil, nil, 25, true); "
        "assert(f:status().fps==25 and f:status().motion); "
        "assert(not pcall(emotion.open,'oled0')); "
        "assert(not pcall(f.set,f,'missing')); "
        "assert(not pcall(f.set,f,'happy',-1)); "
        "assert(not pcall(f.look,f,0/0,0)); "
        "assert(not pcall(f.look,f,2,0)); "
        "assert(not pcall(f.react,f,'happy',100,100,'unknown')); "
        "f:set('happy'); f:look(.5,-.5); f:blink(); f:react('surprised',700,120,'chirp'); "
        "f:close(); f:close(); assert(not pcall(f.blink,f)); "
        "g=emotion.open('oled0'); f:close(); assert(g:status().width==128); g:close(); "
        "assert(not pcall(function() local x <close> = emotion.open('oled0'); error('user error') end)); "
        "g=emotion.open('oled0')");
    assert(state.emotions[0]->palette[0]==0x001f && state.emotions[0]->palette[3]==0xf800);
    assert(solua_emotion_drawing()&&!solua_emotion_active());
    solua_emotion_destroy();
    assert(!solua_emotion_drawing()&&!claims&&!leases&&!allocations);
    run(L,"assert(not pcall(g.status,g))");
    /* Dedicated targets keep running when their script session is hidden. */
    run(L,"g=emotion.open('oled0')");
    solua_emotion_visibility(NULL,false);
    assert(claims && !state.emotions[0]->paused);
    solua_emotion_visibility(NULL,true);
    run(L,"g:close()");
    /* Exercise the foreground mode handshake and close without a target. */
    solua.session_gfx=&gfx;solua.events=(void *)1;
    run(L,"g=emotion.open()");
    state.emotion_busy=true; /* callback drains an open's pending acknowledgement */
    solua_emotion_visibility(NULL,false);
    assert(!claims && state.emotions[0]->paused && state.emotion_suspended);
    run(L,"g:set('happy'); assert(not pcall(emotion.open))");
    solua_emotion_visibility(NULL,true);
    assert(claims && !state.emotions[0]->paused && !state.emotion_suspended);
    run(L,"g:close()");
    run(L,"local f <close> = emotion.open(); f:set('happy')");
    assert(!claims&&!leases&&!allocations&&!state.emotion_count);
    lua_close(L);
    puts("emotion Lua bindings passed");
}
