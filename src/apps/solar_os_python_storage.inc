/* Per-domain python conversions; shared service APIs own policy and state. */
static mp_obj_t solaros_storage_status(void)
{
    char status[96];
    solar_os_storage_get_status(status, sizeof(status));
    return mp_obj_new_str_from_cstr(status);
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_status_obj, solaros_storage_status);

static mp_obj_t solaros_storage_is_mounted(void)
{
    return mp_obj_new_bool(solar_os_storage_is_mounted());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_is_mounted_obj, solaros_storage_is_mounted);

static mp_obj_t solaros_storage_mount(void)
{
    python_check_esp(solar_os_storage_mount());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_mount_obj, solaros_storage_mount);

static mp_obj_t solaros_storage_unmount(void)
{
    python_check_esp(solar_os_storage_unmount());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_unmount_obj, solaros_storage_unmount);

static mp_obj_t solaros_storage_mount_point(void)
{
    return mp_obj_new_str_from_cstr(solar_os_storage_mount_point());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_mount_point_obj, solaros_storage_mount_point);

static mp_obj_t solaros_storage_usage(size_t n_args, const mp_obj_t *args)
{
    solar_os_storage_usage_t usage;
    esp_err_t err;
    if (n_args == 0) {
        err = solar_os_storage_get_usage(&usage);
    } else {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        python_resolve_path_obj(args[0], path, sizeof(path));
        err = solar_os_storage_get_usage_for_path(path, &usage);
    }
    python_check_esp(err);
    return python_storage_usage_to_dict(&usage);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_storage_usage_obj, 0, 1, solaros_storage_usage);

static mp_obj_t solaros_storage_resolve(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    return mp_obj_new_str_from_cstr(path);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_resolve_obj, solaros_storage_resolve);

static mp_obj_t solaros_storage_stat(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    solar_os_storage_metadata_t metadata;
    python_check_esp(solar_os_storage_stat(path, &metadata));
    return python_storage_metadata_to_dict(&metadata);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_stat_obj, solaros_storage_stat);

static mp_obj_t solaros_storage_exists(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    bool exists = false;
    python_check_esp(solar_os_storage_exists(path, &exists));
    return mp_obj_new_bool(exists);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_exists_obj, solaros_storage_exists);

static mp_obj_t solaros_storage_scandir(size_t n_args, const mp_obj_t *args)
{
    size_t cursor = 0U;
    if (n_args >= 2 && args[1] != mp_const_none) {
        const mp_int_t value = mp_obj_get_int(args[1]);
        if (value < 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("cursor must be non-negative"));
        }
        cursor = (size_t)value;
    }

    const uint32_t limit = python_optional_u32(n_args, args, 2, 32U);
    if (limit == 0U || limit > SOLAR_OS_STORAGE_SCANDIR_MAX_LIMIT) {
        mp_raise_ValueError(MP_ERROR_TEXT("limit must be 1..128"));
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(args[0], path, sizeof(path));
    solar_os_storage_entry_t *entries = solar_os_memory_alloc(
        sizeof(*entries) * limit,
        SOLAR_OS_MEMORY_TRANSIENT,
        "python.scandir");
    if (entries == NULL) {
        python_raise_esp(ESP_ERR_NO_MEM);
    }

    size_t entry_count = 0U;
    size_t next_cursor = cursor;
    bool has_more = false;
    const esp_err_t err = solar_os_storage_scandir(path,
                                                   cursor,
                                                   limit,
                                                   entries,
                                                   &entry_count,
                                                   &next_cursor,
                                                   &has_more);
    if (err != ESP_OK) {
        solar_os_memory_free(entries);
        python_check_esp(err);
    }

    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0U; i < entry_count; i++) {
        mp_obj_list_append(list, python_storage_entry_to_dict(&entries[i]));
    }
    solar_os_memory_free(entries);

    mp_obj_t result = mp_obj_new_dict(2);
    mp_obj_dict_store(result, python_key("entries"), list);
    mp_obj_dict_store(result,
                      python_key("next_cursor"),
                      has_more ? mp_obj_new_int_from_uint(next_cursor) : mp_const_none);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_storage_scandir_obj,
                                    1,
                                    3,
                                    solaros_storage_scandir);

static mp_obj_t solaros_storage_read_file(size_t n_args, const mp_obj_t *args)
{
    const uint32_t max_bytes = python_optional_u32(n_args, args, 1, 4096U);
    if (max_bytes == 0 || max_bytes > SOLAR_OS_STORAGE_READ_MAX_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("max_bytes must be 1..65536"));
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(args[0], path, sizeof(path));

    uint8_t *data = python_alloc_psram_first(max_bytes);
    if (data == NULL) {
        python_raise_esp(ESP_ERR_NO_MEM);
    }

    size_t read_len = 0;
    const esp_err_t err = solar_os_storage_read_file(path,
                                                      data,
                                                      max_bytes,
                                                      &read_len);
    if (err != ESP_OK) {
        solar_os_memory_free(data);
        python_check_esp(err);
    }

    mp_obj_t result = mp_obj_new_bytes(data, read_len);
    solar_os_memory_free(data);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_storage_read_file_obj,
                                    1,
                                    2,
                                    solaros_storage_read_file);

