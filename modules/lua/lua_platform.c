#include "lua_runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void solar_os_lua_write(const char *text, size_t length)
{
    (void)lua_runtime.host->write_utf8(text, length);
}

void solar_os_lua_warning(const char *format, const char *text)
{
    char buffer[192];
    int length = snprintf(buffer, sizeof(buffer), format, text);
    if (length > 0) {
        size_t available = sizeof(buffer) - 1U;
        solar_os_lua_write(buffer, (size_t)length < available ? (size_t)length : available);
    }
}

void *solar_os_lua_allocate(void *user, void *ptr, size_t old_size, size_t bytes)
{
    (void)user;
    (void)old_size;
    return lua_runtime.memory->realloc_class(ptr, bytes, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "lua");
}

/* Lua's enabled libraries use these C operations. Dispatch stateful calls
 * through the host without importing the host's newlib state or stdio. */
int snprintf(char *buffer, size_t capacity, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int result = lua_runtime.libc->format(buffer, capacity, format, args);
    va_end(args);
    return result;
}

double strtod(const char *text, char **end)
{
    return lua_runtime.libc->parse_double(text, end);
}

int strcoll(const char *left, const char *right)
{
    return strcmp(left, right);
}

int *__errno(void)
{
    static int error;
    return &error;
}

struct _reent *__getreent(void)
{
    /* debug.debug reads through the console hook below and ignores FILE. */
    static struct _reent state;
    return &state;
}

clock_t clock(void)
{
    return (clock_t)lua_runtime.foreground->time_us();
}

time_t time(time_t *destination)
{
    const time_t seconds = (time_t)(lua_runtime.foreground->time_us() / 1000000U);
    if (destination) *destination = seconds;
    return seconds;
}

void *realloc(void *pointer, size_t bytes)
{
    return solar_os_lua_allocate(NULL, pointer, 0, bytes);
}

void free(void *pointer)
{
    if (pointer) lua_runtime.memory->free(pointer);
}

void abort(void)
{
    __builtin_trap();
}

char *solar_os_lua_console_fgets(char *buffer, int capacity, FILE *unused)
{
    (void)unused;
    if (capacity < 2) return NULL;
    int length = 0;
    while (length < capacity - 1 && !solar_os_lua_stopping()) {
        int key = solar_os_lua_read_key(25);
        if (key == '\r' || key == '\n') {
            buffer[length++] = '\n';
            solar_os_lua_write("\n", 1);
            break;
        }
        if (key == 8 || key == 127) {
            if (length > 0) {
                do { --length; } while (length && ((uint8_t)buffer[length] & 0xC0U) == 0x80U);
                solar_os_lua_write("\b \b", 3);
            }
        } else if (key >= 32 && key <= 255) {
            buffer[length] = (char)key;
            solar_os_lua_write(&buffer[length++], 1);
        }
    }
    buffer[length] = '\0';
    return length || !solar_os_lua_stopping() ? buffer : NULL;
}

typedef struct {
    const char *path;
    uint64_t offset, length;
    int error;
    uint8_t buffer[1024];
} lua_source_t;

static size_t read_source(lua_source_t *source, size_t capacity)
{
    size_t count = 0;
    if (source->error || source->offset >= source->length) return 0;
    uint64_t remaining = source->length - source->offset;
    if (capacity > remaining) capacity = (size_t)remaining;
    if (solar_os_lua_stopping()) {
        source->error = SOLAR_OS_NATIVE_ERROR_CANCELLED;
        return 0;
    }
    source->error = lua_runtime.fs->read_at(source->path, source->offset,
                                           source->buffer, capacity, &count);
    if (!source->error && count != capacity) source->error = SOLAR_OS_NATIVE_ERROR_IO;
    source->offset += count;
    return source->error ? 0 : count;
}

static const char *lua_source_reader(lua_State *L, void *user, size_t *length)
{
    (void)L;
    lua_source_t *source = user;
    *length = read_source(source, sizeof(source->buffer));
    return *length ? (const char *)source->buffer : NULL;
}

int luaL_loadfilex(lua_State *L, const char *name, const char *mode)
{
    char path[SOLAR_OS_NATIVE_FS_PATH_MAX];
    if (!name || lua_runtime.foreground->resolve_path(name, path, sizeof(path))) {
        lua_pushfstring(L, "Lua source unavailable: %s", name ? name : "stdin");
        return LUA_ERRFILE;
    }
    solar_os_native_fs_metadata_v1_t metadata;
    if (lua_runtime.fs->stat(path, &metadata) || metadata.type != SOLAR_OS_NATIVE_FS_FILE) {
        lua_pushfstring(L, "cannot open Lua source: %s", path);
        return LUA_ERRFILE;
    }

    lua_source_t source = {.path = path, .length = metadata.bytes};
    const size_t prefix = read_source(&source, 3);
    source.offset = prefix == 3 && !memcmp(source.buffer, "\xef\xbb\xbf", 3) ? 3U : 0U;
    if (read_source(&source, 1) && source.buffer[0] == '#') {
        /* Leave the line break for the compiler's source line numbering. */
        size_t count;
        while ((count = read_source(&source, sizeof(source.buffer))) != 0) {
            const uint8_t *newline = memchr(source.buffer, '\n', count);
            if (newline) {
                source.offset -= count - (size_t)(newline - source.buffer);
                break;
            }
        }
    } else if (source.offset) {
        --source.offset;
    }
    lua_pushfstring(L, "@%s", path);
    int status = lua_load(L, lua_source_reader, &source, lua_tostring(L, -1), mode ? mode : "t");
    lua_remove(L, -2);
    if (source.error) {
        lua_pop(L, 1);
        lua_pushfstring(L, "cannot read Lua source: %s", path);
        return LUA_ERRFILE;
    }
    return status;
}
