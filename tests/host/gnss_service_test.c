#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_gnss.h"

typedef struct {
    const char *name;
    int32_t latitude;
    unsigned reads;
    bool try_unregister;
} fake_gnss_t;

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

static esp_err_t fake_read_fix(void *ctx,
                               uint32_t timeout_ms,
                               solar_os_gnss_fix_t *fix)
{
    fake_gnss_t *fake = ctx;
    assert(fake != NULL);
    assert(timeout_ms == 250U);
    assert(fix != NULL);
    fake->reads++;
    if (fake->try_unregister) {
        assert(solar_os_gnss_unregister(fake->name) == ESP_ERR_INVALID_STATE);
    }
    *fix = (solar_os_gnss_fix_t) {
        .valid = true,
        .latitude_deg_e7 = fake->latitude,
    };
    return ESP_OK;
}

static const solar_os_gnss_ops_t fake_ops = {
    .read_fix = fake_read_fix,
};

static void register_fake(const char *name, const char *driver, fake_gnss_t *fake)
{
    const solar_os_gnss_registration_t registration = {
        .name = name,
        .driver = driver,
        .ops = &fake_ops,
        .ctx = fake,
    };
    assert(solar_os_gnss_register(&registration) == ESP_OK);
}

int main(void)
{
    fake_gnss_t first = {
        .name = "gnss-a",
        .latitude = 123456789,
        .try_unregister = true,
    };
    fake_gnss_t second = {
        .name = "gnss-b",
        .latitude = -234567890,
    };
    register_fake(first.name, "fake-a", &first);
    register_fake(second.name, "fake-b", &second);
    assert(solar_os_gnss_count() == 2U);

    solar_os_gnss_info_t info;
    assert(solar_os_gnss_get(0U, &info));
    assert(strcmp(info.name, first.name) == 0);
    assert(strcmp(info.driver, "fake-a") == 0);
    assert(solar_os_gnss_get(1U, &info));
    assert(strcmp(info.name, second.name) == 0);
    assert(!solar_os_gnss_get(2U, &info));

    solar_os_gnss_fix_t fix;
    assert(solar_os_gnss_read_fix(first.name, 250U, &fix) == ESP_OK);
    assert(fix.valid);
    assert(fix.latitude_deg_e7 == first.latitude);
    assert(first.reads == 1U);

    assert(solar_os_gnss_read_fix(second.name, 250U, &fix) == ESP_OK);
    assert(fix.latitude_deg_e7 == second.latitude);
    assert(second.reads == 1U);
    assert(solar_os_gnss_read_fix("missing", 250U, &fix) == ESP_ERR_NOT_FOUND);

    assert(solar_os_gnss_unregister(first.name) == ESP_OK);
    assert(solar_os_gnss_unregister(second.name) == ESP_OK);
    assert(solar_os_gnss_count() == 0U);

    puts("GNSS service registry tests: ok");
    return 0;
}
