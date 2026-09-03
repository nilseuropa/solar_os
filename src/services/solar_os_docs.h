#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "solar_os_manual.h"

#define SOLAR_OS_DOCS_REVISION_MAX 17U
#define SOLAR_OS_DOCS_ERROR_MAX 96U
#define SOLAR_OS_DOCS_PAGE_MAX (128U * 1024U)

typedef enum {
    SOLAR_OS_DOCS_PROGRESS_CATALOG,
    SOLAR_OS_DOCS_PROGRESS_SIGNATURE,
    SOLAR_OS_DOCS_PROGRESS_ARCHIVE,
    SOLAR_OS_DOCS_PROGRESS_EXTRACTING,
    SOLAR_OS_DOCS_PROGRESS_VERIFYING,
    SOLAR_OS_DOCS_PROGRESS_ACTIVATING,
    SOLAR_OS_DOCS_PROGRESS_DONE,
} solar_os_docs_progress_stage_t;

typedef struct {
    solar_os_docs_progress_stage_t stage;
    size_t page_index;
    size_t page_count;
    size_t bytes_read;
    size_t bytes_total;
    bool total_known;
    char topic[64];
} solar_os_docs_progress_t;

typedef void (*solar_os_docs_progress_fn)(
    const solar_os_docs_progress_t *progress,
    void *user);

typedef struct {
    bool available;
    bool updating;
    char version[32];
    char revision[SOLAR_OS_DOCS_REVISION_MAX];
    size_t page_count;
    char last_error[SOLAR_OS_DOCS_ERROR_MAX];
} solar_os_docs_status_t;

esp_err_t solar_os_docs_init(void);
esp_err_t solar_os_docs_get_status(solar_os_docs_status_t *status);
esp_err_t solar_os_docs_update(solar_os_docs_progress_fn progress, void *user);
esp_err_t solar_os_docs_reset(void);

/*
 * Loads a page from the active signed catalog and verifies its size and SHA-256
 * before returning it. The caller owns *body and releases it with
 * solar_os_memory_free().
 */
esp_err_t solar_os_docs_load_page(const char *id, char **body, size_t *body_len);

/* Runtime metadata from the active signed catalog. */
bool solar_os_docs_manual_index_available(void);
size_t solar_os_docs_manual_count(void);
const solar_os_manual_page_t *solar_os_docs_manual_get(size_t index);
