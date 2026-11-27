#include "solar_os_native_mjpeg.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_IMAGE
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_mjpeg.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_mjpeg_abi.h"
#include "solar_os_native_support.h"

#define MJPEG_KIND SOLAR_OS_NATIVE_HANDLE_MJPEG

static int parser_destroy(void *parser)
{
    solar_os_memory_free(parser);
    return SOLAR_OS_NATIVE_OK;
}

static int parser_open(uint8_t *buffer, size_t capacity, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!buffer || capacity < 4) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_mjpeg_parser_t *parser = solar_os_memory_alloc(sizeof(*parser),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "mjpeg.parser");
    if (!parser) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    int result = solar_os_native_result(solar_os_mjpeg_parser_init(parser, buffer, capacity));
    if (!result) {
        const solar_os_app_t *owner = solar_os_application_descriptor();
        result = owner ? solar_os_native_handle_create_owned(MJPEG_KIND, parser, parser_destroy, owner, out) :
            solar_os_native_handle_create(MJPEG_KIND, parser, parser_destroy, out);
    }
    if (result) solar_os_memory_free(parser);
    return result;
}

typedef struct {
    solar_os_native_mjpeg_frame_fn visit;
    void *user;
    int result;
} frame_visitor_t;

static esp_err_t visit_frame(const uint8_t *jpeg, size_t length, void *opaque)
{
    frame_visitor_t *visitor = opaque;
    visitor->result = visitor->visit(jpeg, length, visitor->user);
    return visitor->result ? ESP_FAIL : ESP_OK;
}

static int parser_feed(solar_os_native_handle_t id, const uint8_t *data, size_t length,
    solar_os_native_mjpeg_frame_fn visit, void *user, size_t *consumed)
{
    if (!consumed) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *consumed = 0;
    if (!visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *parser;
    int result = solar_os_native_handle_acquire_exclusive(id, MJPEG_KIND, &parser);
    if (result) return result;
    frame_visitor_t visitor = {.visit = visit, .user = user};
    const esp_err_t error = solar_os_mjpeg_parser_feed(parser, data, length, visit_frame, &visitor, consumed);
    solar_os_native_handle_release_exclusive(id);
    return visitor.result ? visitor.result : solar_os_native_result(error);
}

static int parser_reset(solar_os_native_handle_t id)
{
    void *parser;
    const int result = solar_os_native_handle_acquire_exclusive(id, MJPEG_KIND, &parser);
    if (result) return result;
    solar_os_mjpeg_parser_reset(parser);
    solar_os_native_handle_release_exclusive(id);
    return SOLAR_OS_NATIVE_OK;
}

static int parser_status(solar_os_native_handle_t id, solar_os_native_mjpeg_status_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_mjpeg_status_v1_t){0};
    void *state;
    const int result = solar_os_native_handle_acquire_exclusive(id, MJPEG_KIND, &state);
    if (result) return result;
    const solar_os_mjpeg_parser_t *parser = state;
    out->frames = parser->frames;
    out->dropped_frames = parser->dropped_frames;
    solar_os_native_handle_release_exclusive(id);
    return SOLAR_OS_NATIVE_OK;
}

static int parser_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, MJPEG_KIND); }

static const solar_os_native_mjpeg_api_v1_t mjpeg_api = {
    .abi_version = SOLAR_OS_NATIVE_MJPEG_ABI, .struct_size = sizeof(mjpeg_api),
    .open = parser_open, .feed = parser_feed, .reset = parser_reset,
    .status = parser_status, .close = parser_close,
};
#endif

const void *solar_os_native_mjpeg_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_mjpeg_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
