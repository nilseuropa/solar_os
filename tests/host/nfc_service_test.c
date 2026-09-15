#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_nfc.h"

typedef struct {
    const char *name;
    uint8_t uid;
    unsigned scans;
    unsigned power_changes;
    bool powered;
    bool try_unregister;
} fake_nfc_t;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t length = strlen(src);
    if (size > 0U) {
        const size_t copy = length < size - 1U ? length : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return length;
}

static esp_err_t fake_set_power(void *ctx, bool enabled)
{
    fake_nfc_t *fake = ctx;
    fake->power_changes++;
    if (fake->try_unregister) {
        assert(solar_os_nfc_unregister(fake->name) == ESP_ERR_INVALID_STATE);
    }
    fake->powered = enabled;
    return ESP_OK;
}

static esp_err_t fake_scan(void *ctx,
                           uint32_t timeout_ms,
                           solar_os_nfc_tag_t *tag)
{
    fake_nfc_t *fake = ctx;
    assert(fake != NULL);
    assert(timeout_ms == 300U);
    assert(tag != NULL);
    fake->scans++;
    if (fake->try_unregister) {
        assert(solar_os_nfc_unregister(fake->name) == ESP_ERR_INVALID_STATE);
    }
    *tag = (solar_os_nfc_tag_t) {
        .technology = SOLAR_OS_NFC_TECHNOLOGY_NFCA,
        .uid = {fake->uid},
        .uid_len = 1U,
    };
    return ESP_OK;
}

static const solar_os_nfc_ops_t fake_ops = {
    .scan = fake_scan,
};

static const solar_os_nfc_ops_t controlled_ops = {
    .scan = fake_scan,
    .set_power = fake_set_power,
};

static void register_fake(const char *name,
                          const char *driver,
                          fake_nfc_t *fake,
                          bool controlled)
{
    const solar_os_nfc_registration_t registration = {
        .name = name,
        .driver = driver,
        .ops = controlled ? &controlled_ops : &fake_ops,
        .ctx = fake,
        .powered = fake->powered,
    };
    assert(solar_os_nfc_register(&registration) == ESP_OK);
}

int main(void)
{
    fake_nfc_t first = {
        .name = "nfc-a",
        .uid = 0x12U,
        .try_unregister = true,
    };
    fake_nfc_t second = {
        .name = "nfc-b",
        .uid = 0x34U,
    };
    register_fake(first.name, "fake-a", &first, true);
    register_fake(second.name, "fake-b", &second, false);
    assert(solar_os_nfc_count() == 2U);

    solar_os_nfc_info_t info;
    assert(solar_os_nfc_get(0U, &info));
    assert(strcmp(info.name, first.name) == 0);
    assert(strcmp(info.driver, "fake-a") == 0);
    assert(info.power_control);
    assert(!info.powered);
    assert(solar_os_nfc_get(1U, &info));
    assert(strcmp(info.name, second.name) == 0);
    assert(!info.power_control);
    assert(info.powered);
    assert(!solar_os_nfc_get(2U, &info));

    solar_os_nfc_tag_t tag;
    assert(solar_os_nfc_scan(first.name, 300U, &tag) == ESP_ERR_INVALID_STATE);
    assert(solar_os_nfc_set_power(first.name, true) == ESP_OK);
    assert(first.powered);
    assert(first.power_changes == 1U);
    assert(solar_os_nfc_set_power(first.name, true) == ESP_OK);
    assert(first.power_changes == 1U);
    assert(solar_os_nfc_scan(first.name, 300U, &tag) == ESP_OK);
    assert(tag.uid_len == 1U);
    assert(tag.uid[0] == first.uid);
    assert(first.scans == 1U);

    assert(solar_os_nfc_scan(second.name, 300U, &tag) == ESP_OK);
    assert(tag.uid[0] == second.uid);
    assert(second.scans == 1U);
    assert(solar_os_nfc_scan("missing", 300U, &tag) == ESP_ERR_NOT_FOUND);
    assert(solar_os_nfc_set_power(second.name, false) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_nfc_set_power("missing", true) == ESP_ERR_NOT_FOUND);
    assert(solar_os_nfc_set_power(first.name, false) == ESP_OK);
    assert(!first.powered);
    assert(first.power_changes == 2U);

    assert(solar_os_nfc_unregister(first.name) == ESP_OK);
    assert(solar_os_nfc_unregister(second.name) == ESP_OK);
    assert(solar_os_nfc_count() == 0U);

    puts("NFC service registry tests: ok");
    return 0;
}
