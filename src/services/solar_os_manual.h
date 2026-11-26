#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_manual_abi.h"

typedef struct {
    const char *id;
    const char *title;
    const char *section;
    const char *section_title;
    const char *summary;
    const char *aliases;
    const char *keywords;
    const char *body;
    const char *contract;
    const char *markdown;
} solar_os_manual_page_t;

typedef struct {
    const solar_os_manual_page_t *pages;
    size_t count;
    void *pin;
} solar_os_manual_catalog_t;

/* Retain one catalog through all uses of its borrowed pages. A NULL catalog
 * in find or search uses the current legacy view. */
void solar_os_manual_catalog_acquire(bool bundled, solar_os_manual_catalog_t *catalog);
void solar_os_manual_catalog_release(solar_os_manual_catalog_t *catalog);
const solar_os_manual_page_t *solar_os_manual_catalog_find(const solar_os_manual_catalog_t *catalog,
                                                           const char *name);
size_t solar_os_manual_catalog_search(const solar_os_manual_catalog_t *catalog, const char *query,
                                      const solar_os_manual_page_t **results, size_t capacity);

esp_err_t solar_os_manual_load_markdown(const solar_os_manual_page_t *page,
                                        const char **markdown,
                                        size_t *markdown_len,
                                        bool *owned);
esp_err_t solar_os_manual_load_body(const solar_os_manual_page_t *page,
                                    const char **body,
                                    size_t *body_len,
                                    bool *owned);
esp_err_t solar_os_manual_load_contract(const solar_os_manual_page_t *page,
                                        const char **contract,
                                        size_t *contract_len,
                                        bool *owned);
void solar_os_manual_release_text(const char *text, bool owned);

size_t solar_os_manual_count(void);
const solar_os_manual_page_t *solar_os_manual_get(size_t index);
/* Signed-catalog validation uses the firmware-bundled compatibility set. */
size_t solar_os_manual_embedded_count(void);
const solar_os_manual_page_t *solar_os_manual_embedded_get(size_t index);
const solar_os_manual_page_t *solar_os_manual_find(const char *name);
size_t solar_os_manual_search(const char *query,
                              const solar_os_manual_page_t **results,
                              size_t capacity);
size_t solar_os_manual_reference_count(void);
const solar_os_manual_reference_t *solar_os_manual_reference_get(size_t index);
esp_err_t solar_os_manual_reference_text(
    const solar_os_manual_reference_t *reference,
    const char **text,
    size_t *text_len);
size_t solar_os_manual_reference_search(
    const char *query,
    const solar_os_manual_reference_t **results,
    size_t capacity);
