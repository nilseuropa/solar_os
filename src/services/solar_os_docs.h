#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "solar_os_manual.h"
#include "solar_os_native_docs_abi.h"

#define SOLAR_OS_DOCS_PAGE_MAX (128U * 1024U)
#define SOLAR_OS_DOCS_PAGE_COUNT_MAX 1024U

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
/* Pin one immutable signed index through metadata lookup or callbacks. */
bool solar_os_docs_manual_snapshot_acquire(solar_os_manual_catalog_t *catalog);
void solar_os_docs_manual_snapshot_release(solar_os_manual_catalog_t *catalog);
