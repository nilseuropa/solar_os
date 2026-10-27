#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "solar_os_jet.h"
#include "solar_os_display.h"
#include "jet_fixture.h"
#define SOLAR_OS_PACKAGE_SERVICE_JET 1
#define SOLUA_EVENT_JET_MODE 1
#define pdMS_TO_TICKS(value) (value)
#define SOLAR_OS_LOGW(...) ((void)0)
typedef struct { int type,attr; } solua_event_t;
typedef struct { int unused; } solar_os_context_t;
static struct {
    solar_os_jet_t *jet;
    uint32_t jet_generation,jet_count;
    bool jet_busy,jet_ack,jet_suspended;
} state,*solua_runtime_state=&state;
static struct { bool stop_requested,tui_active; void *events; solar_os_gfx_t *session_gfx,*claimed_gfx; } solua;
static bool mode;
static unsigned visibility_race;
static bool solua_should_cancel(void *user) {(void)user;return solua.stop_requested;}
static bool solua_emotion_drawing(void) {return false;}
static void vTaskDelay(unsigned delay) {
    if(visibility_race==1 && delay==1) {
        assert(solar_os_jet_suspend(state.jet)==ESP_OK);
        state.jet_suspended=true;state.jet_busy=false;visibility_race=2;
    } else if(visibility_race==2 && delay==10) {
        assert(solar_os_jet_resume(state.jet)==ESP_OK);
        state.jet_suspended=false;visibility_race=0;
    }
}
static void solua_check_esp(lua_State *L,int err) {if(err)luaL_error(L,"service error %d",err);}
static void solua_set_func(lua_State *L,int table,const char *name,lua_CFunction f) {
    table=lua_absindex(L,table);lua_pushcfunction(L,f);lua_setfield(L,table,name);
}
static void solua_set_int(lua_State *L,int table,const char *name,lua_Integer n) {
    table=lua_absindex(L,table);lua_pushinteger(L,n);lua_setfield(L,table,name);
}
static bool solua_send_event(const solua_event_t *event) {
    assert(event->type==SOLUA_EVENT_JET_MODE);mode=event->attr;state.jet_ack=true;return true;
}
#include "solar_os_lua_jet.inc"
static void solua_drain_events(solar_os_context_t *ctx) {(void)ctx;state.jet_busy=false;state.jet_ack=true;}
#include "jet_visibility.inc"
static void run(lua_State *L,const char *source) {
    if(luaL_dostring(L,source)) {fprintf(stderr,"%s\n",lua_tostring(L,-1));abort();}
}
static int should_exit(lua_State *L) {lua_pushboolean(L,false);return 1;}
static int sleep_ms(lua_State *L) {assert(luaL_checkinteger(L,1)>=0);return 0;}
static bool fail_lua_alloc;
static void *fixture_allocate(void *user,void *ptr,size_t old,size_t bytes) {
    (void)user;if(!bytes) {free(ptr);return NULL;}
    if(fail_lua_alloc&&bytes>old)return NULL;
    return realloc(ptr,bytes);
}
static int arm_alloc_failure(lua_State *L) {(void)L;fail_lua_alloc=true;return 0;}
int main(int argc,char **argv) {
    for(unsigned mono=0;mono<2;++mono) {
        jet_test_reset(mono);memset(&state,0,sizeof(state));memset(&solua,0,sizeof(solua));
        lua_State *L=lua_newstate(fixture_allocate,NULL);assert(L);luaL_openlibs(L);
        lua_pushcfunction(L,arm_alloc_failure);lua_setglobal(L,"arm_alloc_failure");
        lua_newtable(L);solua_set_func(L,-1,"open",solua_jet_open);lua_setglobal(L,"jet");
        run(L,"assert(not pcall(jet.open)); assert(not pcall(jet.open,'missing')); "
            "assert(not pcall(jet.open,'display0',65536)); assert(not pcall(jet.open,'display0',-1)); "
            "assert(not pcall(jet.open,'display0\\0ignored')); "
            "s=jet.open('display0'); assert(not pcall(jet.open,'display0')); "
            "assert(not pcall(s.cube,s,65536,10,10)); assert(not pcall(s.cube,s,10,10,10,-1)); "
            "assert(not pcall(s.camera,s,0,0,-500,0,0,0,1)); "
            "assert(not pcall(s.light,s,0,0,65536)); "
            "s:background(0xffffff); s:camera(); s:light(); "
            "cube=s:cube(150,150,150,0x2192ff); s:transform(cube,-55,0,100,15,30,15); s:render(); "
            "assert(s:status().frames==1 and s:status().presented==1); "
            "assert(not pcall(s.mesh,s,{},{})); "
            "assert(not pcall(s.mesh,s,{{0,0,0},{1,1,1},{2,2,2}},{{0,1,2}})); "
            "assert(not pcall(s.mesh,s,{{0,nil,0},{1,1,1},{2,2,2}},{{1,2,3}})); "
            "m=s:mesh({{-50,-50,0},{50,-50,0},{0,50,0}},{{1,2,3}},0xff0000); "
            "s:remove(m); assert(not pcall(s.transform,s,m)); "
            "s:close();s:close();assert(not pcall(s.render,s)); "
            "g=jet.open('display0');s:close();assert(g:status().width==64);g:close(); "
            "assert(not pcall(function() local a <close> = jet.open('display0');error('script error') end))");
        jet_test_assert_clean();
        run(L,"s=jet.open('display0');assert(s:capabilities().textures and s:capabilities().materials); "
            "t=s:texture(2,2,string.rep(string.char(255,0,0),4),{format='rgb888',name='red'}); "
            "assert(s:texture_info(t).width==2 and s:texture_info(t).format=='rgb888'); "
            "m=s:material{texture=t,shading='unlit',name='surface'}; "
            "assert(s:material_info(m).texture==t and s:material_info(m).shading=='unlit'); "
            "c=s:cube(150,150,150);s:assign(c,m);s:render(); "
            "assert(not pcall(s.remove,s,t));assert(not pcall(s.remove,s,m)); "
            "assert(not pcall(s.material_set,s,m,{shading='typo'})); "
            "assert(not pcall(s.texture_set,s,t,{unknown=true})); "
            "assert(not pcall(s.texture,s,1,1,'\\0',{format='index8',palette={}})); "
            "s:material_set(m,{alpha=180,specular=32});assert(s:material_info(m).alpha==180); "
            "s:texture_set(t,{pixels=string.rep(string.char(0,255,0),4)});s:render(); "
            "i=s:texture(2,2,string.char(0,1,1,0),{format='index8',palette={0xffffff,0x000000}}); "
            "s:texture_set(i,{offset=1,palette={0xff0000,0x00ff00}}); "
            "mesh=s:mesh_data({{-80,-80,0,0,0,0,0,-1},{0,80,0,0.5,1,0,0,-1},{80,-80,0,1,0,0,0,-1}},{{1,2,3,m}}); "
            "s:render();s:remove(mesh);s:assign(c,0);s:remove(m);s:remove(t);s:remove(i);s:remove(c);s:close()");
        jet_test_assert_clean();
        run(L,"s=jet.open('display0');assert(s:capabilities().sprites and s:capabilities().picking); "
            "c=s:cube(150,150,150);hud=s:sprite{x=25,y=20,width=16,height=12,color=0xff0000,name='HUD'}; "
            "assert(s:sprite_info(hud).name=='HUD');s:sprite_set(hud,{alpha=180,scale=2,blend='add'}); "
            "s:pick{{32,24},{0,0},{-1,-1}};assert(#s:picks()==0);s:render();local p=s:picks(); "
            "assert(#p==3 and p[1].hit and p[1].object==c and p[1].triangle>0); "
            "assert(not p[2].hit and p[2].triangle==0 and p[2].x==0 and p[2].y==0); "
            "assert(not p[3].hit);assert(not pcall(s.pick,s,{{1000,0}})); "
            "s:remove(c);local newer=s:cube(150,150,150);assert(s:picks()[1].object==c);s:remove(newer); "
            "t=s:texture(1,1,string.char(255,0,0),{format='rgb888'});m=s:material{texture=t}; "
            "s:sprite_set(hud,{material=m,flip_x=true,mirror_y=true,scale=3});s:render(); "
            "assert(not pcall(s.remove,s,m));assert(not pcall(s.remove,s,t)); "
            "s:remove(hud);s:remove(m);s:remove(t);s:pick(nil);s:render();assert(#s:picks()==0);s:close()");
        jet_test_assert_clean();
        solua.session_gfx=jet_test_gfx();solua.events=(void *)1;
        run(L,"s=jet.open('display0');assert(s:capabilities().tiled_textures); "
            "t=s:texture(2,2,string.char(0,1,2,3),{format='index8',palette={0xff0000,0x00ff00,0x0000ff,0xffffff},tiled=true,address='clamp'}); "
            "assert(s:texture_info(t).tile_bytes>0 and s:sample_texture(t,0,0)==0xff0000); "
            "local nearest=s:sample_texture(t,.5,.5);s:texture_set(t,{filter='bilinear'});local bilinear=s:sample_texture(t,.5,.5); "
            "assert(nearest~=bilinear);s:texture_set(t,{filter='three_point'});assert(s:sample_texture(t,.5,.5)~=bilinear); "
            "s:texture_set(t,{filter='cached_bilinear'});assert(s:sample_texture(t,.5,.5)==bilinear and s:texture_info(t).filter_bytes==2097152); "
            "s:texture_set(t,{offset=1});assert(s:sample_texture(t,0,0)==0x00ff00); "
            "s:texture_set(t,{filter='nearest',mip_level=1});assert(s:texture_info(t).sampler_width==1 and s:sample_texture(t,0,0)==0xff0000); "
            "s:texture_set(t,{mip_level=0,address='zero'});assert(s:sample_texture(t,-1,-1)==0); "
            "m=s:material{texture=t,shading='unlit'};c=s:cube(150,150,150);s:assign(c,m);s:render(); "
            "s:texture_set(t,{tiled=false});assert(s:texture_info(t).tile_bytes==0);s:render();s:close()");
        jet_test_assert_clean();
        run(L,"s=jet.open('display0');assert(s:capabilities().postfx); "
            "assert(s:postfx_info().scratch_bytes==0);c=s:cube(150,150,150); "
            "s:postfx{cel_bits=4};assert(s:postfx_info().scratch_bytes==0);s:render(); "
            "s:postfx{fxaa=true,bloom=true,crt=48,motion_blur=50,chromatic=2,pixelate=4}; "
            "s:render();s:render();assert(s:postfx_info().scratch_bytes>0 and s:postfx_info().history_valid); "
            "s:postfx_reset();assert(not s:postfx_info().history_valid);s:postfx_clear(); "
            "assert(s:postfx_info().scratch_bytes==0 and s:postfx_info().cel_bits==0);s:render();s:close()");
        jet_test_assert_clean();
        run(L,"s=jet.open('display0');assert(s:capabilities().particles and s:capabilities().flares); "
            "pool=s:particles{seed=123,additive=true,name='sparks'};assert(s:particles_info(pool).capacity==200); "
            "assert(s:particles_emit(pool,{count=12,speed=200})==12);assert(s:particles_info(pool).active==12); "
            "s:particles_set(pool,{enabled=false});s:step(.1);assert(s:particles_info(pool).active==12); "
            "s:particles_set(pool,{enabled=true});s:particles_emit(pool,{kind='splash',count=8}); "
            "s:step(.05);s:render();assert(s:particles_info(pool).active>0); "
            "s:particles_clear(pool);assert(s:particles_info(pool).active==0); "
            "mat=s:material{color=0xffffff,shading='unlit'};flare=s:flare({{material=mat,width=8,height=8,blend='replace'}},{z=1}); "
            "s:pick{{32,24}};s:step(.25);s:render();local info=s:flare_info(flare); "
            "assert(info.elements==1 and info.in_front and info.on_screen and info.visibility>0); "
            "assert(#s:picks()==1 and not s:picks()[1].hit);assert(not pcall(s.remove,s,mat)); "
            "s:flare_element_set(flare,1,{alpha=.5,axis=.7,mirror_x=true,z_order=-20});assert(s:flare_element_info(flare,1).alpha==.5); "
            "assert(s:flare_element_info(flare,1).z_order==-20); "
            "s:flare_set(flare,{x=.1,enabled=false});s:render();s:remove(flare);s:remove(mat);s:remove(pool); "
            "assert(not pcall(s.step,s,2));s:close()");
        jet_test_assert_clean();
        run(L,"s=jet.open('display0');assert(s:capabilities().instancing); "
            "src=s:primitive{kind='debug_cube',width=120,height=120,depth=120};proto=s:prototype(src,{name='shared'}); "
            "assert(s:prototype_info(proto).triangles==12);s:remove(src); "
            "batch=s:instance(proto,{x=-50});assert(s:instance_add(batch,proto,{x=50})==2); "
            "assert(s:object_info(batch).instances==2 and s:object_info(batch).triangles==24); "
            "s:instance_set(batch,2,{ry=30});local info=s:instance_info(batch,2); "
            "assert(info.x==50 and info.ry==30 and info.prototype==proto and #info.basis==9 and not info.baked); "
            "s:pick{{32,24}};s:render();local p=s:picks()[1]; "
            "assert(p.hit and p.object==batch and p.mesh==proto and p.batch==batch and p.instance>0); "
            "assert(not pcall(s.prototype,s,batch));assert(not pcall(s.remove,s,proto)); "
            "s:object_action(batch,{action='bake_scale',numerator=2,denominator=1}); "
            "assert(s:instance_info(batch,2).baked and s:instance_info(batch,2).x==100); "
            "s:instance_set(batch,2,{x=120});assert(s:instance_info(batch,2).baked); "
            "s:object_set(batch,{ry=30});s:object_action(batch,{action='bake_rotation'}); "
            "local before=s:instance_info(batch,2).basis;red=s:material{color=0xff0000}; "
            "s:instance_set(batch,2,{material=red});local after=s:instance_info(batch,2).basis; "
            "for i=1,9 do assert(before[i]==after[i]) end;assert(not pcall(s.remove,s,red)); "
            "s:assign(batch,0);s:remove(red);s:render(); "
            "local head=s:cube(300,300,300);s:object_set(batch,{enabled=false});s:lod(head,{batch});s:settings{lod_scale=100}; "
            "s:render();p=s:picks()[1];assert(p.hit and p.object==head and p.batch==batch and p.mesh==proto); "
            "s:remove(head);s:object_set(batch,{enabled=true});s:settings{lod_scale=0}; "
            "local copy=s:object_action(batch,{action='clone'});s:remove(batch);assert(not pcall(s.remove,s,proto)); "
            "s:instance_remove(copy,1);assert(s:object_info(copy).instances==1);s:instance_clear(copy); "
            "s:remove(proto);s:remove(copy); "
            "src=s:primitive{kind='grid',rows=6,cols=6};proto=s:prototype(src);s:remove(src); "
            "red=s:material{color=0xff0000};local materials={};for i=1,50 do materials[i]=red end; "
            "batch=s:instance(proto,{materials=materials});info=s:instance_info(batch,1); "
            "assert(#info.materials==50);for i=1,50 do assert(info.materials[i]==red) end; "
            "assert(not pcall(s.remove,s,red));s:instance_set(batch,1,{materials={}});s:remove(red); "
            "s:remove(batch);s:remove(proto); "
            "red=s:material{color=0xff0000};src=s:cube(100,100,100);s:assign(src,red); "
            "proto=s:prototype(src);s:remove(src);assert(not pcall(s.remove,s,red));s:remove(proto);s:remove(red);s:close()");
        jet_test_assert_clean();
        run(L,"s=jet.open('display0');assert(s:capabilities().assets); "
            "local bmp=string.pack('<c2I4I2I2I4I4I4I4I2I2I4I4I4I4I4I4','BM',70,0,0,54,40,2,2,1,24,0,16,0,0,0,0).. "
            "string.char(255,0,0,255,255,255,0,0,0,0,255,0,255,0,0,0); "
            "t=s:load_bmp(bmp,{name='checker.bmp'});assert(s:texture_info(t).width==2); "
            "local materials=s:load_mtl('newmtl textured\\nKd 1 1 1\\nmap_Kd checker.bmp\\n');assert(#materials==1); "
            "local obj='v -1 -1 0\\nv 0 1 0\\nv 1 -1 0\\nvt 0 0\\nvt .5 1\\nvt 1 0\\nvn 0 0 -1\\nusemtl textured\\nf -3/1/1 -2/2/1 -1/3/1\\n'; "
            "c=s:load_obj(obj,{scale=.5,name='imported'});assert(s:object_info(c).triangles==1);s:render(); "
            "assert(not pcall(s.load_obj,s,'v nan 0 0\\n'));assert(not pcall(s.load_bmp,s,bmp:sub(1,69))); "
            "s:remove(c);s:remove(materials[1]);s:remove(t);s:close()");
        jet_test_assert_clean();
        run(L,"s=jet.open('display0');assert(s:capabilities().geometry and s:capabilities().scene); "
            "for _,kind in ipairs{'cube','grid','plane','pyramid','sphere','capsule','cylinder','billboard','quad','debug_cube'} do "
            "local p=s:primitive{kind=kind};local info=s:object_info(p);assert(info.vertices>0 and info.triangles>0); "
            "s:object_set(p,{x=10,scale_x=0.5,culling='none',blend='average',depth_bias=2,name='shape'}); "
            "assert(s:object_info(p).x==10 and s:object_info(p).scale_x==0.5); "
            "s:object_action(p,{action='bake_rotation'});s:object_action(p,{action='bake_scale',numerator=1,denominator=2}); "
            "s:object_action(p,{action='cache_positions'});local copy=s:object_action(p,{action='clone'}); "
            "s:remove(p);s:render();s:remove(copy);end; "
            "a=s:primitive{kind='sphere',segments=16};b=s:primitive{kind='sphere',segments=6}; "
            "s:object_set(b,{enabled=false});s:lod(a,{b});assert(s:object_info(a).lod_levels==1); "
            "assert(not pcall(s.lod,s,b,{a}));assert(not pcall(s.remove,s,b));s:lod(a,{});s:remove(b); "
            "s:camera_set{z=-450,near=10,far=3000};s:camera_action{action='translate_local',x=2}; "
            "assert(s:camera_info().z==-450 and s:camera_info().near==10); "
            "assert(not pcall(s.camera_set,s,{near=3000,far=10})); "
            "s:settings{lod_scale=200,water_time=2.5};assert(s:settings_info().water_time==2.5); "
            "s:gradient{0xffffff,0x102040};s:render();s:gradient(nil); "
            "p=s:point_light{x=-100,color=0xff0000,range_max=600,name='red'}; "
            "s:point_light_set(p,{enabled=false});assert(not s:point_light_info(p).enabled);s:remove(p); "
            "s:mesh_update(a,{{-60,-60,0},{0,60,0},{60,-60,0}},{{1,2,3}}); "
            "assert(s:object_info(a).triangles==1);s:render();s:close()");
        jet_test_assert_clean();
        run(L,"local s=jet.open();assert(s:capabilities().renderer_controls); "
            "s:settings{texture_lod=true,texture_near=300,texture_far=600};local info=s:settings_info(); "
            "assert(info.texture_lod and info.texture_far==600); "
            "assert(not pcall(s.settings,s,{texture_far=250,clear=false})); "
            "assert(s:settings_info().clear and s:settings_info().texture_far==600); "
            "local t=s:texture(2,2,string.rep(string.char(248,0),4));local m=s:material{texture=t,color=0x00ff00,shading='unlit'}; "
            "local c=s:cube(150,150,150);s:assign(c,m);s:render(); "
            "s:settings{texture_near=0,texture_far=1};s:render(); "
            "s:settings{wireframe=true};s:render();assert(s:settings_info().wireframe); "
            "s:settings{wireframe=false,texture_lod=false};s:render();s:close()");jet_test_assert_clean();
        run(L,"local s=jet.open();assert(s:capabilities().shaders); "
            "local sh=s:shader(nil,{kind='checker',color=0xff0000,secondary=0x0000ff}); "
            "local m=s:material{shader=sh,shading='unlit'};local c=s:cube(150,150,150);s:assign(c,m); "
            "s:render();assert(s:shader_info(sh).calls>10);assert(not pcall(s.remove,s,sh)); "
            "assert(not pcall(s.sprite,s,{material=m})); "
            "for _,kind in ipairs{'uv','depth'} do s:shader_set(sh,nil,{kind=kind});s:render();end; "
            "local calls=0;s:shader_set(sh,function(u,v,p) "
            "assert(type(u)=='number' and type(v)=='number' and p.depth>0 and p.frame>=3); "
            "assert(math.abs(p.world[1]-p['local'][1])<2 and math.abs(p.world[3]-p['local'][3])<2); "
            "calls=calls+1;return 0x00ff00 end);s:render();assert(calls>10); "
            "local co=coroutine.create(function() s:render() end);assert(coroutine.resume(co)); "
            "s:shader_set(sh,function() coroutine.yield() end);assert(not pcall(s.render,s)); "
            "s:postfx{motion_blur=50};local marker={};s:shader_set(sh,function() error(marker) end); "
            "local ok,err=pcall(s.render,s);assert(not ok and err==marker and s:shader_info(sh).failed); "
            "assert(not s:postfx_info().history_valid);s:postfx_clear(); "
            "s:shader_set(sh,function() return s:status() end);assert(not pcall(s.render,s)); "
            "s:shader_set(sh,function() s:close();return 0 end);assert(not pcall(s.render,s)); "
            "s:shader_set(sh,function() jet.open();return 0 end);assert(not pcall(s.render,s)); "
            "s:shader_set(sh,function() return -1 end);assert(not pcall(s.render,s)); "
            "s:shader_set(sh,function() return 'wrong' end);assert(not pcall(s.render,s)); "
            "s:shader_set(sh,function() return 0x00ff00 end);collectgarbage();s:render(); "
            "assert(not s:shader_info(sh).failed); "
            "local proto=s:prototype(c);s:remove(c);local batch=s:instance(proto,{x=10,z=25}); "
            "s:object_set(batch,{x=5,z=15,culling='none'});s:camera_set{near=490};calls=0; "
            "s:shader_set(sh,function(u,v,p) assert(math.abs(p.world[1]-p['local'][1]-15)<3 "
            "and math.abs(p.world[3]-p['local'][3]-40)<3,'instance coordinates');calls=calls+1;return 0x0088ff end); "
            "s:render();assert(calls>10,'clipped shader missed');s:remove(batch);s:remove(proto); "
            "s:material_set(m,{shader=0});s:remove(sh);s:render();s:close(); "
            "s=nil;collectgarbage();local function cycle() local t=jet.open();local sh=t:shader(function() t:status();return 0 end);end; "
            "cycle();collectgarbage();collectgarbage();local next=jet.open();next:close()");
        jet_test_assert_clean();
        run(L,"oomScene=jet.open();local sh=oomScene:shader(function() arm_alloc_failure();return 0xff0000 end); "
            "oomShader=sh;local m=oomScene:material{shader=sh,shading='unlit'}; "
            "local c=oomScene:cube(150,150,150);oomScene:assign(c,m);oom_ok=false;oom_error=false");
        run(L,"oom_ok,oom_error=pcall(oomScene.render,oomScene)");fail_lua_alloc=false;
        run(L,"assert(not oom_ok and type(oom_error)=='string' and oomScene:shader_info(oomShader).failed); "
            "assert(oomScene:status().presented==0);oomScene:shader_set(oomShader,nil,{kind='checker'}); "
            "oomScene:render();oomScene:close();oomScene=nil;collectgarbage()");jet_test_assert_clean();
        run(L,"s=jet.open(); c=s:cube(150,150,150); s:render()");assert(mode&&solua_jet_drawing());
        state.jet_busy=true;solua_jet_visibility(NULL,false);
        assert(state.jet_suspended&&!solua_jet_active());
        solua_jet_visibility(NULL,true);assert(!state.jet_suspended);
        /* The worker passed its wait just before the UI acquired the lock. */
        state.jet_busy=true;visibility_race=1;
        run(L,"s:render()");assert(visibility_race==0&&!solua_jet_active());
        run(L,"s:render();s:close()");assert(!mode);jet_test_assert_clean();
        run(L,"g=jet.open(); g:cube(150,150,150);g:render()");
        solua_jet_destroy();assert(!solua_jet_drawing()&&!solua_jet_active());jet_test_assert_clean();
        run(L,"assert(not pcall(g.render,g))");
        if(argc>=2) {
            lua_newtable(L);lua_getglobal(L,"jet");lua_setfield(L,-2,"jet");
            solua_set_func(L,-1,"should_exit",should_exit);
            lua_newtable(L);solua_set_func(L,-1,"sleep_ms",sleep_ms);lua_setfield(L,-2,"time");
            lua_getglobal(L,"package");lua_getfield(L,-1,"loaded");
            lua_pushvalue(L,-3);lua_setfield(L,-2,"solaros");lua_pop(L,3);
            if(luaL_dofile(L,argv[1])) {fprintf(stderr,"%s\n",lua_tostring(L,-1));abort();}
            jet_test_assert_clean();assert(!mode);
            size_t bytes;const uint8_t *pixels=jet_test_image(&bytes);unsigned nonwhite=0;
            for(size_t i=0;i<bytes;i+=2) nonwhite+=pixels[i]!=0xff||pixels[i+1]!=0xff;
            assert(nonwhite>10&&nonwhite<64*48/2);
            for(int example=2;example<argc;++example) {
                if(luaL_dofile(L,argv[example])) {fprintf(stderr,"%s\n",lua_tostring(L,-1));abort();}
                jet_test_assert_clean();assert(!mode);
            }
        }
        lua_close(L);jet_test_assert_clean();
    }
    puts("Lua Jet objects, mesh validation, stale handles, foreground lifecycle and example passed");return 0;
}
