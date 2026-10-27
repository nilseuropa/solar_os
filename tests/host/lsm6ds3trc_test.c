#include <assert.h>
#include <math.h>
#include <string.h>
#include "lsm6ds3trc.h"

typedef struct {
    uint8_t registers[256];
    unsigned writes, delays, reset_reads;
    int fail_write, fail_read;
    bool stuck;
} fake_t;

static esp_err_t read_regs(void *ctx, uint8_t reg, uint8_t *data, size_t size) {
    fake_t *bus = ctx;
    if (reg == bus->fail_read) return ESP_FAIL;
    if (reg == 0x12 && (bus->registers[reg] & 1)) {
        if (++bus->reset_reads > 2 && !bus->stuck) bus->registers[reg] = 0;
    }
    if (reg == 0x22) assert(size == 12);
    memcpy(data, bus->registers + reg, size); return ESP_OK;
}
static esp_err_t write_regs(void *ctx, uint8_t reg, const uint8_t *data, size_t size) {
    fake_t *bus = ctx;
    assert(size == 1);
    if ((int)bus->writes++ == bus->fail_write) return ESP_FAIL;
    bus->registers[reg] = *data; return ESP_OK;
}
static void delay_ms(void *ctx, uint32_t ms) {
    fake_t *bus = ctx; assert(ms == 1); bus->delays++;
}
static fake_t fresh(void) {
    fake_t bus = {.fail_write=-1, .fail_read=-1};
    bus.registers[0x0f] = 0x6a; return bus;
}
static esp_err_t init(fake_t *bus, lsm6ds3trc_t *device) {
    const lsm6ds3trc_io_t io = {.read=read_regs, .write=write_regs, .delay_ms=delay_ms, .ctx=bus};
    return lsm6ds3trc_init(device, &io);
}
int main(void) {
    fake_t bus = fresh(); lsm6ds3trc_t device;
    bus.registers[0x0f] = 0x69;
    assert(init(&bus, &device) == ESP_ERR_NOT_FOUND && !device.initialized && bus.writes == 0);
    for (int write = 0; write < 4; write++) {
        bus = fresh(); bus.fail_write = write;
        assert(init(&bus, &device) == ESP_FAIL && !device.initialized);
        assert(bus.registers[0x10] == 0 && bus.registers[0x11] == 0);
    }
    bus = fresh(); bus.stuck = true;
    assert(init(&bus, &device) == ESP_ERR_TIMEOUT && bus.delays == 100 && !device.initialized);
    bus = fresh(); bus.fail_read = 0x0f;
    assert(init(&bus, &device) == ESP_FAIL && bus.writes == 0);
    bus = fresh(); assert(init(&bus, &device) == ESP_OK);
    assert(bus.delays == 2 && bus.registers[0x12] == 0x44);
    assert(bus.registers[0x10] == 0x40 && bus.registers[0x11] == 0x40);
    bool ready;
    for (unsigned status = 0; status < 4; status++) {
        bus.registers[0x1e] = status;
        assert(lsm6ds3trc_data_ready(&device, &ready) == ESP_OK && ready == (status == 3));
    }
    const int16_t raw[] = {1000, -2000, 32767, -16384, 1000, -32768};
    for (unsigned i = 0; i < 6; i++) {
        bus.registers[0x22 + i * 2] = (uint16_t)raw[i];
        bus.registers[0x23 + i * 2] = (uint16_t)raw[i] >> 8;
    }
    lsm6ds3trc_sample_t sample;
    assert(lsm6ds3trc_read_sample(&device, &sample) == ESP_OK);
    for (unsigned i = 0; i < 3; i++) {
        assert(fabsf(sample.angular_velocity_rad_s[i] - raw[i] * 0.00015271631F) < 0.00001F);
        assert(fabsf(sample.acceleration_m_s2[i] - raw[i + 3] * 0.00059820565F) < 0.00001F);
    }
    const lsm6ds3trc_sample_t previous = sample;
    bus.fail_read = 0x22;
    assert(lsm6ds3trc_read_sample(&device, &sample) == ESP_FAIL && !memcmp(&sample, &previous, sizeof(sample)));
    bus.fail_read = -1;
    assert(lsm6ds3trc_deinit(&device) == ESP_OK && !device.initialized);
    assert(bus.registers[0x10] == 0 && bus.registers[0x11] == 0);
    assert(lsm6ds3trc_read_sample(&device, &sample) == ESP_ERR_INVALID_ARG);
    return 0;
}
