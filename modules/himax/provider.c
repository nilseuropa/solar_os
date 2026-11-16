#include "solar_os_processor_provider_abi.h"
#include "solar_os_native_sscma_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_native_time_abi.h"
#include "solar_os_json_scan.h"
#include "manager.h"
#include <stdarg.h>
#include <string.h>

typedef struct {
    const solar_os_native_memory_api_v1_t *memory;
    const solar_os_native_sscma_api_v1_t *camera;
    const solar_os_native_libc_api_v1_t *libc;
    const solar_os_native_time_api_v1_t *time;
    uint32_t token, timeout, model;
    solar_os_native_cancel_fn cancel;
    void *user;
    char source[256];
    char *frame;
} himax_t;

static int bind(solar_os_processor_service_fn services, himax_t *c)
{
    if (!services) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    c->memory = services(SOLAR_OS_NATIVE_MEMORY_SERVICE, 1, sizeof(*c->memory));
    c->camera = services(SOLAR_OS_NATIVE_SSCMA_SERVICE, 1, SOLAR_OS_NATIVE_SSCMA_BASE_SIZE);
    c->libc = services(SOLAR_OS_NATIVE_LIBC_SERVICE, 1, sizeof(*c->libc));
    c->time = services(SOLAR_OS_NATIVE_TIME_SERVICE, 1, sizeof(*c->time));
    return c->memory && c->camera && c->libc && c->time ? 0 : SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
}
static int format(himax_t *c, char *buffer, size_t size, const char *fmt, ...)
{
    va_list args; va_start(args, fmt);
    int n = c->libc->format(buffer, size, fmt, args);
    va_end(args);
    return n;
}
static int exchange(himax_t *c, const char *command, const char *name, bool event, char **reply)
{
    return c->camera->exchange(c->token, c->source, command, name, event,
        c->timeout, c->cancel, c->user, reply);
}
static void release_frame(void *client)
{
    himax_t *c = client;
    if (c) { c->memory->free(c->frame); c->frame = NULL; }
}
static int close_client(void *client)
{
    himax_t *c = client;
    if (!c) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    release_frame(c);
    int result = c->token ? c->camera->close(c->token) : 0;
    if (!result) c->memory->free(c);
    return result;
}
static void close_query(himax_t *c)
{
    while (c->camera->close(c->token)) c->time->sleep_ms(20);
}
static int query(solar_os_processor_service_fn services, const char *source,
    const char *what, uint32_t timeout, char **out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    if (!source || !*source || strlen(source) >= 256 || !what || !timeout || timeout > 60000)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    static const char *const names[] = {"VER?", "MODELS?", "MODEL?", "ALGOS?", "ID?", "INFO?"};
    static const char *const keys[] = {"version", "models", "model", "algorithms", "identity", "metadata"};
    size_t first = 0, last = 4;
    if (strcmp(what, "info")) {
        for (first = 0; first < 6 && strcmp(what, keys[first]); ++first) {}
        if (first == 6) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        last = first + 1;
    }
    himax_t c = {.timeout = timeout};
    int error = bind(services, &c);
    if (error) return error;
    memcpy(c.source, source, strlen(source) + 1);
    error = c.camera->open(source, "processor-info", &c.token);
    if (error) return error;
    char *json = c.memory->alloc(SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX);
    if (!json) error = SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    size_t used = 0;
    if (!error) used = (size_t)format(&c, json, SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX,
        "{\"kind\":\"himax\",\"input\":\"camera\",\"external_frames\":false,"
        "\"provider_version\":\"" SOLAR_OS_HIMAX_PROVIDER_VERSION "\",\"capabilities\":[\"capture\",\"model-select\",\"model-upload\"]");
    for (size_t i = first; !error && i < last; ++i) {
        char command[32], *reply = NULL;
        format(&c, command, sizeof(command), "AT+%s\r\n", names[i]);
        error = exchange(&c, command, names[i], false, &reply);
        const char *data; size_t length;
        if (!error && !solar_os_json_scan_object_value(reply, "data", &data, &length))
            error = SOLAR_OS_NATIVE_ERROR_FAILED;
        if (!error) {
            int n = format(&c, json + used, SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX - used,
                ",\"%s\":%.*s", keys[i], (int)length, data);
            if (n < 0 || (size_t)n >= SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX - used)
                error = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            else used += (size_t)n;
        }
        c.memory->free(reply);
    }
    if (!error && used + 2 < SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX) {
        json[used++] = '}'; json[used] = 0;
        *out = json; json = NULL;
    } else if (!error) error = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    c.memory->free(json); close_query(&c);
    return error;
}
static int open_client(solar_os_processor_service_fn services, const char *source,
    uint32_t model, uint32_t timeout, solar_os_native_cancel_fn cancel, void *user, void **out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    if (!source || !*source || strlen(source) >= 256 || !timeout || timeout > 60000)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    /* Zero means current; a nonzero number is an SSCMA slot, never ESP-DL. */
    if (model > 4) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (cancel && cancel(user)) return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    himax_t services_only = {0};
    int error = bind(services, &services_only);
    if (error) return error;
    himax_t *c = services_only.memory->alloc(sizeof(*c));
    if (!c) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    *c = services_only; c->timeout = timeout; c->cancel = cancel; c->user = user;
    memcpy(c->source, source, strlen(source) + 1);
    error = c->camera->open(source, "processor-himax", &c->token);
    char *reply = NULL;
    if (!error && model) {
        char command[32]; format(c, command, sizeof(command), "AT+MODEL=%lu\r\n", (unsigned long)model);
        error = exchange(c, command, "MODEL", false, &reply);
        c->memory->free(reply); reply = NULL;
    }
    if (!error) error = exchange(c, "AT+MODEL?\r\n", "MODEL?", false, &reply);
    const char *data; size_t length; uint64_t id;
    if (!error && (!solar_os_json_scan_object_value(reply, "data", &data, &length) ||
        !solar_os_json_scan_object_uint64(data, "id", &id) || id > UINT32_MAX))
        error = SOLAR_OS_NATIVE_ERROR_FAILED;
    if (!error && model && id != model) error = SOLAR_OS_NATIVE_ERROR_FAILED;
    if (!error) { c->model = (uint32_t)id; error = c->camera->start(c->token); }
    c->memory->free(reply);
    if (error) {
        if (c->token) close_query(c);
        c->memory->free(c);
    } else *out = c;
    return error;
}
static int capture(void *client, solar_os_processor_frame_v1_t *frame, char **out)
{
    if (!client || !frame || !out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = NULL; memset(frame, 0, sizeof(*frame));
    himax_t *c = client;
    if (c->frame) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    char *reply = NULL;
    int error = exchange(c, "AT+INVOKE=1,0,0\r\n", "INVOKE", true, &reply);
    char *json = NULL;
    const char *data; size_t length;
    if (!error && !solar_os_json_scan_object_value(reply, "data", &data, &length))
        error = SOLAR_OS_NATIVE_ERROR_FAILED;
    if (!error) {
        json = c->memory->alloc(SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX);
        if (!json) error = SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    }
    size_t used = 0;
    if (!error) used = (size_t)format(c, json, SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX,
        "{\"kind\":\"himax\",\"model\":%lu,\"coordinate_space\":\"sscma\"", (unsigned long)c->model);
    static const char *const fields[] = {"count", "perf", "boxes", "classes", "points"};
    for (size_t i = 0; !error && i < sizeof(fields) / sizeof(*fields); ++i) {
        const char *value;
        if (!solar_os_json_scan_object_value(data, fields[i], &value, &length)) continue;
        int n = format(c, json + used, SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX - used,
            ",\"%s\":%.*s", fields[i], (int)length, value);
        if (n < 0 || (size_t)n >= SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX - used)
            error = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        else used += (size_t)n;
    }
    if (!error && used + 2 < SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX) {
        json[used++] = '}'; json[used] = 0;
        error = c->camera->image(reply, strlen(reply) + 1, &frame->data,
            &frame->length, &frame->width, &frame->height);
    } else if (!error) error = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (!error) {
        frame->timestamp_us = c->time->now_us();
        c->frame = reply; reply = NULL;
        *out = json; json = NULL;
    }
    c->memory->free(json); c->memory->free(reply);
    return error;
}
const solar_os_processor_provider_api_v1_t *solar_os_himax_processor_api(void)
{
    static const solar_os_processor_provider_api_v1_t api = {
        .abi_version = SOLAR_OS_PROCESSOR_PROVIDER_ABI, .struct_size = sizeof(api),
        .kind = "himax", .capabilities = SOLAR_OS_PROCESSOR_PROVIDER_CAPTURE |
            SOLAR_OS_PROCESSOR_PROVIDER_MODEL_SELECT | SOLAR_OS_PROCESSOR_PROVIDER_MODEL_UPLOAD,
        .query = query, .open = open_client, .capture = capture,
        .release_frame = release_frame, .close = close_client,
        .select = solar_os_himax_model_select, .upload = solar_os_himax_model_upload,
    };
    return &api;
}
