#include "solar_os_native_document.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_document_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_SERVICE_DOCUMENT
#include "solar_os.h"
#include "solar_os_doc.h"
#include "solar_os_html.h"
#include "solar_os_memory.h"
#include "solar_os_native_document_state.h"

static int document_destroy(void *opaque)
{
    solar_os_native_document_state_t *state = opaque;
    solar_os_doc_free(&state->document);
    solar_os_memory_free(opaque);
    return SOLAR_OS_NATIVE_OK;
}

static int document_create(solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_native_document_state_t *state = solar_os_memory_alloc(
        sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.document");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    solar_os_doc_init(&state->document);
    state->generation = 0;
    const uint32_t kind = SOLAR_OS_NATIVE_HANDLE_DOCUMENT;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const int result = owner ? solar_os_native_handle_create_owned(kind, state, document_destroy, owner, out) :
        solar_os_native_handle_create(kind, state, document_destroy, out);
    if (result) document_destroy(state);
    return result;
}

static int document_close(solar_os_native_handle_t id)
{
    return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_DOCUMENT);
}

static bool document_path_valid(const char *path)
{
    if (!path) return true;
    size_t length = 0;
    while (length < SOLAR_OS_DOCUMENT_PATH_MAX && path[length]) ++length;
    return length < SOLAR_OS_DOCUMENT_PATH_MAX;
}

