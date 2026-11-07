#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef int gpio_int_type_t;
typedef void (*gpio_isr_t)(void *);
typedef void *EventGroupHandle_t;
typedef void *gptimer_handle_t;
#include "keyboard_button.h"
#define GPIO_MODE_OUTPUT_OD 3
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_INTR_LOW_LEVEL 2
#define GPIO_INTR_HIGH_LEVEL 3
#define GPIO_NUM_MAX 49
#define ESP_LOGE(...) ((void)0)
#define ESP_RETURN_ON_FALSE(test, error, ...) do { if (!(test)) return error; } while (0)
#define OUTPUT_MASK_HIGE UINT32_MAX
#define OUTPUT_MASK_LOW 0
#define CALL_EVENT_CB(ev) do { if ((ev) == KBD_EVENT_PRESSED) capture(report); } while (0)

static unsigned selected, columns, reports, configured, initial_levels[49];
static unsigned char switches[80];
static keyboard_btn_report_t last_report;
static void capture(keyboard_btn_report_t report) { last_report = report; reports++; }
static void esp_rom_delay_us(unsigned delay) { assert(delay >= 5); }
void kbd_gpios_set_hold_dis(const int *pins, uint32_t count) { (void)pins; (void)count; assert(false); }
void kbd_gpios_set_level(const int *pins, uint32_t count, uint32_t level)
{ (void)pins; (void)count; assert(level == UINT32_MAX); selected = 0; }
void kbd_gpio_set_level(int pin, uint32_t level)
{ if (level) selected &= ~(1U << pin); else selected |= 1U << pin; }
uint32_t kbd_gpios_read_level(const int *pins, uint32_t count)
{
    (void)pins; assert(count == columns && selected && !(selected & (selected - 1)));
    unsigned row = (unsigned)__builtin_ctz(selected), bits = 0;
    for (unsigned col = 0; col < columns; col++) if (!switches[row * columns + col]) bits |= 1U << col;
    return bits;
}
esp_err_t gpio_config(const gpio_config_t *config)
{
    unsigned pin = (unsigned)__builtin_ctzll(config->pin_bit_mask);
    if (config->mode != GPIO_MODE_INPUT) {
        assert(config->mode == GPIO_MODE_OUTPUT_OD && initial_levels[pin] == 1);
    } else assert(config->pull_up_en == GPIO_PULLUP_ENABLE);
    configured++;
    return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) { initial_levels[pin] = level; return ESP_OK; }
static esp_err_t gpio_wakeup_enable(int pin, int interrupt) { (void)pin; (void)interrupt; assert(false); return ESP_OK; }
static esp_err_t esp_sleep_enable_gpio_wakeup(void) { assert(false); return ESP_OK; }

/* TYPES */
/* GPIO_INIT */
/* SCANNER */

int main(void)
{
    int rows[8] = {0,1,2,3,4,5,6,7}, cols[10] = {8,9,10,11,12,13,14,15,16,17};
    kbd_gpio_config_t config = {.gpios=rows, .gpio_num=8, .gpio_mode=KBD_GPIO_MODE_OUTPUT, .active_level=0};
    assert(kbd_gpio_init(&config) == ESP_OK);
    config.gpios = cols; config.gpio_num = 10; config.gpio_mode = KBD_GPIO_MODE_INPUT;
    assert(kbd_gpio_init(&config) == ESP_OK && configured == 18);
    keyboard_btn_t kbd = {.input_gpios=cols, .input_gpio_num=10, .output_gpios=rows,
        .output_gpio_num=8, .debounce_ticks=2};
    kbd.button_level = calloc(80, 1); kbd.debounce_cnt = calloc(80, 1);
    kbd.key_data = calloc(80, sizeof(*kbd.key_data));
    kbd.key_release_data = calloc(80, sizeof(*kbd.key_release_data));
    assert(kbd.button_level && kbd.debounce_cnt && kbd.key_data && kbd.key_release_data);
    columns = 10;
    memset(switches, 1, sizeof(switches));
    kbd_handler(&kbd); assert(!reports);
    kbd_handler(&kbd); assert(reports == 1 && last_report.key_pressed_num == 80);
    memset(switches, 0, sizeof(switches));
    kbd_handler(&kbd); kbd_handler(&kbd);
    assert(reports == 2 && last_report.key_pressed_num == 0 && last_report.key_release_num == 80);
    unsigned random = 1;
    for (unsigned frame = 0; frame < 500; frame++) {
        unsigned expected = 0;
        for (unsigned key = 0; key < 80; key++) {
            random = random * 1664525U + 1013904223U;
            switches[key] = (random >> 31) != 0; expected += switches[key];
        }
        kbd_handler(&kbd); kbd_handler(&kbd);
        assert(kbd.key_pressed_num == expected);
    }
    free(kbd.button_level); free(kbd.debounce_cnt); free(kbd.key_data); free(kbd.key_release_data);
    puts("Espressif scanner: open-drain initialization, debounce, 80-key release and randomized snapshots PASS (ASan/UBSan)");
    return 0;
}
