/* Exercise the real shared service with ordinary board button definitions. */
#define main input_service_tests
#include "input_test.c"
#undef main

#include "solar_os_buttons.h"
#include "solar_os_log.h"

static int levels[64];
esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->mode == GPIO_MODE_INPUT && config->intr_type == GPIO_INTR_DISABLE);
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin) { return levels[pin]; }
esp_err_t gpio_install_isr_service(int flags) { (void)flags; assert(false); return ESP_FAIL; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *arg)
{ (void)pin; (void)handler; (void)arg; assert(false); return ESP_FAIL; }
esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{ (void)pin; assert(false); return ESP_FAIL; }
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type)
{ (void)pin; (void)type; assert(false); return ESP_FAIL; }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; return ESP_OK; }

static void expect_no_events(void)
{
    solar_os_input_key_event_t event;
    assert(solar_os_input_read_events(&event, 1) == 0);
}

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    levels[4] = 1; /* Active-low press/release button. */
    levels[5] = 0; /* Active-high release-only button. */
    assert(solar_os_buttons_init() == ESP_OK);
    solar_os_buttons_poll();
    expect_no_events();

    levels[4] = 0;
    solar_os_buttons_poll();
    now_us += 10000;
    levels[4] = 1; /* Contact bounce never reaches stable press. */
    solar_os_buttons_poll();
    now_us += 30000;
    solar_os_buttons_poll();
    expect_no_events();

    levels[4] = 0;
    solar_os_buttons_poll();
    now_us += 24000;
    solar_os_buttons_poll();
    expect_no_events();
    now_us += 1000;
    solar_os_buttons_poll();
    solar_os_input_key_event_t event;
    assert(solar_os_input_read_events(&event, 1) == 1);
    assert(event.key == SOLAR_OS_KEY_UP && event.action == SOLAR_OS_INPUT_KEY_PRESS);
    now_us += 100000;
    solar_os_buttons_poll();
    expect_no_events();
    levels[4] = 1;
    solar_os_buttons_poll();
    now_us += 25000;
    solar_os_buttons_poll();
    assert(solar_os_input_read_events(&event, 1) == 1);
    assert(event.key == SOLAR_OS_KEY_UP && event.action == SOLAR_OS_INPUT_KEY_RELEASE);

    levels[5] = 1;
    solar_os_buttons_poll();
    now_us += 25000;
    solar_os_buttons_poll();
    expect_no_events();
    levels[5] = 0;
    solar_os_buttons_poll();
    now_us += 25000;
    solar_os_buttons_poll();
    solar_os_input_key_event_t tap[2];
    assert(solar_os_input_read_events(tap, 2) == 2);
    assert(tap[0].key == SOLAR_OS_KEY_ENTER && tap[0].action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(tap[1].key == SOLAR_OS_KEY_ENTER && tap[1].action == SOLAR_OS_INPUT_KEY_RELEASE);
    solar_os_buttons_poll();
    expect_no_events();
    puts("Ordinary buttons retain debounce and press/release behavior");
    return 0;
}