static int document_replace(solar_os_native_handle_t id, solar_os_native_document_state_t *state,
                            solar_os_doc_t *next, esp_err_t error)
{
    if (error == ESP_OK) {
        solar_os_doc_set_asset_provider(next, NULL);
        solar_os_doc_free(&state->document);
        state->document = *next;
        state->generation++;
    } else {
        solar_os_doc_free(next);
    }
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static esp_err_t document_asset_read(void *user, const char *path, const char *target,
                                     uint8_t **data, size_t *bytes)
{
    const solar_os_native_document_assets_v1_t *assets = user;
    const uint8_t *value = NULL;
    int result = assets->read(assets->user, path, target, &value, bytes);
    if (result == SOLAR_OS_NATIVE_OK) *data = (uint8_t *)value;
    return solar_os_native_callback_result(result);
}

static void document_asset_release(void *user, uint8_t *data)
{
    const solar_os_native_document_assets_v1_t *assets = user;
    if (assets->release) assets->release(assets->user, data);
}

static solar_os_doc_asset_provider_t document_assets(const solar_os_native_document_assets_v1_t *assets)
{
    return assets ? (solar_os_doc_asset_provider_t){document_asset_read, document_asset_release, (void *)assets} :
        (solar_os_doc_asset_provider_t){0};
}

static int document_parse_with_assets(solar_os_native_handle_t id, uint32_t format, const char *source,
                                      size_t bytes, const char *path,
                                      const solar_os_native_document_assets_v1_t *assets)
{
    if ((!source && bytes) || format > SOLAR_OS_DOCUMENT_HTML || (assets && !assets->read))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (bytes > SOLAR_OS_DOCUMENT_MAX_BYTES || !document_path_valid(path))
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    solar_os_native_document_state_t *state = solar_os_native_document_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_doc_t next = {.assets = document_assets(assets)};
    esp_err_t error;
    switch (format) {
    case SOLAR_OS_DOCUMENT_TEXT: error = solar_os_doc_parse_text(&next, source, bytes, path); break;
    case SOLAR_OS_DOCUMENT_MARKDOWN: error = solar_os_doc_parse_markdown(&next, source, bytes, path); break;
    default: error = solar_os_html_parse_doc(&next, source, bytes, path); break;
    }
    return document_replace(id, state, &next, error);
}

static int document_parse(solar_os_native_handle_t id, uint32_t format, const char *source,
                          size_t bytes, const char *path)
{
    return document_parse_with_assets(id, format, source, bytes, path, NULL);
}

static int document_load_with_assets(solar_os_native_handle_t id, uint32_t format, const char *path,
                                      const solar_os_native_document_assets_v1_t *assets)
{
    if (!path || !*path || format > SOLAR_OS_DOCUMENT_MARKDOWN || (assets && !assets->read))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!document_path_valid(path)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    solar_os_native_document_state_t *state = solar_os_native_document_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_doc_t next = {.assets = document_assets(assets)};
    return document_replace(id, state, &next,
        solar_os_doc_load_path_as(&next, path, format == SOLAR_OS_DOCUMENT_MARKDOWN));
}

static int document_load(solar_os_native_handle_t id, uint32_t format, const char *path)
{
    return document_load_with_assets(id, format, path, NULL);
}

static int document_info(solar_os_native_handle_t id, solar_os_native_document_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_document_info_v1_t){0};
    const solar_os_native_document_state_t *state = solar_os_native_document_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const solar_os_doc_t *doc = &state->document;
    _Static_assert(sizeof(doc->path) == sizeof(out->path), "document path capacity");
    memcpy(out->path, doc->path, sizeof(out->path));
    out->source_bytes = doc->source_len;
    out->block_count = doc->block_count;
    out->run_count = doc->run_count;
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int document_block(solar_os_native_handle_t id, size_t index,
                          solar_os_native_document_block_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_document_block_v1_t){0};
    const solar_os_native_document_state_t *state = solar_os_native_document_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const solar_os_doc_t *doc = &state->document;
    const bool found = index < doc->block_count;
    if (found) {
        const solar_os_doc_block_t *block = &doc->blocks[index];
        *out = (solar_os_native_document_block_v1_t){
            .type = block->type, .level = block->level, .list_delimiter = block->list_delimiter,
            .list_number = block->list_number, .raw_source_start = block->raw_source_start,
            .raw_source_end = block->raw_source_end, .source_start = block->source_start,
            .source_end = block->source_end, .run_start = block->run_start, .run_count = block->run_count,
            .text_bytes = block->text ? strlen(block->text) : 0,
            .image_width = block->image.width, .image_height = block->image.height,
            .image_attempted = block->image.attempted, .image_error = solar_os_native_result(block->image.status),
        };
    }
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int document_run(solar_os_native_handle_t id, size_t index, solar_os_doc_run_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_doc_run_t){0};
    const solar_os_native_document_state_t *state = solar_os_native_document_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const solar_os_doc_t *doc = &state->document;
    const bool found = index < doc->run_count;
    if (found) *out = doc->runs[index];
    solar_os_native_handle_release(id);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int document_read(solar_os_native_handle_t id, bool source, size_t index, size_t offset,
                         char *buffer, size_t capacity, size_t *read)
{
    if (!read) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *read = 0;
    if (capacity && !buffer) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_native_document_state_t *state = solar_os_native_document_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const solar_os_doc_t *doc = &state->document;
    int result = SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (source || index < doc->block_count) {
        const char *text = source ? doc->source : doc->blocks[index].text;
        const size_t size = source ? doc->source_len : text ? strlen(text) : 0;
        result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if (offset <= size) {
            *read = capacity < size - offset ? capacity : size - offset;
            if (*read) memcpy(buffer, text + offset, *read);
            result = SOLAR_OS_NATIVE_OK;
        }
    }
    solar_os_native_handle_release(id);
    return result;
}

static int document_read_source(solar_os_native_handle_t id, size_t offset,
                                char *buffer, size_t capacity, size_t *read)
{
    return document_read(id, true, 0, offset, buffer, capacity, read);
}

static int document_read_block(solar_os_native_handle_t id, size_t index, size_t offset,
                               char *buffer, size_t capacity, size_t *read)
{
    return document_read(id, false, index, offset, buffer, capacity, read);
}

static int document_write_markdown(solar_os_native_handle_t id, const char *path)
{
    if (!document_path_valid(path)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    const solar_os_native_document_state_t *state = solar_os_native_document_acquire(id);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const solar_os_doc_t *doc = &state->document;
    const esp_err_t error = solar_os_doc_write_markdown(doc, path);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int html_to_markdown(const char *source, size_t bytes, const char *asset_base,
                            char **out, size_t *written)
{
    if (out) *out = NULL;
    if (written) *written = 0;
    if (bytes > SOLAR_OS_DOCUMENT_MAX_BYTES) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    return solar_os_native_result(solar_os_html_to_markdown(source, bytes, asset_base, out, written));
}

static const solar_os_native_document_api_v1_t document_api = {
    .abi_version = SOLAR_OS_NATIVE_DOCUMENT_ABI,
    .struct_size = sizeof(document_api),
    .create = document_create,
    .close = document_close,
    .parse = document_parse,
    .load = document_load,
    .info = document_info,
    .block = document_block,
    .run = document_run,
    .read_source = document_read_source,
    .read_block = document_read_block,
    .write_markdown = document_write_markdown,
    .html_to_markdown = html_to_markdown,
    .parse_with_assets = document_parse_with_assets,
    .load_with_assets = document_load_with_assets,
};
#endif

const void *solar_os_native_document_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_document_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
