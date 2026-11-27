#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_docs.h"
#include "solar_os_manual.h"
#include "solar_os_native_manual.h"
#include "solar_os_native_manual_abi.h"
#include "solar_os_memory.h"

static bool external_active = true;
static bool external_file_available = true;

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

static unsigned snapshot_pins;

bool solar_os_docs_manual_snapshot_acquire(solar_os_manual_catalog_t *catalog)
{
    *catalog = (solar_os_manual_catalog_t){0};
    if (!external_active) return false;
    *catalog = (solar_os_manual_catalog_t){
        .pages = external_pages, .count = solar_os_docs_manual_count(), .pin = &snapshot_pins,
    };
    snapshot_pins++;
    return true;
}

void solar_os_docs_manual_snapshot_release(solar_os_manual_catalog_t *catalog)
{
    if (!catalog->pin) return;
    assert(catalog->pin == &snapshot_pins && snapshot_pins);
    snapshot_pins--;
    *catalog = (solar_os_manual_catalog_t){0};
}

esp_err_t solar_os_docs_load_page(const char *id, char **body, size_t *body_len)
{
    if (!external_active || !external_file_available ||
        strcmp(id, "overview") != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const char *source = "+++\nid = \"overview\"\n+++\n"
                         "# Downloaded overview\n\n"
                         "The complete downloaded guide.\n\n"
                         "## Quick reference\n\n"
                         "The complete downloaded API contract.\n";
    *body_len = strlen(source);
    *body = malloc(*body_len + 1U);
    assert(*body != NULL);
    memcpy(*body, source, *body_len + 1U);
    return ESP_OK;
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

esp_err_t solar_os_docs_get_status(solar_os_docs_status_t *status)
{
    *status = (solar_os_docs_status_t){
        .available = external_active, .page_count = solar_os_docs_manual_count(), .revision = "1234567890123456",
    };
    return ESP_OK;
}

esp_err_t solar_os_docs_update(solar_os_docs_progress_fn progress, void *user)
{
    external_active = external_file_available = true;
    const solar_os_docs_progress_t result = {.stage = SOLAR_OS_DOCS_PROGRESS_DONE, .page_count = 2};
    if (progress) progress(&result, user);
    return ESP_OK;
}

esp_err_t solar_os_docs_reset(void)
{
    external_active = false;
    return ESP_OK;
}

static bool check_downloaded_page(const solar_os_native_manual_page_v1_t *page, void *user)
{
    unsigned *visits = user;
    assert(snapshot_pins == 1 && external_active);
    assert(!strcmp(page->id, "command.schedule"));
    assert(!strcmp(page->summary, "Fresh schedule routing metadata"));
    (*visits)++;
    return false;
}

static void test_native_manual(void)
{
    const solar_os_native_manual_api_v1_t *api = solar_os_native_manual_get_service(
        SOLAR_OS_NATIVE_MANUAL_SERVICE, SOLAR_OS_NATIVE_MANUAL_ABI, sizeof(*api));
    assert(api && api->page_count(false) == 2 && api->page_count(true) == solar_os_manual_embedded_count());
    assert(!solar_os_native_manual_get_service(SOLAR_OS_NATIVE_MANUAL_SERVICE, 2, 8));
    unsigned visits = 0;
    assert(api->find_page(false, "schedule", check_downloaded_page, &visits) == SOLAR_OS_NATIVE_OK);
    assert(visits == 1 && !snapshot_pins);

    size_t visited = 0;
    assert(api->search_pages(false, "fresh scheduler", 10, check_downloaded_page, &visits, &visited)
        == SOLAR_OS_NATIVE_OK && visited == 1 && visits == 2 && !snapshot_pins);
    assert(api->page_info(false, 2, check_downloaded_page, &visits) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->reference_count() == 0 && !snapshot_pins);

    char text[80] = {0};
    size_t copied, total;
    assert(api->read_page(false, "overview", SOLAR_OS_NATIVE_MANUAL_MARKDOWN, 2, text,
        sizeof(text) - 1, &copied, &total) == SOLAR_OS_NATIVE_OK);
    assert(!memcmp(text, "Downloaded overview", 19) && copied > 19 && total >= copied + 2 && !snapshot_pins);
    assert(api->read_page(false, "overview", SOLAR_OS_NATIVE_MANUAL_QUICK_REFERENCE, 0, NULL, 0,
        &copied, &total) == SOLAR_OS_NATIVE_OK && !copied && total > 0);
    assert(api->read_page(true, "overview", SOLAR_OS_NATIVE_MANUAL_BODY, SIZE_MAX, text, sizeof(text),
        &copied, &total) == SOLAR_OS_NATIVE_OK && !copied && total > 0);
    const solar_os_manual_reference_t reference = {.page_id = "help", .length = 5, .offset = 1};
    assert(api->read_reference(&reference, 0, text, sizeof(text), &copied, &total) == SOLAR_OS_NATIVE_OK);
    assert(copied == 5 && total == 5 && !snapshot_pins);
}

static void check_catalog_progress(const solar_os_docs_progress_t *progress, void *user)
{
    unsigned *visits = user;
    assert(progress->stage == SOLAR_OS_DOCS_PROGRESS_DONE && progress->page_count == 2);
    (*visits)++;
}

static void test_native_catalog(void)
{
    const solar_os_native_docs_api_v1_t *api = solar_os_native_manual_get_service(
        SOLAR_OS_NATIVE_DOCS_SERVICE, SOLAR_OS_NATIVE_DOCS_ABI, sizeof(*api));
    assert(api && api->available() && api->page_count() == 2);
    solar_os_docs_status_t status;
    assert(api->status(&status) == SOLAR_OS_NATIVE_OK && status.available);
    assert(!strcmp(status.revision, "1234567890123456"));
    unsigned visits = 0;
    assert(api->page_info(1, check_downloaded_page, &visits) == SOLAR_OS_NATIVE_OK);
    assert(visits == 1 && !snapshot_pins);

    size_t copied, total;
    char prefix[4];
    assert(api->read_source("overview", 0, prefix, sizeof(prefix), &copied, &total) == SOLAR_OS_NATIVE_OK);
    assert(copied == 4 && total > 4 && !memcmp(prefix, "+++\n", 4));
    assert(api->read_source("missing", 0, prefix, sizeof(prefix), &copied, &total) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!copied && !total && api->available());
    assert(api->reset() == SOLAR_OS_NATIVE_OK && !api->available() && !api->page_count());
    assert(api->page_info(0, check_downloaded_page, &visits) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->update(check_catalog_progress, &visits) == SOLAR_OS_NATIVE_OK && api->available());
    assert(visits == 2 && !snapshot_pins);
}

int main(void)
{
    test_native_manual();
    test_native_catalog();
    assert(solar_os_manual_count() == 2U);
    const solar_os_manual_page_t *page = solar_os_manual_find("schedule");
    assert(page == &external_pages[1]);
    assert(strcmp(page->summary, "Fresh schedule routing metadata") == 0);

    const solar_os_manual_page_t *matches[2] = {0};
    assert(solar_os_manual_search("fresh scheduler", matches, 2U) == 1U);
    assert(matches[0] == &external_pages[1]);
    assert(solar_os_manual_reference_count() == 0U);

    page = solar_os_manual_find("overview");
    const char *text = NULL;
    size_t text_len = 0U;
    bool owned = false;
    assert(solar_os_manual_load_body(page, &text, &text_len, &owned) == ESP_OK);
    assert(owned && strstr(text, "complete downloaded guide") != NULL);
    solar_os_manual_release_text(text, owned);
    assert(solar_os_manual_load_markdown(page, &text, &text_len, &owned) == ESP_OK);
    assert(owned && strstr(text, "# Downloaded overview") != NULL);
    assert(strstr(text, "+++") == NULL);
    solar_os_manual_release_text(text, owned);
    assert(solar_os_manual_load_contract(page, &text, &text_len, &owned) == ESP_OK);
    assert(owned && strcmp(text, "\nThe complete downloaded API contract.") == 0);
    solar_os_manual_release_text(text, owned);

    external_file_available = false;
    assert(solar_os_manual_load_body(page, &text, &text_len, &owned) == ESP_OK);
    assert(!owned && strcmp(text, "embedded body") == 0);

    external_active = false;
    assert(solar_os_manual_count() == solar_os_manual_embedded_count());
    page = solar_os_manual_find("overview");
    assert(page && !strcmp(page->id, "overview") && strcmp(page->title, "Downloaded overview"));
    assert(solar_os_manual_reference_count() == 0U);
    page = solar_os_manual_find("help");
    assert(solar_os_manual_load_body(page, &text, &text_len, &owned) == ESP_OK);
    assert(!owned && strstr(text, "wifi connect SSID PASSWORD") != NULL);
    assert(strstr(text, "disk mount") != NULL);
    assert(strstr(text, "help update") != NULL);
    page = solar_os_manual_find("python.network");
    assert(page != NULL);
    assert(solar_os_manual_load_body(page, &text, &text_len, &owned) == ESP_OK);
    assert(!owned && strstr(text, "downloadable manual") != NULL);
    assert(strstr(text, "post(url") == NULL);
    const char *body = text;
    assert(solar_os_manual_load_markdown(page, &text, &text_len, &owned) == ESP_OK);
    assert(!owned && text == body);
    return 0;
}
