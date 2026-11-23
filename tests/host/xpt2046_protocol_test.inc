#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "xpt2046.h"
static unsigned transfers, fail_at, z_reads, x_reads, y_reads;
static bool floating, weak, release_during_read;
static uint8_t last_command;
static esp_err_t transfer(void *context, const uint8_t tx[3], uint8_t rx[3])
{
    (void)context; ++transfers; last_command = tx[0]; assert(!tx[1] && !tx[2]);
    assert((tx[0] & 0x0eU) == 0 && (tx[0] & 0x80U));
    if (transfers == fail_at) return ESP_FAIL;
    uint16_t value = 0;
    switch (tx[0] & 0xf0U) {
    case 0xb0: value = release_during_read && z_reads >= 2 ? 0 : weak ? 1 : 800; ++z_reads; break;
    case 0xc0: value = weak || (release_during_read && z_reads > 2) ? 4095 : 3200; break;
    case 0xd0: { const uint16_t samples[] = {4095,1000,0,1001,4095,1002}; assert(x_reads < 6); value = samples[x_reads++]; break; }
    case 0x90: {
        if (tx[0] & 1U) { const uint16_t samples[] = {4095,2000,1999,500,2001,4000}; assert(y_reads < 6); value = samples[y_reads++]; }
        break;
    }
    default: assert(false);
    }
    if (floating) value = 4095;
    value <<= 3; rx[0] = 0; rx[1] = value >> 8; rx[2] = value;
    return ESP_OK;
}
static void reset(void)
{ transfers = fail_at = z_reads = x_reads = y_reads = 0; floating = weak = release_during_read = false; last_command = 0; }
int main(void)
{
    solar_os_xpt2046_io_t io = {.transfer = transfer}; solar_os_xpt2046_sample_t sample;
    assert(solar_os_xpt2046_read(NULL,400,&sample) == ESP_ERR_INVALID_ARG && !sample.valid);
    assert(solar_os_xpt2046_read(&io,0,&sample) == ESP_ERR_INVALID_ARG);
    assert(solar_os_xpt2046_read(&io,4096,&sample) == ESP_ERR_INVALID_ARG && !transfers);
    assert(solar_os_xpt2046_read(&io,400,&sample) == ESP_OK);
    assert(sample.valid && sample.touched && sample.x == 1001 && sample.y == 2000 && sample.pressure == 1695);
    assert(transfers == 21 && last_command == 0x90);
    for (unsigned failure = 1; failure <= 21; ++failure) {
        reset(); fail_at = failure;
        assert(solar_os_xpt2046_read(&io,400,&sample) == ESP_FAIL && !sample.valid);
        assert(last_command == 0x90);
    }
    reset(); weak = true;
    assert(solar_os_xpt2046_read(&io,400,&sample) == ESP_OK && sample.valid && !sample.touched && transfers == 5);
    reset(); floating = true;
    assert(solar_os_xpt2046_read(&io,400,&sample) == ESP_OK && sample.valid && !sample.touched && !x_reads);
    reset(); release_during_read = true;
    assert(solar_os_xpt2046_read(&io,400,&sample) == ESP_OK && sample.valid && !sample.touched);
    reset(); assert(solar_os_xpt2046_idle(&io) == ESP_OK && transfers == 1 && last_command == 0x90);
    puts("XPT2046 ADC: channel timing, median filtering, pressure, floating data, release and every transfer failure passed");
}
