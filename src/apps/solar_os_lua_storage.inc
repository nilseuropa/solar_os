/* Per-domain lua conversions; shared service APIs own policy and state. */
static int solua_storage_status(lua_State *L)
{
    char status[96];
    solar_os_storage_get_status(status, sizeof(status));
    lua_pushstring(L, status);
    return 1;
}

static int solua_storage_is_mounted(lua_State *L)
{
    lua_pushboolean(L, solar_os_storage_is_mounted());
    return 1;
}

static int solua_storage_mount(lua_State *L)
{
    return solua_check_esp(L, solar_os_storage_mount());
}

static int solua_storage_unmount(lua_State *L)
{
    return solua_check_esp(L, solar_os_storage_unmount());
}

static int solua_storage_mount_point(lua_State *L)
{
    lua_pushstring(L, solar_os_storage_mount_point());
    return 1;
}

static int solua_storage_usage(lua_State *L)
{
    solar_os_storage_usage_t usage;
    esp_err_t err;
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        err = solar_os_storage_get_usage(&usage);
    } else {
        char path[SOLAR_OS_STORAGE_PATH_MAX];
        solua_resolve_path(L, 1, path, sizeof(path));
        err = solar_os_storage_get_usage_for_path(path, &usage);
    }
    (void)solua_check_esp(L, err);
    solua_push_storage_usage(L, &usage);
    return 1;
}

static int solua_storage_resolve(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    lua_pushstring(L, path);
    return 1;
}

static int solua_storage_stat(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    solar_os_storage_metadata_t metadata;
    (void)solua_check_esp(L, solar_os_storage_stat(path, &metadata));
    solua_push_storage_metadata(L, &metadata);
    return 1;
}

static int solua_storage_exists(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    bool exists = false;
    (void)solua_check_esp(L, solar_os_storage_exists(path, &exists));
    lua_pushboolean(L, exists);
    return 1;
}

static int solua_storage_scandir(lua_State *L)
{
    size_t cursor = 0U;
    if (!lua_isnoneornil(L, 2)) {
        const lua_Integer value = luaL_checkinteger(L, 2);
        if (value < 0) {
            return luaL_error(L, "cursor must be non-negative");
        }
        cursor = (size_t)value;
    }

    const uint32_t limit = solua_optional_u32(L, 3, 32U);
    if (limit == 0U || limit > SOLAR_OS_STORAGE_SCANDIR_MAX_LIMIT) {
        return luaL_error(L, "limit must be 1..128");
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    solar_os_storage_entry_t *entries = solar_os_memory_alloc(
        sizeof(*entries) * limit,
        SOLAR_OS_MEMORY_TRANSIENT,
        "lua.scandir");
    if (entries == NULL) {
        return solua_check_esp(L, ESP_ERR_NO_MEM);
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
        return solua_check_esp(L, err);
    }

    lua_newtable(L);
    const int result = lua_gettop(L);
    lua_newtable(L);
    const int list = lua_gettop(L);
    for (size_t i = 0U; i < entry_count; i++) {
        solua_push_storage_entry(L, &entries[i]);
        lua_rawseti(L, list, (lua_Integer)i + 1);
    }
    solar_os_memory_free(entries);
    lua_setfield(L, result, "entries");
    if (has_more) {
        solua_set_int(L, result, "next_cursor", (lua_Integer)next_cursor);
    }
    return 1;
}

static int solua_storage_read_file(lua_State *L)
{
    const uint32_t max_bytes = solua_optional_u32(L, 2, 4096U);
    if (max_bytes == 0 || max_bytes > SOLAR_OS_STORAGE_READ_MAX_BYTES) {
        return luaL_error(L, "max_bytes must be 1..65536");
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));

    uint8_t *data = solar_os_memory_alloc(max_bytes,
                                           SOLAR_OS_MEMORY_TRANSIENT,
                                           "lua.storage");
    if (data == NULL) {
        return solua_check_esp(L, ESP_ERR_NO_MEM);
    }

    size_t read_len = 0;
    const esp_err_t err = solar_os_storage_read_file(path,
                                                      data,
                                                      max_bytes,
                                                      &read_len);
    if (err != ESP_OK) {
        solar_os_memory_free(data);
        return solua_check_esp(L, err);
    }

    lua_pushlstring(L, (const char *)data, read_len);
    solar_os_memory_free(data);
    return 1;
}

