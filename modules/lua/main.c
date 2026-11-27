#include "lua_runtime.h"

#include <limits.h>
#include <string.h>
#include "lualib.h"

solar_os_lua_runtime_t lua_runtime;
extern void *solar_os_lua_allocate(void *, void *, size_t, size_t);
extern char *solar_os_lua_console_fgets(char *, int, FILE *);

SOLAR_OS_NATIVE_APP_MANIFEST = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
    .lifecycle_abi = SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_app_manifest_v2_t),
    .app_class = SOLAR_OS_NATIVE_APP_CLASS_TUI,
    .flags = SOLAR_OS_NATIVE_APP_CONSOLE,
    .worker_stack_bytes = 12288,
    .worker_priority = 2,
    .minimum_psram_bytes = 768U * 1024U,
    .tick_interval_ms = 20,
    .id = "lua",
    .summary = "Lua runtime",
    .file_extensions = ".lua",
};

bool solar_os_lua_stopping(void)
{
    return lua_runtime.exiting || lua_runtime.foreground->stop_requested();
}

int solar_os_lua_read_key(uint32_t timeout_ms)
{
    uint64_t started = lua_runtime.foreground->time_us();
    do {
        solar_os_native_foreground_key_v1_t key;
        bool received;
        if (lua_runtime.foreground->read_key(&key, &received)) return -1;
        if (received && !key.reset && key.action != SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE) {
            return key.key;
        }
        if (!received && timeout_ms) lua_runtime.foreground->sleep_ms(5);
    } while (!solar_os_lua_stopping() &&
             lua_runtime.foreground->time_us() - started < (uint64_t)timeout_ms * 1000U);
    return -1;
}

int solar_os_lua_check(lua_State *L, int result)
{
    return result ? luaL_error(L, "SolarOS operation failed (%d)", result) : 0;
}

lua_Integer solar_os_lua_integer(lua_State *L, int index, lua_Integer minimum,
                                lua_Integer maximum, lua_Integer fallback)
{
    lua_Integer value = luaL_optinteger(L, index, fallback);
    if (value < minimum || value > maximum) luaL_argerror(L, index, "value out of range");
    return value;
}

void solar_os_lua_set_number(lua_State *L, const char *name, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name);
}

static void lua_checkpoint(lua_State *L, lua_Debug *event)
{
    (void)event;
    if (solar_os_lua_stopping()) luaL_error(L, "Lua interrupted");
    solar_os_native_foreground_cooperate(lua_runtime.foreground);
}

static int lua_exit(lua_State *L)
{
    lua_runtime.status = (int)solar_os_lua_integer(L, 1, 0, INT_MAX, 0);
    lua_runtime.exiting = true;
    return luaL_error(L, "Lua exited");
}

static int lua_write(lua_State *L)
{
    size_t size;
    const char *text = luaL_checklstring(L, 1, &size);
    solar_os_lua_write(text, size);
    return 0;
}

static int lua_os_version(lua_State *L)
{
    lua_pushstring(L, lua_runtime.host->firmware_version);
    return 1;
}

static int lua_should_exit(lua_State *L)
{
    lua_pushboolean(L, solar_os_lua_stopping());
    return 1;
}

static int lua_uptime(lua_State *L)
{
    lua_pushinteger(L, lua_runtime.foreground->time_us() / 1000U);
    return 1;
}

static int lua_sleep(lua_State *L)
{
    unsigned ms = (unsigned)solar_os_lua_integer(L, 1, 0, 3600000, 0);
    do {
        unsigned delay = ms < 20U ? ms : 20U;
        lua_runtime.foreground->sleep_ms(delay);
        ms -= delay;
    } while (ms && !solar_os_lua_stopping());
    return 0;
}

static int lua_require(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    if (strcmp(name, "solaros")) return luaL_error(L, "module '%s' not found", name);
    lua_getglobal(L, "solaros");
    return 1;
}

static int lua_bootstrap(lua_State *L)
{
    static const luaL_Reg libraries[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},
        {LUA_DBLIBNAME, luaopen_debug},
    };
    for (size_t i = 0; i < sizeof(libraries) / sizeof(*libraries); ++i) {
        luaL_requiref(L, libraries[i].name, libraries[i].func, 1);
        lua_pop(L, 1);
    }
    lua_pushcfunction(L, lua_exit);
    lua_setglobal(L, "exit");
    lua_pushcfunction(L, lua_require);
    lua_setglobal(L, "require");

    static const luaL_Reg methods[] = {
        {"write", lua_write}, {"version", lua_os_version}, {"should_exit", lua_should_exit},
        {NULL, NULL},
    };
    static const luaL_Reg time[] = {
        {"uptime_ms", lua_uptime}, {"sleep_ms", lua_sleep}, {NULL, NULL},
    };
    luaL_newlib(L, methods);
    luaL_newlib(L, time);
    lua_setfield(L, -2, "time");
    solar_os_lua_graphics(L);
    solar_os_lua_audio(L);
    solar_os_lua_jet(L);
    lua_setglobal(L, "solaros");
    return 0;
}

