#include "solar_os_native_document_layout.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_document_layout_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_DOCUMENT
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_document_state.h"

typedef struct {
    solar_os_doc_layout_t value;
    solar_os_native_document_state_t *document;
    solar_os_native_handle_t document_id;
    uint64_t generation;
    bool built;
} native_document_layout_t;

static int layout_destroy(void *opaque)
{
    native_document_layout_t *layout = opaque;
    solar_os_doc_layout_free(&layout->value);
    solar_os_native_handle_release(layout->document_id);
    solar_os_memory_free(layout);
    return SOLAR_OS_NATIVE_OK;
}

static int layout_create(solar_os_native_handle_t document_id, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_native_document_state_t *document = solar_os_native_document_acquire(document_id);
    if (!document) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    native_document_layout_t *layout = solar_os_memory_calloc(1, sizeof(*layout),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.document.layout");
    if (!layout) {
        solar_os_native_handle_release(document_id);
        return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    }
    layout->document = document;
    layout->document_id = document_id;
    solar_os_doc_layout_init(&layout->value);
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_DOCUMENT_LAYOUT;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const int result = owner ? solar_os_native_handle_create_owned(kind, layout, layout_destroy, owner, out) :
        solar_os_native_handle_create(kind, layout, layout_destroy, out);
    if (result) layout_destroy(layout);
    return result;
}

static int layout_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_DOCUMENT_LAYOUT);
}

static int layout_acquire(solar_os_native_handle_t id, native_document_layout_t **out)
{
    native_document_layout_t *layout = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DOCUMENT_LAYOUT);
    if (!layout) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (!layout->built || layout->generation != layout->document->generation) {
        solar_os_native_handle_release(id);
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    *out = layout;
    return SOLAR_OS_NATIVE_OK;
}

static int layout_rebuild(solar_os_native_handle_t id, int32_t width, int32_t zoom,
                          const solar_os_doc_reveal_range_t *reveals, size_t count)
{
    if (width <= 0 || width > INT16_MAX || (count && !reveals)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_document_layout_t *layout = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_DOCUMENT_LAYOUT);
    if (!layout) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const solar_os_doc_t *doc = &layout->document->document;
    for (size_t i = 0; i < count; i++) {
        if (reveals[i].end > doc->source_len || reveals[i].start > reveals[i].end) {
            solar_os_native_handle_release(id);
            return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        }
    }
    const esp_err_t error = solar_os_doc_layout_build_ex(&layout->value, doc, width, zoom, reveals, count);
    layout->built = error == ESP_OK;
    layout->generation = layout->document->generation;
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int layout_measure(solar_os_native_handle_t id, int32_t width, int32_t zoom, int32_t *height)
{
    if (!height) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *height = 0;
    if (width <= 0 || width > INT16_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_native_document_state_t *doc = solar_os_native_document_acquire(id);
    if (!doc) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *height = solar_os_doc_measure_height(&doc->document, width, zoom);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int layout_info(solar_os_native_handle_t id, solar_os_native_document_layout_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_document_layout_info_v1_t){0};
    native_document_layout_t *layout;
    const int result = layout_acquire(id, &layout);
    if (result) return result;
    const solar_os_doc_layout_t *value = &layout->value;
    *out = (solar_os_native_document_layout_info_v1_t){
        .width = value->width, .height = value->height, .zoom = value->zoom,
        .lines = value->line_count, .runs = value->run_count,
    };
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int layout_line(solar_os_native_handle_t id, size_t index, solar_os_doc_layout_line_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_doc_layout_line_t){0};
    native_document_layout_t *layout;
    const int result = layout_acquire(id, &layout);
    if (result) return result;
    const bool found = index < layout->value.line_count;
    if (found) *out = *solar_os_doc_layout_line(&layout->value, index);
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int layout_run(solar_os_native_handle_t id, size_t index, solar_os_doc_layout_run_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_doc_layout_run_t){0};
    native_document_layout_t *layout;
    const int result = layout_acquire(id, &layout);
    if (result) return result;
    const bool found = index < layout->value.run_count;
    if (found) *out = *solar_os_doc_layout_run(&layout->value, index);
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int layout_run_x(solar_os_native_handle_t id, size_t index, size_t offset, int32_t *x)
{
    if (!x) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *x = 0;
    native_document_layout_t *layout;
    const int result = layout_acquire(id, &layout);
    if (result) return result;
    const bool found = index < layout->value.run_count;
    if (found) *x = solar_os_doc_layout_run_source_x(&layout->value,
        solar_os_doc_layout_run(&layout->value, index), offset);
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int layout_source_position(solar_os_native_handle_t id, size_t offset,
                                  solar_os_native_document_position_v1_t *position)
{
    if (!position) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *position = (solar_os_native_document_position_v1_t){0};
    native_document_layout_t *layout;
    const int result = layout_acquire(id, &layout);
    if (result) return result;
    int x, y, height;
    const bool found = solar_os_doc_layout_source_to_xy(&layout->value, offset, &x, &y, &height);
    if (found) *position = (solar_os_native_document_position_v1_t){x, y, height};
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int layout_hit_test(solar_os_native_handle_t id, int32_t x, int32_t y, size_t *offset)
{
    if (!offset || x < INT16_MIN || x > INT16_MAX || y < 0) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *offset = 0;
    native_document_layout_t *layout;
    const int result = layout_acquire(id, &layout);
    if (result) return result;
    const bool found = solar_os_doc_layout_hit_test(&layout->value, x, y, offset);
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int layout_adjacent_source(solar_os_native_handle_t id, size_t offset, bool down, size_t *target)
{
    if (!target) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *target = 0;
    native_document_layout_t *layout;
    const int result = layout_acquire(id, &layout);
    if (result) return result;
    const bool found = solar_os_doc_layout_adjacent_source(&layout->value, offset, down, target);
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int layout_render(solar_os_native_handle_t id, solar_os_gfx_t *gfx, const solar_os_doc_view_t *view)
{
    if (!gfx || !view || view->x < INT16_MIN || view->x > INT16_MAX ||
        view->y < INT16_MIN || view->y > INT16_MAX || view->width <= 0 || view->width > INT16_MAX ||
        view->height <= 0 || view->height > INT16_MAX || view->scroll_y < 0)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_document_layout_t *layout;
    const int result = layout_acquire(id, &layout);
    if (result) return result;
    if (view->scroll_y <= layout->value.height)
        solar_os_doc_layout_render(gfx, &layout->document->document, &layout->value, view);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_document_layout_api_v1_t document_layout_api = {
    .abi_version = SOLAR_OS_NATIVE_DOCUMENT_LAYOUT_ABI,
    .struct_size = sizeof(document_layout_api),
    .create = layout_create,
    .close = layout_close,
    .rebuild = layout_rebuild,
    .measure = layout_measure,
    .info = layout_info,
    .line = layout_line,
    .run = layout_run,
    .run_source_x = layout_run_x,
    .source_position = layout_source_position,
    .hit_test = layout_hit_test,
    .adjacent_source = layout_adjacent_source,
    .render = layout_render,
};
#endif

const void *solar_os_native_document_layout_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_document_layout_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
