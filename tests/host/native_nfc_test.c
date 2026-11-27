#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_native_device.h"
#include "solar_os_native_handles.h"
#include "solar_os_nfc.h"
#include "solar_os_memory.h"

static const solar_os_native_nfc_api_v1_t *api;
static solar_os_native_handle_t handle;
static bool powered, malformed;
static unsigned allocations, leases;

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t memory_class, const char *tag)
{
    assert(memory_class == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && !strcmp(tag, "native.provider"));
    void *ptr = calloc(count, size);
    if (ptr) ++allocations;
    return ptr;
}

void solar_os_memory_free(void *ptr)
{
    assert(ptr && allocations);
    --allocations;
    free(ptr);
}

int solar_os_native_library_retain(uint32_t lease, uint32_t *out)
{
    assert(lease == 7);
    *out = 20 + ++leases;
    return SOLAR_OS_NATIVE_OK;
}

int solar_os_native_library_release(uint32_t lease)
{
    assert(leases && lease == 20 + leases);
    --leases;
    return SOLAR_OS_NATIVE_OK;
}

static int set_power(void *user, bool enabled)
{
    assert(user == &powered && leases == 1);
    assert(api->provider_close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    powered = enabled;
    return SOLAR_OS_NATIVE_OK;
}

static int scan(void *user, uint32_t timeout_ms, solar_os_nfc_tag_t *tag)
{
    assert(user == &powered && powered && timeout_ms == 300 && leases == 1);
    assert(api->provider_close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(api->set_power("fixture", false) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && powered);
    *tag = (solar_os_nfc_tag_t){.uid_len = malformed ? 255 : 4, .uid = {1, 2, 3, 4}};
    return SOLAR_OS_NATIVE_OK;
}

static esp_err_t scan_replacement(void *user, uint32_t timeout_ms, solar_os_nfc_tag_t *tag)
{
    assert(!user && timeout_ms == 300);
    *tag = (solar_os_nfc_tag_t){.uid_len = 4, .uid = {4, 3, 2, 1}};
    return ESP_OK;
}

int main(void)
{
    api = solar_os_native_device_get_service(SOLAR_OS_NATIVE_NFC_SERVICE, SOLAR_OS_NATIVE_NFC_ABI, sizeof(*api));
    assert(api && !api->count());
    char name[] = "fixture", driver[] = "portable";
    solar_os_native_nfc_provider_v1_t provider = {.name = name, .driver = driver,
        .library_lease = 7, .user = &powered, .scan = scan, .set_power = set_power};
    assert(!api->provider_register(&provider, &handle) && handle && allocations == 1 && leases == 1);
    solar_os_native_handle_t duplicate = 0;
    assert(api->provider_register(&provider, &duplicate) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !duplicate);
    assert(allocations == 1 && leases == 1);
    memset(&provider, 0, sizeof(provider));
    name[0] = driver[0] = 0;
    solar_os_nfc_info_t info;
    assert(api->info(0, &info) && !strcmp(info.name, "fixture") && !strcmp(info.driver, "portable"));
    assert(info.power_control && !info.powered);
    solar_os_nfc_tag_t tag;
    assert(api->scan("fixture", 300, &tag) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !tag.uid_len);
    assert(!api->set_power("fixture", true) && api->info(0, &info) && info.powered);
    assert(!api->scan("fixture", 300, &tag) && tag.uid_len == 4 && tag.uid[0] == 1);
    malformed = true;
    assert(api->scan("fixture", 300, &tag) == SOLAR_OS_NATIVE_ERROR_FAILED && !tag.uid_len);
    assert(!api->set_power("fixture", false));

    /* The native token must only remove the registration it owns. */
    assert(!solar_os_nfc_unregister("fixture"));
    static const solar_os_nfc_ops_t ops = {.scan = scan_replacement};
    const solar_os_nfc_registration_t replacement = {.name = "fixture", .driver = "built-in", .ops = &ops};
    assert(!solar_os_nfc_register(&replacement));
    assert(!api->provider_close(handle) && !allocations && !leases && !solar_os_native_handle_count());
    assert(api->count() == 1 && !api->scan("fixture", 300, &tag) && tag.uid[0] == 4);
    assert(!solar_os_nfc_unregister("fixture"));
    return 0;
}