static int solua_storage_write_file(lua_State *L)
{
    size_t data_len = 0U;
    const char *data = luaL_checklstring(L, 2, &data_len);
    if (data_len > SOLAR_OS_STORAGE_WRITE_MAX_BYTES) {
        return luaL_error(L, "data exceeds 65536 bytes");
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    const bool append = lua_toboolean(L, 3);
    (void)solua_check_esp(L, solar_os_storage_write_file(path, data, data_len, append));
    lua_pushinteger(L, (lua_Integer)data_len);
    return 1;
}

static int solua_storage_rescan(lua_State *L)
{
    return solua_check_esp(L, solar_os_storage_rescan());
}

static int solua_storage_blocks(lua_State *L)
{
    lua_newtable(L);
    const int list = lua_gettop(L);
    const size_t count = solar_os_storage_block_count();
    int out = 1;
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        if (solar_os_storage_get_block(i, &block)) {
            solua_push_storage_block(L, &block);
            lua_rawseti(L, list, out++);
        }
    }
    return 1;
}

static int solua_storage_block_count(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)solar_os_storage_block_count());
    return 1;
}

static int solua_storage_block(lua_State *L)
{
    const lua_Integer index = luaL_checkinteger(L, 1);
    if (index < 0) {
        return luaL_error(L, "expected non-negative index");
    }

    solar_os_storage_block_t block;
    if (!solar_os_storage_get_block((size_t)index, &block)) {
        return solua_check_esp(L, ESP_ERR_NOT_FOUND);
    }
    solua_push_storage_block(L, &block);
    return 1;
}

static int solua_storage_usage_for_block(lua_State *L)
{
    const lua_Integer index = luaL_checkinteger(L, 1);
    if (index < 0) {
        return luaL_error(L, "expected non-negative index");
    }

    solar_os_storage_block_t block;
    if (!solar_os_storage_get_block((size_t)index, &block)) {
        return solua_check_esp(L, ESP_ERR_NOT_FOUND);
    }

    solar_os_storage_usage_t usage;
    (void)solua_check_esp(L, solar_os_storage_get_usage_for_block(&block, &usage));
    solua_push_storage_usage(L, &usage);
    return 1;
}

static int solua_storage_mkdir(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    return solua_check_esp(L, solar_os_storage_mkdir(path));
}

static int solua_storage_makedirs(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    const bool exist_ok = lua_isnoneornil(L, 2) || lua_toboolean(L, 2);
    return solua_check_esp(L, solar_os_storage_makedirs(path, exist_ok));
}

static int solua_storage_rmdir(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    return solua_check_esp(L, solar_os_storage_rmdir(path));
}

static int solua_storage_remove(lua_State *L)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, path, sizeof(path));
    return solua_check_esp(L, solar_os_storage_remove(path));
}

static int solua_storage_rename(lua_State *L)
{
    char old_path[SOLAR_OS_STORAGE_PATH_MAX];
    char new_path[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, old_path, sizeof(old_path));
    solua_resolve_path(L, 2, new_path, sizeof(new_path));
    return solua_check_esp(L, solar_os_storage_rename(old_path, new_path));
}

static int solua_storage_copy(lua_State *L)
{
    char source[SOLAR_OS_STORAGE_PATH_MAX];
    char dest[SOLAR_OS_STORAGE_PATH_MAX];
    solua_resolve_path(L, 1, source, sizeof(source));
    solua_resolve_path(L, 2, dest, sizeof(dest));
    return solua_check_esp(L, solar_os_storage_copy_file(source, dest));
}

static int solua_storage_mount_volume(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *mount_point = solua_optional_str(L, 2, NULL);
    return solua_check_esp(L, solar_os_storage_mount_volume(name, mount_point));
}

static int solua_storage_unmount_volume(lua_State *L)
{
    return solua_check_esp(L, solar_os_storage_unmount_volume(luaL_checkstring(L, 1)));
}
