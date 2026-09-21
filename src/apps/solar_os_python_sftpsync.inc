typedef struct {
    bool remote;
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char host[SOLAR_OS_SSH_HOST_MAX];
    const char *path;
} python_sftpsync_target_t;

static bool python_sftpsync_parse_remote(const char *arg,
                                         python_sftpsync_target_t *target)
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

static mp_obj_t solaros_sftpsync_sync(size_t n_args, const mp_obj_t *args)
{
    python_sftpsync_target_t source = {0};
    python_sftpsync_target_t destination = {0};
    const char *source_arg = mp_obj_str_get_str(args[0]);
    const char *destination_arg = mp_obj_str_get_str(args[1]);
    (void)python_sftpsync_parse_remote(source_arg, &source);
    (void)python_sftpsync_parse_remote(destination_arg, &destination);
    if (source.remote == destination.remote) {
        mp_raise_ValueError(MP_ERROR_TEXT("exactly one path must be [user@]host:path"));
    }

    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    const python_sftpsync_target_t *remote = source.remote ? &source : &destination;
    python_resolve_path_obj(source.remote ? args[1] : args[0],
                            local_path,
                            sizeof(local_path));
    const uint32_t port = python_optional_u32(n_args, args, 4U, 22U);
    if (port == 0U || port > UINT16_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected SSH port 1..65535"));
    }
    const solar_os_sftpsync_config_t config = {
        .direction = source.remote ? SOLAR_OS_SFTPSYNC_DOWNLOAD : SOLAR_OS_SFTPSYNC_UPLOAD,
        .host = remote->host,
        .port = (uint16_t)port,
        .username = remote->username,
        .password = python_optional_str(n_args, args, 5U, ""),
        .local_path = local_path,
        .remote_path = remote->path,
        .recursive = n_args > 2U && mp_obj_is_true(args[2]),
        .dry_run = n_args > 3U && mp_obj_is_true(args[3]),
    };

    solar_os_sftpsync_session_t *session = NULL;
    python_check_esp(solar_os_sftpsync_start(&config, &session));
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
        if (!done && python_should_cancel(NULL)) {
            (void)solar_os_sftpsync_stop(session);
            mp_raise_type(&mp_type_KeyboardInterrupt);
        }
        if (!done) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (!solar_os_sftpsync_stop(session)) {
        python_check_esp(ESP_ERR_TIMEOUT);
    }
    if (failed) {
        python_check_esp(ESP_FAIL);
    }

    mp_obj_t result = mp_obj_new_dict(3);
    python_dict_store_uint(result, "files_changed", files_changed);
    python_dict_store_u64(result, "bytes_transferred", transferred);
    python_dict_store_bool(result, "dry_run", config.dry_run);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_sftpsync_sync_obj,
                                    2,
                                    6,
                                    solaros_sftpsync_sync);
