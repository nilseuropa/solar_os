#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "max17048.h"

typedef struct {
    uint8_t registers[256];
    esp_err_t result;
    uint8_t requested_regs[8];
    size_t requested_lens[8];
    size_t request_count;
} fake_bus_t;

static esp_err_t fake_read(void *user,
                           uint8_t reg,
                           uint8_t *data,
                           size_t len)
{
    fake_bus_t *bus = user;
    assert(bus->request_count < 8U);
    bus->requested_regs[bus->request_count] = reg;
    bus->requested_lens[bus->request_count] = len;
    bus->request_count++;
    if (bus->result != ESP_OK) {
        return bus->result;
    }
    memcpy(data, &bus->registers[reg], len);
    return ESP_OK;
}

int main(void)
{
    fake_bus_t bus = {.result = ESP_OK};
    bus.registers[0x08] = 0x00;
    bus.registers[0x09] = 0x11;
    const max17048_io_t io = {.read = fake_read, .user = &bus};
    max17048_t gauge;
    assert(max17048_init(&gauge, &io) == ESP_OK);
    assert(gauge.version == 0x0011U);

    /* Separate hardware reads: 0xD440 is 4245 mV and 0x2998 is 41.59%. */
    bus.registers[0x02] = 0xD4;
    bus.registers[0x03] = 0x40;
    bus.registers[0x04] = 0x29;
    bus.registers[0x05] = 0x98;
    max17048_sample_t sample;
    size_t request = bus.request_count;
    assert(max17048_read_sample(&gauge, &sample) == ESP_OK);
    assert(bus.request_count == request + 2U);
    assert(bus.requested_regs[request] == 0x02U);
    assert(bus.requested_lens[request] == 2U);
    assert(bus.requested_regs[request + 1U] == 0x04U);
    assert(bus.requested_lens[request + 1U] == 2U);
    assert(sample.voltage_mv == 4245U);
    assert(sample.soc_raw == 0x2998U);
    assert(sample.percent == 42U);

    bus.registers[0x04] = 0xFF;
    bus.registers[0x05] = 0xFF;
    assert(max17048_read_sample(&gauge, &sample) == ESP_OK);
    assert(sample.percent == 100U);

    bus.result = ESP_FAIL;
    assert(max17048_read_sample(&gauge, &sample) == ESP_FAIL);
    puts("MAX17048 tests: ok");
    return 0;
}
