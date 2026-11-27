#include "solar_os_native_manual.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_manual_abi.h"
#include "solar_os_native_support.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_manual.h"

static size_t manual_page_count(bool bundled)
{
    return bundled ? solar_os_manual_embedded_count() : solar_os_manual_count();
}

static bool manual_visit(const solar_os_manual_page_t *page, solar_os_native_manual_page_fn visit, void *user)
{
    const solar_os_native_manual_page_v1_t record = {
        .id = page->id, .title = page->title, .section = page->section, .section_title = page->section_title,
        .summary = page->summary, .aliases = page->aliases, .keywords = page->keywords,
    };
    return visit(&record, user);
}

static int manual_page_info(bool bundled, size_t index, solar_os_native_manual_page_fn visit, void *user)
{
    if (!visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_manual_catalog_t catalog;
    solar_os_manual_catalog_acquire(bundled, &catalog);
    const bool found = index < catalog.count;
    if (found) (void)manual_visit(&catalog.pages[index], visit, user);
    solar_os_manual_catalog_release(&catalog);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int manual_find_page(bool bundled, const char *name, solar_os_native_manual_page_fn visit, void *user)
{
    if (!name || !*name || !visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_manual_catalog_t catalog;
    solar_os_manual_catalog_acquire(bundled, &catalog);
    const solar_os_manual_page_t *page = solar_os_manual_catalog_find(&catalog, name);
    if (page) (void)manual_visit(page, visit, user);
    const bool found = page != NULL;
    solar_os_manual_catalog_release(&catalog);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int manual_search_pages(bool bundled, const char *query, size_t limit,
                               solar_os_native_manual_page_fn visit, void *user, size_t *visited)
{
    if (!visited) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *visited = 0;
    if (!query || !*query || !visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_manual_catalog_t catalog;
    solar_os_manual_catalog_acquire(bundled, &catalog);
    const solar_os_manual_page_t *matches[SOLAR_OS_NATIVE_MANUAL_SEARCH_MAX];
    const size_t count = solar_os_manual_catalog_search(&catalog, query, matches, limit);
    for (size_t i = 0; i < count; i++) {
        (*visited)++;
        if (!manual_visit(matches[i], visit, user)) break;
    }
    solar_os_manual_catalog_release(&catalog);
    return SOLAR_OS_NATIVE_OK;
}

static esp_err_t manual_load_page(const solar_os_manual_page_t *page, bool bundled, uint32_t format,
                                 const char **text, size_t *size, bool *owned)
{
    if (!page) return ESP_ERR_NOT_FOUND;
    if (bundled) {
        switch (format) {
        case SOLAR_OS_NATIVE_MANUAL_BODY: *text = page->body; break;
        case SOLAR_OS_NATIVE_MANUAL_QUICK_REFERENCE: *text = page->contract; break;
        case SOLAR_OS_NATIVE_MANUAL_MARKDOWN: *text = page->markdown; break;
        default: return ESP_ERR_INVALID_ARG;
        }
        if (!*text) return ESP_ERR_NOT_FOUND;
        *size = strlen(*text);
        *owned = false;
        return ESP_OK;
    }

    switch (format) {
    case SOLAR_OS_NATIVE_MANUAL_BODY: return solar_os_manual_load_body(page, text, size, owned);
    case SOLAR_OS_NATIVE_MANUAL_QUICK_REFERENCE: return solar_os_manual_load_contract(page, text, size, owned);
    case SOLAR_OS_NATIVE_MANUAL_MARKDOWN: return solar_os_manual_load_markdown(page, text, size, owned);
    default: return ESP_ERR_INVALID_ARG;
    }
}

static void manual_copy(const char *text, size_t size, size_t offset,
                        char *buffer, size_t capacity, size_t *copied, size_t *total)
{
    *total = size;
    if (offset >= size) return;
    const size_t remaining = size - offset;
    *copied = remaining < capacity ? remaining : capacity;
    if (*copied) memcpy(buffer, text + offset, *copied);
}

static int manual_read_page(bool bundled, const char *name, uint32_t format, size_t offset,
                            char *buffer, size_t capacity, size_t *copied, size_t *total)
{
    if (!copied || !total) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *copied = *total = 0;
    if (!name || !*name || format > SOLAR_OS_NATIVE_MANUAL_MARKDOWN || (!buffer && capacity))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    solar_os_manual_catalog_t catalog;
    solar_os_manual_catalog_acquire(bundled, &catalog);
    const solar_os_manual_page_t *page = solar_os_manual_catalog_find(&catalog, name);
    const char *text = NULL;
    size_t size = 0;
    bool owned = false;
    const esp_err_t error = manual_load_page(page, bundled, format, &text, &size, &owned);
    if (error == ESP_OK) manual_copy(text, size, offset, buffer, capacity, copied, total);
    solar_os_manual_release_text(text, owned);
    solar_os_manual_catalog_release(&catalog);
    return solar_os_native_result(error);
}

static int manual_reference_info(size_t index, solar_os_native_manual_reference_fn visit, void *user)
{
    if (!visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const solar_os_manual_reference_t *entry = solar_os_manual_reference_get(index);
    if (!entry) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    (void)visit(entry, user);
    return SOLAR_OS_NATIVE_OK;
}

static int manual_search_references(const char *query, size_t limit,
                                    solar_os_native_manual_reference_fn visit, void *user, size_t *visited)
{
    if (!visited) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *visited = 0;
    if (!query || !*query || !visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    const solar_os_manual_reference_t *matches[SOLAR_OS_NATIVE_MANUAL_SEARCH_MAX];
    const size_t count = solar_os_manual_reference_search(query, matches, limit);
    for (size_t i = 0; i < count; i++) {
        (*visited)++;
        if (!visit(matches[i], user)) break;
    }
    return SOLAR_OS_NATIVE_OK;
}

static int manual_read_reference(const solar_os_manual_reference_t *reference, size_t offset,
                                 char *buffer, size_t capacity, size_t *copied, size_t *total)
{
    if (!copied || !total) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *copied = *total = 0;
    if (!reference || (!buffer && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const char *text = NULL;
    size_t size = 0;
    const esp_err_t error = solar_os_manual_reference_text(reference, &text, &size);
    if (error == ESP_OK) manual_copy(text, size, offset, buffer, capacity, copied, total);
    return solar_os_native_result(error);
}

static const solar_os_native_manual_api_v1_t manual_api = {
    .abi_version = SOLAR_OS_NATIVE_MANUAL_ABI,
    .struct_size = sizeof(manual_api),
    .page_count = manual_page_count,
    .page_info = manual_page_info,
    .find_page = manual_find_page,
    .search_pages = manual_search_pages,
    .read_page = manual_read_page,
    .reference_count = solar_os_manual_reference_count,
    .reference_info = manual_reference_info,
    .search_references = manual_search_references,
    .read_reference = manual_read_reference,
};
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
#include "solar_os_docs.h"
#include "solar_os_memory.h"

static int catalog_status(solar_os_docs_status_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_docs_status_t){0};
    return solar_os_native_result(solar_os_docs_get_status(out));
}

static int catalog_update(solar_os_docs_progress_fn progress, void *user)
{
    return solar_os_native_result(solar_os_docs_update(progress, user));
}

static int catalog_reset(void)
{
    return solar_os_native_result(solar_os_docs_reset());
}

static int catalog_page_info(size_t index, solar_os_native_manual_page_fn visit, void *user)
{
    if (!visit) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_manual_catalog_t catalog;
    if (!solar_os_docs_manual_snapshot_acquire(&catalog)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const bool found = index < catalog.count;
    if (found) (void)manual_visit(&catalog.pages[index], visit, user);
    solar_os_docs_manual_snapshot_release(&catalog);
    return found ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int catalog_read_source(const char *id, size_t offset, char *buffer, size_t capacity,
                               size_t *copied, size_t *total)
{
    if (!copied || !total) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *copied = *total = 0;
    if (!id || !*id || (!buffer && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    char *text = NULL;
    size_t size = 0;
    const esp_err_t error = solar_os_docs_load_page(id, &text, &size);
    if (error == ESP_OK) manual_copy(text, size, offset, buffer, capacity, copied, total);
    solar_os_memory_free(text);
    return solar_os_native_result(error);
}

static const solar_os_native_docs_api_v1_t docs_api = {
    .abi_version = SOLAR_OS_NATIVE_DOCS_ABI,
    .struct_size = sizeof(docs_api),
    .status = catalog_status,
    .update = catalog_update,
    .reset = catalog_reset,
    .available = solar_os_docs_manual_index_available,
    .page_count = solar_os_docs_manual_count,
    .page_info = catalog_page_info,
    .read_source = catalog_read_source,
};
#endif
#endif

const void *solar_os_native_manual_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_manual_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
