#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_native_client.h"
#include "solar_os_native_device.h"
#include "solar_os_ota.h"

#if SOLAR_OS_PACKAGE_SERVICE_OTA
static const char release_url[] = "https://example.test/releases";
static const char flavor[] = "classic";
static bool boot_selected;

void solar_os_ota_get_url(char *out, size_t capacity)
{
    snprintf(out, capacity, "%s", release_url);
}

esp_err_t solar_os_ota_set_url(const char *url)
{
    return url && !strcmp(url, release_url) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

void solar_os_ota_get_flavor(char *out, size_t capacity)
{
    snprintf(out, capacity, "%s", flavor);
}

esp_err_t solar_os_ota_set_flavor(const char *setting)
{
    return setting && !strcmp(setting, flavor) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

bool solar_os_ota_available_flavors_checked(void) { return true; }
size_t solar_os_ota_available_flavor_count(void) { return 1; }

bool solar_os_ota_get_available_flavor(size_t index, char *out, size_t capacity)
{
    if (index) return false;
    solar_os_ota_get_flavor(out, capacity);
    return true;
}

esp_err_t solar_os_ota_get_index_url(char *out, size_t capacity)
{
    const int length = snprintf(out, capacity, "%s/index.json", release_url);
    return length >= 0 && (size_t)length < capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_ota_get_status(solar_os_ota_status_t *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    *status = (solar_os_ota_status_t){.ota_partition_count = 2, .running = {.valid = true, .slot = 0}};
    return ESP_OK;
}

esp_err_t solar_os_ota_check(solar_os_ota_check_result_t *result)
{
    assert(result && result->status_code == -1 && result->content_length == -1);
    result->index_signature_verified = true;
    result->status_code = 200;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_ota_upgrade(solar_os_ota_progress_cb_t progress, void *user)
{
    const solar_os_ota_progress_t event = {.stage = SOLAR_OS_OTA_PROGRESS_VERIFYING, .bytes_read = 1234};
    if (progress) progress(&event, user);
    return ESP_ERR_TIMEOUT;
}

esp_err_t solar_os_ota_set_boot_slot(uint8_t slot)
{
    if (slot > 1) return ESP_ERR_NOT_FOUND;
    boot_selected = true;
    return ESP_OK;
}

static void report_progress(const solar_os_ota_progress_t *event, void *user)
{
    bool *called = user;
    assert(!*called && event->stage == SOLAR_OS_OTA_PROGRESS_VERIFYING && event->bytes_read == 1234);
    *called = true;
}
#endif

int main(void)
{
    const solar_os_native_ota_api_v1_t *ota =
        solar_os_native_ota_get(solar_os_native_device_get_service, sizeof(*ota));
#if SOLAR_OS_PACKAGE_SERVICE_OTA
    assert(ota && !solar_os_native_device_get_service("ota", 2, sizeof(*ota)));
    assert(!solar_os_native_ota_get(solar_os_native_device_get_service, sizeof(*ota) + 1));
    solar_os_ota_status_t status;
    assert(!ota->status(&status) && status.ota_partition_count == 2 && status.running.valid);
    assert(ota->status(NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);

    char text[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
    memset(text, 'x', sizeof(text));
    assert(ota->get_url(text, strlen(release_url)) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && text[0] == 'x');
    assert(!ota->get_url(text, sizeof(text)) && !strcmp(text, release_url));
    assert(!ota->set_url(text) && ota->set_url(NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!ota->get_flavor(text, sizeof(text)) && !strcmp(text, flavor) && !ota->set_flavor(text));
    assert(ota->available_flavors_checked() && ota->available_flavor_count() == 1);
    assert(ota->available_flavor_at(1, text, sizeof(text)) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!ota->available_flavor_at(0, text, sizeof(text)) && !strcmp(text, flavor));
    assert(!ota->index_url(text, sizeof(text)) && !strcmp(text, "https://example.test/releases/index.json"));

    solar_os_ota_check_result_t result;
    assert(ota->check(&result) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(result.index_signature_verified && result.status_code == 200);
    assert(ota->check(NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    bool called = false;
    assert(ota->upgrade(report_progress, &called) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && called);
    assert(ota->set_boot_slot(2) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !boot_selected);
    assert(!ota->set_boot_slot(1) && boot_selected);
#else
    assert(!ota);
#endif
    return 0;
}
