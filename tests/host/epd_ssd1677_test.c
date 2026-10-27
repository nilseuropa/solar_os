#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Test the real command/refresh implementation with a captured SPI transport. */
#include "../../src/drivers/epd_ssd1677.c"

typedef struct {
    uint8_t command;
    size_t length;
    uint8_t data[SSD1677_BUFFER_BYTES];
} captured_command_t;

static captured_command_t commands[128];
static size_t command_count;
static int dc_level, reset_level = 1;
static int reset_count, power_count;
static bool stuck_busy;
static int fail_after = -1;
static int64_t time_us;
static uint8_t frame[SSD1677_BUFFER_BYTES], shadow[SSD1677_BUFFER_BYTES];
static uint8_t line[SSD1677_PANEL_ROW_BYTES];

void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    time_us += (int64_t)ticks * SOLAR_OS_EPD_TEST_TICK_MS * 1000;
}

int64_t esp_timer_get_time(void) { return time_us; }
int gpio_get_level(gpio_num_t pin) { (void)pin; return stuck_busy ? 1 : 0; }

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    if (pin == 9) dc_level = (int)level;
    if (pin == 46) {
        reset_level = (int)level;
        if (level == 0) reset_count++;
    }
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *transaction)
{
    assert(device != NULL);
    if (fail_after == 0) { fail_after = -1; return ESP_FAIL; }
    if (fail_after > 0) --fail_after;
    const uint8_t *data = transaction->flags & SPI_TRANS_USE_TXDATA ?
        transaction->tx_data : transaction->tx_buffer;
    size_t length = transaction->length / 8U;
    if (dc_level == 0) {
        assert(length == 1 && command_count < 128);
        commands[command_count++] = (captured_command_t){.command = data[0]};
    } else {
        assert(command_count > 0);
        captured_command_t *command = &commands[command_count - 1U];
        assert(command->length + length <= sizeof(command->data));
        memcpy(command->data + command->length, data, length);
        command->length += length;
    }
    return ESP_OK;
}

static esp_err_t set_power(void *context, bool on)
{
    (void)context;
    assert(on && command_count == 0);
    power_count++;
    return ESP_OK;
}

static epd_ssd1677_t make_display(void)
{
    memset(frame, 0xff, sizeof(frame)); /* white */
    memset(shadow, 0, sizeof(shadow));
    command_count = 0;
    return (epd_ssd1677_t) {
        .spi = (void *)1, .buffer = frame, .shadow = shadow, .line_buffer = line,
        .buffer_size = sizeof(frame), .shadow_size = sizeof(shadow),
        .line_buffer_size = sizeof(line), .dc_pin = 9, .reset_pin = 46,
        .busy_pin = 3, .busy_level = 1, .power_pin = -1,
        .set_power = set_power,
    };
}

static captured_command_t *find_command(uint8_t command, size_t occurrence)
{
    for (size_t i = 0; i < command_count; i++) {
        if (commands[i].command == command && occurrence-- == 0) return &commands[i];
    }
    assert(!"missing command");
    return NULL;
}

static void expect_data(uint8_t command, const uint8_t *data, size_t length)
{
    captured_command_t *capture = find_command(command, 0);
    assert(capture->length == length && memcmp(capture->data, data, length) == 0);
}

