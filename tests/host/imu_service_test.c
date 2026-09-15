#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_imu.h"

typedef struct {
    const char *name;
    float acceleration;
    unsigned reads;
    bool try_unregister;
} fake_imu_t;

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

static esp_err_t fake_read_sample(void *ctx,
                                  uint32_t timeout_ms,
                                  solar_os_imu_sample_t *sample)
{
    fake_imu_t *fake = ctx;
    assert(fake != NULL);
    assert(timeout_ms == 250U);
    assert(sample != NULL);
    fake->reads++;
    if (fake->try_unregister) {
        assert(solar_os_imu_unregister(fake->name) == ESP_ERR_INVALID_STATE);
    }
    *sample = (solar_os_imu_sample_t) {
        .timestamp_us = 123456U,
        .valid = SOLAR_OS_IMU_CAP_ACCELERATION |
                 SOLAR_OS_IMU_CAP_ORIENTATION,
        .acceleration_m_s2 = {fake->acceleration, 2.0F, 3.0F},
        .orientation = {1.0F, 0.0F, 0.0F, 0.0F},
    };
    return ESP_OK;
}

static const solar_os_imu_ops_t fake_ops = {
    .read_sample = fake_read_sample,
};

static void register_fake(const char *name,
                          const char *driver,
                          solar_os_imu_capabilities_t capabilities,
                          fake_imu_t *fake)
{
    const solar_os_imu_registration_t registration = {
        .name = name,
        .driver = driver,
        .capabilities = capabilities,
        .ops = &fake_ops,
        .ctx = fake,
    };
    assert(solar_os_imu_register(&registration) == ESP_OK);
}

int main(void)
{
    fake_imu_t first = {
        .name = "imu-a",
        .acceleration = 1.25F,
        .try_unregister = true,
    };
    fake_imu_t second = {
        .name = "imu-b",
        .acceleration = -2.5F,
    };
    register_fake(first.name,
                  "fake-a",
                  SOLAR_OS_IMU_CAP_ACCELERATION,
                  &first);
    register_fake(second.name,
                  "fake-b",
                  SOLAR_OS_IMU_CAP_ACCELERATION |
                      SOLAR_OS_IMU_CAP_ORIENTATION,
                  &second);
    assert(solar_os_imu_count() == 2U);

    solar_os_imu_info_t info;
    assert(solar_os_imu_get(0U, &info));
    assert(strcmp(info.name, first.name) == 0);
    assert(strcmp(info.driver, "fake-a") == 0);
    assert(info.capabilities == SOLAR_OS_IMU_CAP_ACCELERATION);
    assert(solar_os_imu_get(1U, &info));
    assert(strcmp(info.name, second.name) == 0);
    assert(!solar_os_imu_get(2U, &info));

    solar_os_imu_sample_t sample;
    assert(solar_os_imu_read_sample(first.name, 250U, &sample) == ESP_OK);
    assert(sample.timestamp_us == 123456U);
    assert(sample.valid == SOLAR_OS_IMU_CAP_ACCELERATION);
    assert(fabsf(sample.acceleration_m_s2[0] - first.acceleration) < 0.001F);
    assert(first.reads == 1U);

    assert(solar_os_imu_read_sample(second.name, 250U, &sample) == ESP_OK);
    assert(sample.valid == (SOLAR_OS_IMU_CAP_ACCELERATION |
                            SOLAR_OS_IMU_CAP_ORIENTATION));
    assert(fabsf(sample.acceleration_m_s2[0] - second.acceleration) < 0.001F);
    assert(second.reads == 1U);
    assert(solar_os_imu_read_sample("missing", 250U, &sample) ==
           ESP_ERR_NOT_FOUND);

    assert(solar_os_imu_unregister(first.name) == ESP_OK);
    assert(solar_os_imu_unregister(second.name) == ESP_OK);
    assert(solar_os_imu_count() == 0U);

    puts("IMU service registry tests: ok");
    return 0;
}
