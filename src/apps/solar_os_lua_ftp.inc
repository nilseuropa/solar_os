static solar_os_ftp_session_t *solua_ftp_connect_args(lua_State *L, int optional_index)
{
    const lua_Integer port_value = lua_isnoneornil(L, optional_index + 2) ?
        SOLAR_OS_FTP_DEFAULT_PORT : luaL_checkinteger(L, optional_index + 2);
    if (port_value <= 0 || port_value > UINT16_MAX) {
        luaL_error(L, "expected FTP port 1..65535");
    }
    const solar_os_ftp_options_t options = {
        .host = luaL_checkstring(L, 1),
        .port = (uint16_t)port_value,
        .username = solua_optional_str(L, optional_index, "anonymous"),
        .password = solua_optional_str(L, optional_index + 1, "solaros@"),
        .timeout_ms = SOLAR_OS_FTP_DEFAULT_TIMEOUT_MS,
    };
    solar_os_ftp_session_t *session = NULL;
    (void)solua_check_esp(L,
                          solar_os_ftp_connect(&options,
                                               solua_should_cancel,
                                               NULL,
                                               &session));
    return session;
}

typedef struct {
    lua_State *L;
    int table;
    lua_Integer index;
} solua_ftp_list_t;

static bool solua_ftp_list_entry(const solar_os_ftp_entry_t *entry, void *user)
{
    solua_ftp_list_t *result = (solua_ftp_list_t *)user;
    lua_createtable(result->L, 0, 3);
    solua_set_str(result->L, -1, "name", entry->name);
    solua_set_bool(result->L, -1, "is_directory", entry->is_directory);
    lua_pushinteger(result->L, (lua_Integer)entry->size);
    lua_setfield(result->L, -2, "size");
    lua_rawseti(result->L, result->table, result->index++);
    return true;
}

static int solua_ftp_list(lua_State *L)
{
    const char *path = solua_optional_str(L, 2, "/");
    solar_os_ftp_session_t *session = solua_ftp_connect_args(L, 3);
    lua_newtable(L);
    solua_ftp_list_t result = {
        .L = L,
        .table = lua_gettop(L),
        .index = 1,
    };
    const esp_err_t err = solar_os_ftp_list(session,
                                            path,
                                            solua_ftp_list_entry,
                                            &result);
    solar_os_ftp_disconnect(session);
    (void)solua_check_esp(L, err);
    return 1;
}

static int solua_ftp_download(lua_State *L)
{
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 3, local_path, sizeof(local_path));
    solar_os_ftp_session_t *session = solua_ftp_connect_args(L, 4);
    const esp_err_t err = solar_os_ftp_download(session,
                                                luaL_checkstring(L, 2),
                                                local_path,
                                                NULL,
                                                NULL);
    solar_os_ftp_disconnect(session);
    return solua_check_esp(L, err);
}

static int solua_ftp_upload(lua_State *L)
{
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 2, local_path, sizeof(local_path));
    solar_os_ftp_session_t *session = solua_ftp_connect_args(L, 4);
    const esp_err_t err = solar_os_ftp_upload(session,
                                              local_path,
                                              luaL_checkstring(L, 3),
                                              NULL,
                                              NULL);
    solar_os_ftp_disconnect(session);
    return solua_check_esp(L, err);
}

typedef esp_err_t (*solua_ftp_path_fn_t)(solar_os_ftp_session_t *, const char *);

static int solua_ftp_path_call(lua_State *L, solua_ftp_path_fn_t operation)
{
    solar_os_ftp_session_t *session = solua_ftp_connect_args(L, 3);
    const esp_err_t err = operation(session, luaL_checkstring(L, 2));
    solar_os_ftp_disconnect(session);
    return solua_check_esp(L, err);
}

static int solua_ftp_mkdir(lua_State *L)
{
    return solua_ftp_path_call(L, solar_os_ftp_mkdir);
}

static int solua_ftp_rmdir(lua_State *L)
{
    return solua_ftp_path_call(L, solar_os_ftp_rmdir);
}

static int solua_ftp_remove(lua_State *L)
{
    return solua_ftp_path_call(L, solar_os_ftp_remove);
}

static int solua_ftp_rename(lua_State *L)
{
    solar_os_ftp_session_t *session = solua_ftp_connect_args(L, 4);
    const esp_err_t err = solar_os_ftp_rename(session,
                                              luaL_checkstring(L, 2),
                                              luaL_checkstring(L, 3));
    solar_os_ftp_disconnect(session);
    return solua_check_esp(L, err);
}
