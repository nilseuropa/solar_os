static solar_os_ftp_session_t *python_ftp_connect_args(size_t n_args,
                                                       const mp_obj_t *args,
                                                       size_t optional_index)
{
    const char *host = mp_obj_str_get_str(args[0]);
    const char *username = python_optional_str(n_args,
                                                args,
                                                optional_index,
                                                "anonymous");
    const char *password = python_optional_str(n_args,
                                                args,
                                                optional_index + 1U,
                                                "solaros@");
    const uint32_t port_value = optional_index + 2U < n_args ?
        python_u32_from_obj(args[optional_index + 2U]) : SOLAR_OS_FTP_DEFAULT_PORT;
    if (port_value == 0 || port_value > UINT16_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected FTP port 1..65535"));
    }
    const solar_os_ftp_options_t options = {
        .host = host,
        .port = (uint16_t)port_value,
        .username = username,
        .password = password,
        .timeout_ms = SOLAR_OS_FTP_DEFAULT_TIMEOUT_MS,
    };
    solar_os_ftp_session_t *session = NULL;
    python_check_esp(solar_os_ftp_connect(&options,
                                          python_should_cancel,
                                          NULL,
                                          &session));
    return session;
}

typedef struct {
    mp_obj_t list;
} python_ftp_list_t;

static bool python_ftp_list_entry(const solar_os_ftp_entry_t *entry, void *user)
{
    python_ftp_list_t *result = (python_ftp_list_t *)user;
    mp_obj_t item = mp_obj_new_dict(3);
    python_dict_store_cstr(item, "name", entry->name);
    python_dict_store_bool(item, "is_directory", entry->is_directory);
    python_dict_store_u64(item, "size", entry->size);
    mp_obj_list_append(result->list, item);
    return true;
}

static mp_obj_t solaros_ftp_list(size_t n_args, const mp_obj_t *args)
{
    const char *path = n_args > 1U ? mp_obj_str_get_str(args[1]) : "/";
    solar_os_ftp_session_t *session = python_ftp_connect_args(n_args, args, 2U);
    python_ftp_list_t result = {.list = mp_obj_new_list(0, NULL)};
    const esp_err_t err = solar_os_ftp_list(session,
                                            path,
                                            python_ftp_list_entry,
                                            &result);
    solar_os_ftp_disconnect(session);
    python_check_esp(err);
    return result.list;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ftp_list_obj, 1, 5, solaros_ftp_list);

static mp_obj_t solaros_ftp_download(size_t n_args, const mp_obj_t *args)
{
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(args[2], local_path, sizeof(local_path));
    solar_os_ftp_session_t *session = python_ftp_connect_args(n_args, args, 3U);
    const esp_err_t err = solar_os_ftp_download(session,
                                                mp_obj_str_get_str(args[1]),
                                                local_path,
                                                NULL,
                                                NULL);
    solar_os_ftp_disconnect(session);
    python_check_esp(err);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ftp_download_obj,
                                    3,
                                    6,
                                    solaros_ftp_download);

static mp_obj_t solaros_ftp_upload(size_t n_args, const mp_obj_t *args)
{
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(args[1], local_path, sizeof(local_path));
    solar_os_ftp_session_t *session = python_ftp_connect_args(n_args, args, 3U);
    const esp_err_t err = solar_os_ftp_upload(session,
                                              local_path,
                                              mp_obj_str_get_str(args[2]),
                                              NULL,
                                              NULL);
    solar_os_ftp_disconnect(session);
    python_check_esp(err);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ftp_upload_obj, 3, 6, solaros_ftp_upload);

typedef esp_err_t (*python_ftp_path_fn_t)(solar_os_ftp_session_t *, const char *);

static mp_obj_t python_ftp_path_call(size_t n_args,
                                     const mp_obj_t *args,
                                     python_ftp_path_fn_t operation)
{
    solar_os_ftp_session_t *session = python_ftp_connect_args(n_args, args, 2U);
    const esp_err_t err = operation(session, mp_obj_str_get_str(args[1]));
    solar_os_ftp_disconnect(session);
    python_check_esp(err);
    return mp_const_none;
}

static mp_obj_t solaros_ftp_mkdir(size_t n_args, const mp_obj_t *args)
{
    return python_ftp_path_call(n_args, args, solar_os_ftp_mkdir);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ftp_mkdir_obj, 2, 5, solaros_ftp_mkdir);

static mp_obj_t solaros_ftp_rmdir(size_t n_args, const mp_obj_t *args)
{
    return python_ftp_path_call(n_args, args, solar_os_ftp_rmdir);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ftp_rmdir_obj, 2, 5, solaros_ftp_rmdir);

static mp_obj_t solaros_ftp_remove(size_t n_args, const mp_obj_t *args)
{
    return python_ftp_path_call(n_args, args, solar_os_ftp_remove);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ftp_remove_obj, 2, 5, solaros_ftp_remove);

static mp_obj_t solaros_ftp_rename(size_t n_args, const mp_obj_t *args)
{
    solar_os_ftp_session_t *session = python_ftp_connect_args(n_args, args, 3U);
    const esp_err_t err = solar_os_ftp_rename(session,
                                              mp_obj_str_get_str(args[1]),
                                              mp_obj_str_get_str(args[2]));
    solar_os_ftp_disconnect(session);
    python_check_esp(err);
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_ftp_rename_obj, 3, 6, solaros_ftp_rename);
