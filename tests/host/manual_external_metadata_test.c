#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_docs.h"
#include "solar_os_manual.h"
#include "solar_os_memory.h"

static bool external_active = true;

static const solar_os_manual_page_t external_pages[] = {
    {
        .id = "overview",
        .title = "Downloaded overview",
        .section = "concept",
        .section_title = "Downloaded section",
        .summary = "Downloaded manual metadata",
        .aliases = "manual",
        .keywords = "downloaded external metadata",
        .body = "embedded body",
        .contract = "Downloaded overview reference",
    },
    {
        .id = "command.schedule",
        .title = "Downloaded schedule command",
        .section = "command",
        .section_title = "Commands",
        .summary = "Fresh schedule routing metadata",
        .aliases = "schedule",
        .keywords = "fresh scheduler command",
        .body = "embedded body",
        .contract = "Downloaded schedule reference",
    },
};

bool solar_os_docs_manual_index_available(void)
{
    return external_active;
}

size_t solar_os_docs_manual_count(void)
{
    return external_active ?
        sizeof(external_pages) / sizeof(external_pages[0]) : 0U;
}

const solar_os_manual_page_t *solar_os_docs_manual_get(size_t index)
{
    return external_active && index < solar_os_docs_manual_count() ?
        &external_pages[index] : NULL;
}

esp_err_t solar_os_docs_load_page(const char *id, char **body, size_t *body_len)
{
    (void)id;
    (void)body;
    (void)body_len;
    return ESP_ERR_NOT_FOUND;
}

void *solar_os_memory_alloc(size_t size,
                            solar_os_memory_class_t memory_class,
                            const char *tag)
{
    (void)memory_class;
    (void)tag;
    return malloc(size);
}

void solar_os_memory_free(void *pointer)
{
    free(pointer);
}

int main(void)
{
    assert(solar_os_manual_count() == 2U);
    const solar_os_manual_page_t *page = solar_os_manual_find("schedule");
    assert(page == &external_pages[1]);
    assert(strcmp(page->summary, "Fresh schedule routing metadata") == 0);

    const solar_os_manual_page_t *matches[2] = {0};
    assert(solar_os_manual_search("fresh scheduler", matches, 2U) == 1U);
    assert(matches[0] == &external_pages[1]);
    assert(solar_os_manual_reference_count() == 0U);

    external_active = false;
    assert(solar_os_manual_count() == solar_os_manual_embedded_count());
    assert(solar_os_manual_find("overview") == solar_os_manual_embedded_get(1U));
    assert(solar_os_manual_reference_count() > 0U);
    return 0;
}
