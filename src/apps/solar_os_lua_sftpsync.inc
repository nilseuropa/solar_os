typedef struct {
    bool remote;
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char host[SOLAR_OS_SSH_HOST_MAX];
    const char *path;
} solua_sftpsync_target_t;

static bool solua_sftpsync_parse_remote(const char *arg,
                                        solua_sftpsync_target_t *target)
{
    const char *colon = arg != NULL ? strchr(arg, ':') : NULL;
    if (colon == NULL || colon == arg || colon[1] == '\0') {
        return false;
    }
    const size_t authority_len = (size_t)(colon - arg);
    if (authority_len >= SOLAR_OS_SSH_USERNAME_MAX + SOLAR_OS_SSH_HOST_MAX + 2U) {
        return false;
    }
    char authority[SOLAR_OS_SSH_USERNAME_MAX + SOLAR_OS_SSH_HOST_MAX + 2U];
    memcpy(authority, arg, authority_len);
    authority[authority_len] = '\0';

    char *host = authority;
    char *at = strchr(authority, '@');
    if (at != NULL) {
        if (at == authority || at[1] == '\0') {
            return false;
        }
        *at = '\0';
        host = at + 1;
        strlcpy(target->username, authority, sizeof(target->username));
    } else {
        solar_os_identity_get_user(target->username, sizeof(target->username));
    }
    if (target->username[0] == '\0' || host[0] == '\0') {
        return false;
    }
    target->remote = true;
    strlcpy(target->host, host, sizeof(target->host));
    target->path = colon + 1;
    return true;
}

static int solua_sftpsync_sync(lua_State *L)
{
    solua_sftpsync_target_t source = {0};
    solua_sftpsync_target_t destination = {0};
    (void)solua_sftpsync_parse_remote(luaL_checkstring(L, 1), &source);
    (void)solua_sftpsync_parse_remote(luaL_checkstring(L, 2), &destination);
    if (source.remote == destination.remote) {
        return luaL_error(L, "exactly one path must be [user@]host:path");
    }

    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    const solua_sftpsync_target_t *remote = source.remote ? &source : &destination;
    solua_resolve_path(L, source.remote ? 2 : 1, local_path, sizeof(local_path));
    const uint32_t port = solua_optional_u32(L, 5, 22U);
    if (port == 0U || port > UINT16_MAX) {
        return luaL_error(L, "expected SSH port 1..65535");
    }
    const solar_os_sftpsync_config_t config = {
        .direction = source.remote ? SOLAR_OS_SFTPSYNC_DOWNLOAD : SOLAR_OS_SFTPSYNC_UPLOAD,
        .host = remote->host,
        .port = (uint16_t)port,
        .username = remote->username,
        .password = solua_optional_str(L, 6, ""),
        .local_path = local_path,
        .remote_path = remote->path,
        .recursive = lua_toboolean(L, 3),
        .dry_run = lua_toboolean(L, 4),
    };

    solar_os_sftpsync_session_t *session = NULL;
    (void)solua_check_esp(L, solar_os_sftpsync_start(&config, &session));
    bool done = false;
    bool failed = false;
    uint64_t transferred = 0;
    size_t files_changed = 0;
    while (!done) {
        solar_os_sftpsync_event_t event;
        while (solar_os_sftpsync_poll(session, &event)) {
            transferred = event.transferred;
            files_changed = event.files_changed;
            failed = failed || event.type == SOLAR_OS_SFTPSYNC_EVENT_ERROR;
            done = event.type == SOLAR_OS_SFTPSYNC_EVENT_DONE;
        }
        if (!done && solua_should_cancel(NULL)) {
            (void)solar_os_sftpsync_stop(session);
            solua.interrupt_requested = true;
            solua.interrupted = true;
            return luaL_error(L, "interrupted");
        }
        if (!done) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (!solar_os_sftpsync_stop(session)) {
        return solua_check_esp(L, ESP_ERR_TIMEOUT);
    }
    if (failed) {
        return solua_check_esp(L, ESP_FAIL);
    }

    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)files_changed);
    lua_setfield(L, -2, "files_changed");
    lua_pushinteger(L, (lua_Integer)transferred);
    lua_setfield(L, -2, "bytes_transferred");
    solua_set_bool(L, -1, "dry_run", config.dry_run);
    return 1;
}
