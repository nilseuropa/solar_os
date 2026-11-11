#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_imu.h"
#include "solar_os_stream.h"

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
static esp_err_t unavailable(void *user, solar_os_stream_handle_t *handle,
    void *data, size_t len, uint32_t timeout_ms, size_t *read_len)
{
    (void)user; (void)handle; (void)data; (void)len; (void)timeout_ms;
    *read_len=0; return ESP_ERR_TIMEOUT;
}

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
    assert(first.reads == 0 && second.reads == 0); /* Registering never samples. */
    solar_os_stream_info_t endpoint;
    assert(solar_os_stream_get_info("imu.imu-a", &endpoint) == ESP_OK);
    assert(endpoint.type == SOLAR_OS_STREAM_TYPE_BYTES && endpoint.direction == SOLAR_OS_STREAM_DIRECTION_SOURCE);
    assert(!strcmp(endpoint.format, SOLAR_OS_IMU_STREAM_FORMAT) && !strcmp(endpoint.device, "imu-a"));
    solar_os_stream_handle_t reader = SOLAR_OS_STREAM_HANDLE_INIT, other = SOLAR_OS_STREAM_HANDLE_INIT;
    assert(solar_os_stream_open(endpoint.id, "reader", &reader) == ESP_OK);
    assert(solar_os_stream_open(endpoint.id, "other", &other) == ESP_OK);
    assert(solar_os_imu_unregister(first.name) == ESP_ERR_INVALID_STATE);
    uint8_t wire[64]; memset(wire, 0xab, sizeof(wire)); size_t bytes = 0;
    assert(solar_os_stream_read(&reader, wire, 51, 250, &bytes) == ESP_ERR_INVALID_SIZE && !bytes && !first.reads);
    assert(solar_os_stream_read(&reader, wire, sizeof(wire), 250, &bytes) == ESP_OK && bytes == 52);
    assert(first.reads == 1 && wire[0] == 0x40 && wire[1] == 0xe2 && wire[2] == 1 && wire[7] == 0);
    assert(wire[8] == SOLAR_OS_IMU_CAP_ACCELERATION && wire[9] == 0);
    /* IEEE float32 1.25 = 0x3fa00000, independent of C struct padding. */
    assert(wire[12] == 0 && wire[13] == 0 && wire[14] == 0xa0 && wire[15] == 0x3f && wire[52] == 0xab);
    solar_os_stream_csv_record_t record; solar_os_stream_read_options_t options = {.timeout_ms=250};
    assert(solar_os_stream_read_csv(&other, &options, &record) == ESP_OK);
    assert(record.has_data && record.uptime_ms == 123 && strstr(record.line, "123456,1,1.25,2,3,") == record.line);
    assert(first.reads == 2);
    assert(solar_os_stream_close_ex(&reader) == ESP_OK);
    assert(solar_os_imu_unregister(first.name) == ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_close_ex(&other) == ESP_OK);

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
    assert(first.reads == 3U);

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
    assert(solar_os_stream_get_info("imu.imu-a", &endpoint) == ESP_ERR_NOT_FOUND);
    /* A duplicate endpoint rolls back the IMU registration. */
    solar_os_stream_driver_t conflict = {.info={.id="imu.imu-a", .provider="test", .format="bytes",
        .type=SOLAR_OS_STREAM_TYPE_BYTES, .direction=SOLAR_OS_STREAM_DIRECTION_SOURCE}, .read=unavailable};
    assert(solar_os_stream_register(&conflict) == ESP_OK);
    solar_os_imu_registration_t registration = {.name=first.name, .driver="fake", .capabilities=1, .ops=&fake_ops, .ctx=&first};
    assert(solar_os_imu_register(&registration) != ESP_OK && solar_os_imu_count() == 0);
    assert(solar_os_stream_unregister(conflict.info.id) == ESP_OK);
    register_fake(first.name, "fake", 1, &first);
    assert(solar_os_imu_unregister(first.name) == ESP_OK);

    puts("IMU service registry tests: ok");
    return 0;
}