static void lua_error_output(lua_State *L)
{
    size_t length;
    const char *message = lua_tolstring(L, -1, &length);
    if (message && !lua_runtime.exiting) {
        solar_os_lua_write(message, length);
        solar_os_lua_write("\n", 1);
    }
    lua_pop(L, 1);
}

static int lua_script(lua_State *L, int argc, char **argv)
{
    lua_createtable(L, argc - 2, 1);
    for (int i = 1; i < argc; ++i) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }
    lua_setglobal(L, "arg");
    const int result = luaL_loadfilex(L, argv[1], "t");
    return result == LUA_OK ? lua_pcall(L, 0, 0, 0) : result;
}

static void lua_repl(lua_State *L)
{
    char line[1024];
    while (!solar_os_lua_stopping()) {
        solar_os_lua_write("> ", 2);
        if (!solar_os_lua_console_fgets(line, sizeof(line), NULL)) break;
        const char *expression = line[0] == '=' ? line + 1 : line;
        lua_pushfstring(L, "return %s", expression);
        int result = luaL_loadstring(L, lua_tostring(L, -1));
        lua_remove(L, -2);
        if (result == LUA_ERRSYNTAX && line[0] != '=') {
            lua_pop(L, 1);
            result = luaL_loadstring(L, line);
        }
        if (result == LUA_OK) result = lua_pcall(L, 0, LUA_MULTRET, 0);
        if (result == LUA_OK && lua_gettop(L)) {
            lua_getglobal(L, "print");
            lua_insert(L, 1);
            result = lua_pcall(L, lua_gettop(L) - 1, 0, 0);
        }
        if (result != LUA_OK) lua_error_output(L);
        lua_settop(L, 0);
    }
}

__attribute__((visibility("default"))) int app_main(int argc, char **argv)
{
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_get(
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_host_api_v1_t, get_service));
    if (!host || !host->get_service || !host->write_utf8) return 1;

    lua_runtime = (solar_os_lua_runtime_t){
        .host = host,
        .foreground = solar_os_native_foreground_get(host->get_service,
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_foreground_api_v1_t, console_gfx_release)),
        .memory = solar_os_native_memory_get(host->get_service,
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_memory_api_v1_t, realloc_class)),
        .libc = solar_os_native_libc_get(host->get_service,
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_libc_api_v1_t, parse_double)),
        .fs = solar_os_native_fs_get(host->get_service, SOLAR_OS_NATIVE_THROUGH(solar_os_native_fs_api_v1_t, read_at)),
        .graphics = solar_os_native_gfx_get(host->get_service,
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_gfx_api_v1_t, present)),
        .display = solar_os_native_display_get(host->get_service,
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_display_api_v1_t, get_theme)),
        .audio = solar_os_native_audio_get(host->get_service,
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_audio_api_v1_t, cancel_tone)),
        .jet = solar_os_native_jet_get(host->get_service, sizeof(solar_os_native_jet_api_v1_t)),
        .voice = solar_os_native_voice_get(host->get_service, sizeof(solar_os_native_voice_api_v1_t)),
    };
    if (!lua_runtime.foreground || !lua_runtime.memory || !lua_runtime.libc || !lua_runtime.fs) {
        static const char message[] = "Lua requires a compatible SolarOS firmware.\n";
        solar_os_lua_write(message, sizeof(message) - 1U);
        return 1;
    }

    lua_State *L = lua_newstate(solar_os_lua_allocate, NULL);
    if (!L) {
        solar_os_lua_write("Lua: out of memory\n", 19);
        return 1;
    }
    lua_sethook(L, lua_checkpoint, LUA_MASKCOUNT, 10000);
    lua_pushcfunction(L, lua_bootstrap);
    int result = lua_pcall(L, 0, 0, 0);
    if (result == LUA_OK) {
        if (argc > 1) result = lua_script(L, argc, argv);
        else lua_repl(L);
    }
    if (result != LUA_OK) lua_error_output(L);
    lua_close(L);

    if (lua_runtime.tone) (void)lua_runtime.audio->cancel_tone(lua_runtime.tone);
    if (lua_runtime.synth) (void)lua_runtime.voice->close(lua_runtime.synth);
    if (lua_runtime.scene) {
        while (lua_runtime.jet->close(lua_runtime.scene) != SOLAR_OS_NATIVE_OK) {
            lua_runtime.foreground->sleep_ms(10);
        }
        (void)lua_runtime.foreground->console_gfx_release();
    }
    if (lua_runtime.surface) (void)lua_runtime.foreground->console_gfx_release();

    int status = lua_runtime.exiting ? lua_runtime.status : result != LUA_OK;
    lua_runtime.foreground->set_exit_code(status);
    return status;
}
