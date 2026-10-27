typedef struct {
    solua_jet_t *self;uint32_t slot,*color;double u,v;
    const solar_os_jet_shader_parameters_t *params;
} solua_jet_shader_call_t;
static int solua_jet_shader_protected(lua_State *L) {
    solua_jet_shader_call_t *call=lua_touserdata(L,1);
    lua_getiuservalue(L,2,1);lua_rawgeti(L,-1,call->slot+1);
    lua_pushnumber(L,call->u);lua_pushnumber(L,call->v);lua_newtable(L);
    const solar_os_jet_shader_parameters_t *p=call->params;
    solua_set_int(L,-1,"x",p->screen_x);solua_set_int(L,-1,"y",p->screen_y);
    solua_set_int(L,-1,"depth",p->depth);solua_set_int(L,-1,"frame",p->frame);
    lua_createtable(L,3,0);for(unsigned i=0;i<3;++i) {lua_pushinteger(L,p->local[i]);lua_rawseti(L,-2,i+1);}
    lua_setfield(L,-2,"local");
    lua_createtable(L,3,0);for(unsigned i=0;i<3;++i) {lua_pushinteger(L,p->world[i]);lua_rawseti(L,-2,i+1);}
    lua_setfield(L,-2,"world");lua_call(L,3,1);
    lua_Integer color=luaL_checkinteger(L,-1);
    if(color<0||color>0xffffff)return luaL_error(L,"shader color out of range");
    *call->color=(uint32_t)color;return 0;
}
static int solua_jet_shader_callback(void *user,uint32_t slot,double u,double v,
    const solar_os_jet_shader_parameters_t *params,uint32_t *color) {
    solua_jet_t *self=user;lua_State *L=self->state;
    if(self->shader_error)return SOLAR_OS_NATIVE_ERROR_FAILED;
    solua_jet_shader_call_t call={self,slot,color,u,v,params};
    // render() reserves stack space before entering C++. These pushes cannot
    // allocate; every operation that can raise runs inside lua_pcall.
    lua_pushcfunction(L,solua_jet_shader_protected);lua_pushlightuserdata(L,&call);lua_pushvalue(L,1);
    solua_jet_in_callback=true;int status=lua_pcall(L,2,0,0);solua_jet_in_callback=false;
    if(status!=LUA_OK)self->shader_error=lua_gettop(L);
    return status==LUA_OK ? 0 : SOLAR_OS_NATIVE_ERROR_FAILED;
}
static int solua_jet_shader(lua_State *L,bool update) {
    solar_os_jet_t *scene=solua_jet_scene(L);solua_jet_t *self=lua_touserdata(L,1);
    int function=update ? 3 : 2,options=function+1;
    if(lua_gettop(L)>options)return luaL_error(L,"too many Jet shader arguments");
    uint32_t handle=update ? solua_jet_id(L,2) : 0;
    unsigned slot=0;for(;slot<SOLAR_OS_JET_MAX_SHADERS;++slot)if(self->shader_ids[slot]==handle)break;
    if(slot==SOLAR_OS_JET_MAX_SHADERS)return luaL_error(L,"Jet shader slot unavailable");
    solar_os_jet_command_t q={.opcode=update ? SOLAR_OS_JET_SHADER_SET : SOLAR_OS_JET_SHADER,.handle=handle};solar_os_jet_reply_t r;
    solua_jet_options(L,options,jet_shader_fields,JET_SHADER_COUNT,&q);
    solar_os_jet_shader_callback_t callback={solua_jet_shader_callback,self,slot};
    bool replace=!lua_isnoneornil(L,function);
    if(replace) {
        luaL_checktype(L,function,LUA_TFUNCTION);
        if((q.fields&(1U<<JET_SHADER_kind))&&q.values[JET_SHADER_kind]!=3)return luaL_error(L,"callback requires shader kind callback");
        q.fields|=1U<<JET_SHADER_kind;q.values[JET_SHADER_kind]=3;q.data=&callback;q.bytes=sizeof(callback);
    }
    lua_getiuservalue(L,1,1);int roots=lua_gettop(L);lua_rawgeti(L,roots,slot+1);int prior=lua_gettop(L);
    bool clear=!update||((q.fields&(1U<<JET_SHADER_kind))&&q.values[JET_SHADER_kind]!=3);
    if(replace||clear) {if(replace)lua_pushvalue(L,function);else lua_pushboolean(L,0);lua_rawseti(L,roots,slot+1);}
    esp_err_t err;SOLUA_JET_CALL((err=solar_os_jet_execute(scene,&q,&r),ESP_OK));
    if(err!=ESP_OK) {lua_pushvalue(L,prior);lua_rawseti(L,roots,slot+1);solua_check_esp(L,err);}
    self->shader_ids[slot]=r.handle;
    if(!update) {lua_pushinteger(L,r.handle);return 1;}return 0;
}
static int solua_jet_scene_shader(lua_State *L) {return solua_jet_shader(L,false);}
static int solua_jet_scene_shader_set(lua_State *L) {return solua_jet_shader(L,true);}
static int solua_jet_scene_shader_info(lua_State *L) {
    solar_os_jet_t *scene=solua_jet_scene(L);
    solar_os_jet_command_t q={.opcode=SOLAR_OS_JET_SHADER_INFO,.handle=solua_jet_id(L,2)};solar_os_jet_reply_t r;
    SOLUA_JET_CALL(solar_os_jet_execute(scene,&q,&r));solua_jet_info_table(L,&r,jet_shader_fields,JET_SHADER_COUNT);
    solua_set_int(L,-1,"calls",r.values[16]);lua_pushboolean(L,r.values[17]);lua_setfield(L,-2,"failed");return 1;
}
