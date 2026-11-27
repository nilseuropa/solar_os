#include "solar_os_native_diagnostics.h"
#include "solar_os_native_diagnostics_abi.h"
#include "solar_os_native_support.h"
#include "solar_os_memory.h"
#include "solar_os_config.h"
#include <string.h>
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_clipboard.h"
#include "solar_os_log.h"
static int clipboard_set(const void *bytes, size_t size) { return solar_os_native_result(solar_os_clipboard_set(bytes, size)); }
static int clipboard_read(uint64_t offset, void *bytes, size_t capacity, size_t *size, uint64_t *total)
{
    if (!size || !total) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *size = 0; *total = 0; size_t length = 0;
    _Static_assert(SOLAR_OS_NATIVE_CLIPBOARD_MAX_BYTES == SOLAR_OS_CLIPBOARD_MAX_BYTES, "clipboard limit changed");
    size_t position = offset > SOLAR_OS_NATIVE_CLIPBOARD_MAX_BYTES ? SOLAR_OS_NATIVE_CLIPBOARD_MAX_BYTES : (size_t)offset;
    int result = solar_os_native_result(solar_os_clipboard_read(position, bytes, capacity, size, &length));
    *total = length; return result;
}
static uint64_t clipboard_size(void) { return solar_os_clipboard_size(); }
static const solar_os_native_clipboard_api_v1_t clipboard_api = {
    .abi_version = SOLAR_OS_NATIVE_CLIPBOARD_ABI, .struct_size = sizeof(clipboard_api),
    .set = clipboard_set, .read = clipboard_read, .size = clipboard_size, .clear = solar_os_clipboard_clear,
};
static int level_in(uint32_t in, solar_os_log_level_t *out)
{
    switch (in) {
    case SOLAR_OS_NATIVE_LOG_ERROR: *out = SOLAR_OS_LOG_LEVEL_ERROR; break;
    case SOLAR_OS_NATIVE_LOG_WARN: *out = SOLAR_OS_LOG_LEVEL_WARN; break;
    case SOLAR_OS_NATIVE_LOG_INFO: *out = SOLAR_OS_LOG_LEVEL_INFO; break;
    case SOLAR_OS_NATIVE_LOG_DEBUG: *out = SOLAR_OS_LOG_LEVEL_DEBUG; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    return SOLAR_OS_NATIVE_OK;
}
static int level_out(solar_os_log_level_t in, uint32_t *out)
{
    switch (in) {
    case SOLAR_OS_LOG_LEVEL_ERROR: *out = SOLAR_OS_NATIVE_LOG_ERROR; break;
    case SOLAR_OS_LOG_LEVEL_WARN: *out = SOLAR_OS_NATIVE_LOG_WARN; break;
    case SOLAR_OS_LOG_LEVEL_INFO: *out = SOLAR_OS_NATIVE_LOG_INFO; break;
    case SOLAR_OS_LOG_LEVEL_DEBUG: *out = SOLAR_OS_NATIVE_LOG_DEBUG; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    return SOLAR_OS_NATIVE_OK;
}
static int log_clear(void) { return solar_os_native_result(solar_os_log_clear()); }
static int log_set_level(uint32_t in)
{ solar_os_log_level_t value; int result = level_in(in, &value); return result ? result : solar_os_native_result(solar_os_log_set_level(value)); }
static int log_set_cdc(bool enabled) { return solar_os_native_result(solar_os_log_set_cdc_enabled(enabled)); }
static int log_status(solar_os_native_log_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_log_status_t value = {0}; int result = solar_os_native_result(solar_os_log_get_status(&value));
    if (!result) {
        out->capacity = value.capacity; out->count = value.count; out->bytes = value.bytes; out->initialized = value.initialized;
        out->cdc_enabled = value.cdc_enabled; out->ring_in_psram = value.ring_in_psram; out->dropped = value.dropped; result = level_out(value.level, &out->level);
    }
    return result;
}
static int log_snapshot_common(bool since, uint32_t after, uint32_t threshold, solar_os_native_log_entry_v1_t *out,
    size_t capacity, size_t *copied, size_t *total)
{
    if (!copied || !total || (!out && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *copied = 0; *total = 0; solar_os_log_level_t level; int result = level_in(threshold, &level); if (result) return result;
    if (capacity > SOLAR_OS_LOG_CAPACITY) capacity = SOLAR_OS_LOG_CAPACITY;
    solar_os_log_entry_t *values = capacity ? solar_os_memory_calloc(capacity, sizeof(*values), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.log") : NULL;
    if (capacity && !values) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    size_t count = since ? solar_os_log_snapshot_since(after, level, values, capacity, total) : solar_os_log_snapshot(values, capacity, total);
    if (count > capacity) result = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    else for (size_t i = 0; i < count && !result; ++i) {
        memset(&out[i], 0, sizeof(out[i])); result = level_out(values[i].level, &out[i].level); if (result) break;
        out[i].sequence = values[i].sequence; out[i].timestamp_ms = values[i].timestamp_ms; out[i].truncated = values[i].truncated;
        _Static_assert(sizeof(out[i].tag) == sizeof(values[i].tag), "log tag capacity");
        _Static_assert(sizeof(out[i].message) == sizeof(values[i].message), "log message capacity");
        memcpy(out[i].tag, values[i].tag, sizeof(out[i].tag)); memcpy(out[i].message, values[i].message, sizeof(out[i].message)); ++*copied;
    }
    solar_os_memory_free(values); return result;
}
static int log_snapshot(solar_os_native_log_entry_v1_t *out, size_t capacity, size_t *copied, size_t *total)
{ return log_snapshot_common(false, 0, SOLAR_OS_NATIVE_LOG_DEBUG, out, capacity, copied, total); }
static int log_snapshot_since(uint32_t after, uint32_t threshold, solar_os_native_log_entry_v1_t *out, size_t capacity, size_t *copied, size_t *total)
{ return log_snapshot_common(true, after, threshold, out, capacity, copied, total); }
static int log_vwrite(uint32_t in, const char *tag, const char *format, va_list args)
{ solar_os_log_level_t level; int result = level_in(in, &level); return result ? result : solar_os_native_result(solar_os_log_vwrite(level, tag, format, args)); }
static int log_write(uint32_t in, const char *tag, const char *format, ...)
{ va_list args; va_start(args, format); int result = log_vwrite(in, tag, format, args); va_end(args); return result; }
static int log_text(uint32_t in, const char *tag, const char *text)
{ return text ? log_write(in, tag, "%s", text) : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; }
static int log_hex(uint32_t in, const char *tag, const void *bytes, size_t size)
{ solar_os_log_level_t level; int result = level_in(in, &level); return result ? result : solar_os_native_result(solar_os_log_buffer_hex(level, tag, bytes, size)); }
static const char *log_level_name(uint32_t in)
{ solar_os_log_level_t level; return level_in(in, &level) ? NULL : solar_os_log_level_name(level); }
static int log_parse_level(const char *text, uint32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; solar_os_log_level_t value;
    return solar_os_log_parse_level(text, &value) ? level_out(value, out) : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
}
static const solar_os_native_log_api_v1_t log_api = {
    .abi_version = SOLAR_OS_NATIVE_LOG_ABI, .struct_size = sizeof(log_api), .clear = log_clear, .set_level = log_set_level, .set_cdc_enabled = log_set_cdc,
    .status = log_status, .snapshot = log_snapshot, .snapshot_since = log_snapshot_since, .text = log_text, .write = log_write, .vwrite = log_vwrite,
    .buffer_hex = log_hex, .level_name = log_level_name, .parse_level = log_parse_level,
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ENGINES
#include "solar_os_engines.h"

static int engine_measure(const char *name, const char *owner, const char *label,
                          solar_os_native_engine_work_fn work, void *user)
{
    if (!work) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_engine_token_t token;
    int result = solar_os_native_result(solar_os_engine_begin(name, owner, label, &token));
    if (result) return result;
    uint64_t units = 0;
    result = work(user, &units);
    const int finish = solar_os_native_result(solar_os_engine_end(&token, units));
    return result ? result : finish;
}

static int engine_record(const char *name, const char *owner, const char *label,
                         uint64_t duration_us, uint64_t units)
{
    return solar_os_native_result(solar_os_engine_record(name, owner, label, duration_us, units));
}

static const solar_os_native_engines_api_v1_t engines_api = {
    .abi_version = SOLAR_OS_NATIVE_ENGINES_ABI, .struct_size = sizeof(engines_api),
    .count = solar_os_engine_count, .info = solar_os_engine_get, .find = solar_os_engine_get_by_name,
    .measure = engine_measure, .record = engine_record, .reset = solar_os_engine_reset_all,
};
#endif

const void *solar_os_native_diagnostics_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_diagnostics_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