static mp_obj_t solaros_storage_write_file(size_t n_args, const mp_obj_t *args)
{
    const void *data;
    size_t data_len;
    if (mp_obj_is_str(args[1])) {
        data = mp_obj_str_get_data(args[1], &data_len);
    } else {
        mp_buffer_info_t buffer;
        mp_get_buffer_raise(args[1], &buffer, MP_BUFFER_READ);
        data = buffer.buf;
        data_len = buffer.len;
    }
    if (data_len > SOLAR_OS_STORAGE_WRITE_MAX_BYTES) {
        mp_raise_ValueError(MP_ERROR_TEXT("data exceeds 65536 bytes"));
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(args[0], path, sizeof(path));
    const bool append = n_args > 2 && mp_obj_is_true(args[2]);
    python_check_esp(solar_os_storage_write_file(path, data, data_len, append));
    return mp_obj_new_int_from_uint(data_len);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_storage_write_file_obj,
                                    2,
                                    3,
                                    solaros_storage_write_file);

static mp_obj_t solaros_storage_rescan(void)
{
    python_check_esp(solar_os_storage_rescan());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_rescan_obj, solaros_storage_rescan);

static mp_obj_t solaros_storage_blocks(void)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    const size_t count = solar_os_storage_block_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        if (solar_os_storage_get_block(i, &block)) {
            mp_obj_list_append(list, python_storage_block_to_dict(&block));
        }
    }
    return list;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_blocks_obj, solaros_storage_blocks);

static mp_obj_t solaros_storage_block_count(void)
{
    return mp_obj_new_int_from_uint(solar_os_storage_block_count());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_storage_block_count_obj, solaros_storage_block_count);

static mp_obj_t solaros_storage_block(mp_obj_t index_obj)
{
    const mp_int_t index = mp_obj_get_int(index_obj);
    if (index < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected non-negative index"));
    }

    solar_os_storage_block_t block;
    if (!solar_os_storage_get_block((size_t)index, &block)) {
        python_raise_esp(ESP_ERR_NOT_FOUND);
    }
    return python_storage_block_to_dict(&block);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_block_obj, solaros_storage_block);

static mp_obj_t solaros_storage_usage_for_block(mp_obj_t index_obj)
{
    const mp_int_t index = mp_obj_get_int(index_obj);
    if (index < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected non-negative index"));
    }

    solar_os_storage_block_t block;
    if (!solar_os_storage_get_block((size_t)index, &block)) {
        python_raise_esp(ESP_ERR_NOT_FOUND);
    }

    solar_os_storage_usage_t usage;
    python_check_esp(solar_os_storage_get_usage_for_block(&block, &usage));
    return python_storage_usage_to_dict(&usage);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_usage_for_block_obj, solaros_storage_usage_for_block);

static mp_obj_t solaros_storage_mkdir(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    python_check_esp(solar_os_storage_mkdir(path));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_mkdir_obj, solaros_storage_mkdir);

static mp_obj_t solaros_storage_makedirs(size_t n_args, const mp_obj_t *args)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(args[0], path, sizeof(path));
    const bool exist_ok = n_args < 2 || args[1] == mp_const_none || mp_obj_is_true(args[1]);
    python_check_esp(solar_os_storage_makedirs(path, exist_ok));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_storage_makedirs_obj,
                                    1,
                                    2,
                                    solaros_storage_makedirs);

static mp_obj_t solaros_storage_rmdir(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    python_check_esp(solar_os_storage_rmdir(path));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_rmdir_obj, solaros_storage_rmdir);

static mp_obj_t solaros_storage_remove(mp_obj_t path_obj)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(path_obj, path, sizeof(path));
    python_check_esp(solar_os_storage_remove(path));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_remove_obj, solaros_storage_remove);

static mp_obj_t solaros_storage_rename(mp_obj_t old_path_obj, mp_obj_t new_path_obj)
{
    char old_path[SOLAR_OS_STORAGE_PATH_MAX];
    char new_path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(old_path_obj, old_path, sizeof(old_path));
    python_resolve_path_obj(new_path_obj, new_path, sizeof(new_path));
    python_check_esp(solar_os_storage_rename(old_path, new_path));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_storage_rename_obj, solaros_storage_rename);

static mp_obj_t solaros_storage_copy(mp_obj_t source_obj, mp_obj_t dest_obj)
{
    char source[SOLAR_OS_STORAGE_PATH_MAX];
    char dest[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(source_obj, source, sizeof(source));
    python_resolve_path_obj(dest_obj, dest, sizeof(dest));
    python_check_esp(solar_os_storage_copy_file(source, dest));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_storage_copy_obj, solaros_storage_copy);

static mp_obj_t solaros_storage_mount_volume(size_t n_args, const mp_obj_t *args)
{
    const char *name = mp_obj_str_get_str(args[0]);
    const char *mount_point = python_optional_str(n_args, args, 1, NULL);
    python_check_esp(solar_os_storage_mount_volume(name, mount_point));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_storage_mount_volume_obj,
                                    1,
                                    2,
                                    solaros_storage_mount_volume);

static mp_obj_t solaros_storage_unmount_volume(mp_obj_t target_obj)
{
    python_check_esp(solar_os_storage_unmount_volume(mp_obj_str_get_str(target_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_storage_unmount_volume_obj, solaros_storage_unmount_volume);
