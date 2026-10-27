#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"

typedef struct {
    const char *i2c_bus;
    uint8_t address;
    uint32_t invalid_counts;
} solar_os_cl32_core_device_t;

static uint8_t interrupts, queued;
static unsigned acknowledgements, published;
static bool fail_read, fail_write, keep_arriving, irq_low;
static esp_err_t read_register(solar_os_cl32_core_device_t *device, uint8_t reg, uint8_t *value)
{
    assert(device->address == 8);
    if (fail_read) return ESP_FAIL;
    if (reg == 2) *value = interrupts;
    else if (reg == 3) *value = queued;
    else { assert(reg == 4 && queued); *value = 0x84; if (!keep_arriving) queued--; }
    return ESP_OK;
}
static esp_err_t solar_os_bus_i2c_write_reg(const char *bus, uint8_t address,
    uint8_t reg, const void *data, size_t size)
{
    assert(!strcmp(bus, "i2c0") && address == 8 && reg == 2 && size == 1);
    if (fail_write) return ESP_FAIL;
    interrupts = *(const uint8_t *)data;
    irq_low = interrupts != 0;
    acknowledgements++;
    return ESP_OK;
}
static void publish_wire_event(solar_os_cl32_core_device_t *device, uint8_t event)
{ (void)device; assert(event == 0x84); published++; }

#include "cl32_interrupt.inc"

int main(void)
{
    solar_os_cl32_core_device_t device = {.i2c_bus = "i2c0", .address = 8};
    /* USB/charging notifications release the wake line without keyboard data. */
    interrupts = 6;
    assert(poll_keyboard(&device) == ESP_OK && interrupts == 0 && acknowledgements == 1);
    interrupts = 15; queued = 2;
    assert(poll_keyboard(&device) == ESP_OK && interrupts == 0 && published == 2);
    assert(acknowledgements == 2);
    /* Preserve unknown notifications, including alongside known power flags. */
    interrupts = 0x86;
    assert(poll_keyboard(&device) == ESP_OK && interrupts == 0x80 && acknowledgements == 3);
    assert(poll_keyboard(&device) == ESP_OK && acknowledgements == 3);
    /* Failed I2C operations leave pending flags available for retry. */
    interrupts = 6; fail_read = true;
    assert(poll_keyboard(&device) == ESP_FAIL && interrupts == 6);
    fail_read = false; fail_write = true;
    assert(poll_keyboard(&device) == ESP_FAIL && interrupts == 6);
    fail_write = false;
    assert(poll_keyboard(&device) == ESP_OK && interrupts == 0);
    /* Never acknowledge while events remain in the bounded FIFO drain. */
    interrupts = 7; queued = 1; keep_arriving = true;
    const unsigned before = acknowledgements;
    assert(poll_keyboard(&device) == ESP_OK && acknowledgements == before && interrupts == 7);
    keep_arriving = false;
    assert(poll_keyboard(&device) == ESP_OK && interrupts == 0);
    interrupts = 1; queued = 11;
    assert(poll_keyboard(&device) == ESP_ERR_INVALID_RESPONSE && interrupts == 1);
    /* Empty event flags do not prove that the AVR released its IRQ output. */
    interrupts = 0; queued = 0; irq_low = true;
    const unsigned empty_ack = acknowledgements;
    assert(poll_keyboard(&device) == ESP_OK && !irq_low);
    assert(acknowledgements == empty_ack + 1);
    puts("CL-32 shared wake interrupt acknowledgement passed");
    return 0;
}