static void test_initial_full(void)
{
    epd_ssd1677_t display = make_display();
    /* Three distinct marks prove conversion order and bottom/top gate mapping. */
    frame[0] &= (uint8_t)~1U;
    frame[799] &= (uint8_t)~1U;
    frame[sizeof(frame) - 1U] &= (uint8_t)~0x80U;
    assert(ssd1677_refresh(&display) == ESP_OK);
    assert(power_count == 1 && reset_count == 1 && reset_level == 1);
    expect_data(0x11, (uint8_t[]){0x01}, 1);
    expect_data(0x44, (uint8_t[]){0, 0, 0x1f, 3}, 4);
    expect_data(0x45, (uint8_t[]){0xdf, 1, 0, 0}, 4);
    expect_data(0x4f, (uint8_t[]){0xdf, 1}, 2);
    expect_data(0x46, (uint8_t[]){0xf7}, 1);
    expect_data(0x47, (uint8_t[]){0xf7}, 1);
    expect_data(0x21, (uint8_t[]){0}, 1);
    expect_data(0x22, (uint8_t[]){0xfc}, 1);
    assert(find_command(0x3c, 1)->data[0] == 0xc0);
    const captured_command_t *current = find_command(0x24, 0);
    const captured_command_t *baseline = find_command(0x26, 0);
    assert(current->length == sizeof(frame) && baseline->length == sizeof(frame));
    assert(current->data[0] == 0x7f && current->data[99] == 0xfe);
    assert(current->data[sizeof(frame) - 1U] == 0xfe);
    for (size_t i = 0; i < sizeof(frame); i++) {
        assert((current->data[i] ^ baseline->data[i]) == 0xff);
    }
    assert(memcmp(find_command(0x24, 1)->data, current->data, sizeof(frame)) == 0);
    assert(memcmp(find_command(0x26, 1)->data, current->data, sizeof(frame)) == 0);
    assert(display.shadow_valid && memcmp(frame, shadow, sizeof(frame)) == 0);
    command_count = 0;
    assert(ssd1677_refresh(&display) == ESP_OK && command_count == 0);
}

static void test_partial_and_cleanup(void)
{
    epd_ssd1677_t display = make_display();
    display.controller_ready = true;
    display.shadow_valid = true;
    memcpy(shadow, frame, sizeof(frame));
    /* Dirty byte at x=16..23, logical y=10: controller gate Y=469. */
    frame[800 + 17] &= (uint8_t)~4U;
    const int resets_before = reset_count;
    assert(ssd1677_refresh(&display) == ESP_OK);
    assert(reset_count == resets_before);
    expect_data(0x44, (uint8_t[]){16, 0, 23, 0}, 4);
    expect_data(0x45, (uint8_t[]){0xd5, 1, 0xd5, 1}, 4);
    expect_data(0x4f, (uint8_t[]){0xd5, 1}, 2);
    expect_data(0x24, (uint8_t[]){0xbf}, 1);
    expect_data(0x26, (uint8_t[]){0xff}, 1);
    assert(find_command(0x26, 1)->data[0] == 0xbf);
    assert(display.partial_refresh_count == 1);

    /* Periodic cleanup must force a full transition without resetting RAM. */
    display.partial_refresh_count = SSD1677_AUTO_FULL_INTERVAL - 1U;
    frame[0] &= (uint8_t)~1U;
    command_count = 0;
    assert(ssd1677_refresh(&display) == ESP_OK);
    assert(find_command(0x24, 0)->length == sizeof(frame));
    assert(find_command(0x26, 0)->data[0] == 0x80);
    assert(display.partial_refresh_count == 0 && reset_count == resets_before);
}

static void test_failure_and_no_shadow(void)
{
    epd_ssd1677_t display = make_display();
    display.controller_ready = true;
    display.shadow_valid = true;
    memcpy(shadow, frame, sizeof(frame));
    frame[0] &= (uint8_t)~1U;
    stuck_busy = true;
    assert(ssd1677_refresh(&display) == ESP_ERR_TIMEOUT);
    assert(!display.shadow_valid && !display.controller_ready);
    assert(shadow[0] == 0xff);
    stuck_busy = false;

    display = make_display();
    display.controller_ready = true;
    display.shadow = NULL;
    display.shadow_valid = true;
    assert(ssd1677_refresh(&display) == ESP_OK);
    assert(find_command(0x24, 0)->length == sizeof(frame));
    assert(find_command(0x26, 0)->data[0] == 0);
}

#define RELAX_TYPE epd_ssd1677_t
#define RELAX_REFRESH ssd1677_refresh
#define RELAX_APPLY epd_ssd1677_relax
#define RELAX_MODE epd_ssd1677_set_controller_mode
#define RELAX_PARTIAL EPD_SSD1677_REFRESH_PARTIAL
#define RELAX_COUNT command_count
#define RELAX_WIRE find_command(0x24, 0)->data
#include "epd_relax_contract.h"

int main(void)
{
    test_initial_full();
    test_partial_and_cleanup();
    test_failure_and_no_shadow();
    test_relax_transaction();
    puts("epd_ssd1677_test: PASS");
    return 0;
}
