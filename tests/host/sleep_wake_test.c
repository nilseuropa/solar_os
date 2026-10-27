#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "solar_os_power.h"

#define GPIO_NUM_0 0
#define GPIO_NUM_3 3
#define GPIO_NUM_18 18
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_INTR_ANYEDGE 3
#define RTC_GPIO_MODE_INPUT_ONLY 1
#define ESP_EXT1_WAKEUP_ANY_LOW 1
#define ESP_EXT1_WAKEUP_ANY_HIGH 2
#define ESP_PD_DOMAIN_RTC_PERIPH 0
#define ESP_PD_OPTION_ON 1
#define ESP_PD_OPTION_AUTO 0
#define ESP_SLEEP_WAKEUP_ALL 0
#define SOLAR_OS_RTC_INTERRUPT_GPIO_NONE -1
#define SOLAR_OS_BOARD_CAP_KEY 1
#define KEY_RELEASE_STABLE_MS 60
#define KEY_RELEASE_STABLE_TIMEOUT_MS 600
#define RADIO_RESUME_PM_HOLDOFF_MS 15000
#define KEY_WAKE_MASK (1ULL << SOLAR_OS_BOARD_PIN_KEY)
#define pdMS_TO_TICKS(ms) (ms)
#define SOLAR_OS_LOGW(...) log_message(__VA_ARGS__)
#define SOLAR_OS_LOGI(...) log_message(__VA_ARGS__)
#define ESP_ERR_SLEEP_REJECT ESP_ERR_INVALID_STATE

typedef int gpio_num_t;
typedef int esp_sleep_wakeup_cause_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef struct { int interrupt_gpio, interrupt_active_level; } solar_os_rtc_info_t;
typedef struct { int unused; } solar_os_input_key_event_t;

static const char *TAG = "test";
static uint32_t now = 1, held_until, line_active_until, input_generation;
static uint32_t prepare_line_active_ms;
static unsigned sleeps, deeps, resumes, suspends, ends, restores, wakes;
static uint64_t configured_mask, wake_mask, active_wake_mask;
static int wake_mode, extra_rtc = -1;
static bool gpio_failure, rtc_failure, wake_failure;
static bool key_capability;
static solar_os_power_status_t status;
static jmp_buf deep_return;
static int key_release_action;
static bool key_irq_pending, key_pressed, key_long_press_fired, key_ignore_until_released;
static uint32_t key_pressed_ms, last_app_tick_ms, last_status_update_ms;
static uint32_t suspend_input_key_press_generation;
static uint32_t session_overlay_until_ms, last_session_overlay_draw_ms;
static char session_overlay_title[48];
static bool session_overlay_persistent, session_overlay_after_next_frame, session_switch_alt_held;
static uint8_t session_switch_nav_held;
static void *session_overlay_u8g2;

static void log_message(const char *tag, const char *format, ...) { (void)tag; (void)format; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test"; }
static uint32_t millis_u32(void) { return now; }
static void vTaskDelay(unsigned ticks) { now += ticks; }
static bool board_has(int capability) { assert(capability == SOLAR_OS_BOARD_CAP_KEY); return key_capability; }
static bool key_button_is_pressed(void) { return false; }
static void update_status(void) {}
static void draw_terminal_if_needed(void) {}
static int gpio_get_level(gpio_num_t pin) { (void)pin; return now < line_active_until ? 0 : 1; }
static int rtc_gpio_get_level(gpio_num_t pin) { return gpio_get_level(pin); }
static esp_err_t gpio_config(const gpio_config_t *config)
{ assert(config->mode == GPIO_MODE_INPUT); configured_mask = config->pin_bit_mask; return gpio_failure ? ESP_FAIL : ESP_OK; }
static esp_err_t rtc_gpio_init(gpio_num_t pin) { (void)pin; return rtc_failure ? ESP_FAIL : ESP_OK; }
static esp_err_t rtc_gpio_deinit(gpio_num_t pin) { (void)pin; restores++; return ESP_OK; }
static esp_err_t rtc_gpio_set_direction(gpio_num_t pin, int mode) { (void)pin; assert(mode == RTC_GPIO_MODE_INPUT_ONLY); return ESP_OK; }
static esp_err_t rtc_gpio_pullup_en(gpio_num_t pin) { (void)pin; return ESP_OK; }
static esp_err_t rtc_gpio_pullup_dis(gpio_num_t pin) { (void)pin; return ESP_OK; }
static esp_err_t rtc_gpio_pulldown_en(gpio_num_t pin) { (void)pin; return ESP_OK; }
static esp_err_t rtc_gpio_pulldown_dis(gpio_num_t pin) { (void)pin; return ESP_OK; }
static size_t solar_os_input_get_pressed(solar_os_input_key_event_t *held, size_t count)
{ (void)held; assert(count == 1); return now < held_until ? 1 : 0; }
static uint32_t solar_os_input_key_press_generation(void) { return input_generation; }
void solar_os_power_get_status(solar_os_power_status_t *out) { *out = status; }
esp_err_t solar_os_power_begin_explicit_sleep(void)
{
    status.explicit_sleep_active = true;
    if (prepare_line_active_ms) line_active_until = now + prepare_line_active_ms;
    return ESP_OK;
}
esp_err_t solar_os_power_end_explicit_sleep(void) { status.explicit_sleep_active = false; ends++; return ESP_OK; }
esp_err_t solar_os_power_begin_suspend(void) { status.suspend_active = true; suspends++; return ESP_OK; }
esp_err_t solar_os_power_end_suspend(void) { status.suspend_active = false; return ESP_OK; }
const char *solar_os_power_profile_name(solar_os_power_profile_t profile) { (void)profile; return "test"; }
static void exit_suspend(const char *reason) { (void)reason; status.suspend_active = false; wakes++; }
static void resume_display_after_sleep(uint32_t tick) { (void)tick; resumes++; }
void solar_os_power_note_activity(uint32_t tick) { status.last_activity_ms = tick; }
void solar_os_power_note_sleep_enter(uint32_t tick) { (void)tick; }
void solar_os_power_note_sleep_exit(uint32_t tick, int cause, uint64_t mask, bool success)
{ (void)tick; (void)cause; assert(mask == wake_mask && success); }
esp_err_t solar_os_power_hold_automatic_light_sleep(uint32_t duration) { (void)duration; return ESP_OK; }
static esp_err_t solar_os_display_show_power_state(const char *text) { assert(text[0]); return ESP_OK; }
static esp_err_t solar_os_display_suspend_primary(void) { return ESP_OK; }
static esp_err_t solar_os_display_set_overlay_active(void *display, bool active) { (void)display; assert(!active); return ESP_OK; }
static esp_err_t esp_sleep_disable_wakeup_source(int source)
{ assert(source == ESP_SLEEP_WAKEUP_ALL); active_wake_mask = 0; return ESP_OK; }
static esp_err_t esp_sleep_pd_config(int domain, int option) { (void)domain; (void)option; return ESP_OK; }
static esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask, int mode)
{ active_wake_mask = wake_mask = mask; wake_mode = mode; return wake_failure ? ESP_FAIL : ESP_OK; }
static esp_err_t esp_sleep_enable_timer_wakeup(uint64_t delay) { assert(delay == 1000000); return ESP_OK; }
static bool solar_os_schedule_next_wake_us(uint64_t *delay) { *delay = 1000000; return true; }
static esp_err_t solar_os_rtc_get_info(solar_os_rtc_info_t *info)
{ *info = (solar_os_rtc_info_t){.interrupt_gpio = extra_rtc, .interrupt_active_level = 0}; return ESP_OK; }
static esp_err_t esp_light_sleep_start(void) { sleeps++; return ESP_OK; }
static void esp_deep_sleep_start(void) { deeps++; longjmp(deep_return, 1); }
static int esp_sleep_get_wakeup_cause(void) { return 3; }
static uint64_t esp_sleep_get_ext1_wakeup_status(void) { return active_wake_mask; }

#include "sleep.inc"

int main(void)
{
    key_capability = SOLAR_OS_BOARD_HAS_KEY;
    enter_suspend("shell");
    assert(status.suspend_active && suspends == 1);
    maybe_exit_suspend_for_input(); assert(status.suspend_active);
    input_generation++;
    maybe_exit_suspend_for_input(); assert(!status.suspend_active && wakes == 1);
    /* A wired keyboard supports suspend even with no physical KEY. */
    key_capability = false;
    enter_suspend("shell");
    assert(status.suspend_active == (SOLAR_OS_BOARD_HAS_WIRED_KEYBOARD != 0));
    status.suspend_active = false; key_capability = SOLAR_OS_BOARD_HAS_KEY;

    enter_sleep("shell", false);
    assert(sleeps == 1 && resumes == 1 && ends == 1 && !status.explicit_sleep_active);
    assert(configured_mask == SLEEP_WAKE_MASK && wake_mask == SLEEP_WAKE_MASK);
    assert(wake_mode == ESP_EXT1_WAKEUP_ANY_LOW);
    extra_rtc = 4;
    enter_sleep("shell", false);
    assert(sleeps == 2 && wake_mask == (SLEEP_WAKE_MASK | (1ULL << 4)));
    extra_rtc = -1;
    if (!setjmp(deep_return)) { enter_sleep("shell", true); assert(!"deep sleep returned"); }
    assert(deeps == 1 && sleeps == 2 && status.explicit_sleep_active);
    status.explicit_sleep_active = false;

    const unsigned before = sleeps;
    gpio_failure = true; enter_sleep("shell", false); gpio_failure = false;
    assert(sleeps == before);
    rtc_failure = true; enter_sleep("shell", false); rtc_failure = false;
    assert(sleeps == before && !status.explicit_sleep_active);
    wake_failure = true; enter_sleep("shell", false); wake_failure = false;
    assert(sleeps == before && !status.explicit_sleep_active);
    line_active_until = now + SLEEP_WAKE_RELEASE_TIMEOUT_MS + 100;
    enter_sleep("shell", false);
    assert(sleeps == before); line_active_until = 0;
    if (SOLAR_OS_BOARD_HAS_WIRED_KEYBOARD) {
        held_until = now + SLEEP_WAKE_RELEASE_TIMEOUT_MS + 100;
        enter_sleep("shell", false);
        assert(sleeps == before); held_until = 0;
    }
    /* Pending IRQs and releases may settle while preparing sleep. */
    line_active_until = now + 100; held_until = now + 150;
    enter_sleep("shell", false);
    assert(sleeps == before + 1 && !status.explicit_sleep_active && restores > 0);
    /* Repeated commands must tolerate a slower Enter release/IRQ drain,
     * without ever sleeping through a permanently held key or low line. */
    if (SOLAR_OS_BOARD_HAS_WIRED_KEYBOARD) {
        for (unsigned cycle = 0; cycle < 10; cycle++) {
            line_active_until = now + 700;
            held_until = now + 900;
            prepare_line_active_ms = 800;
            enter_sleep("shell", false);
            assert(sleeps == before + 2 + cycle && !status.explicit_sleep_active);
        }
        prepare_line_active_ms = SLEEP_WAKE_RELEASE_TIMEOUT_MS + 100;
        enter_sleep("shell", false);
        assert(sleeps == before + 11 && !status.explicit_sleep_active);
        prepare_line_active_ms = 0;
        line_active_until = 0;
    }
    puts("Board wake input, suspend, light/deep sleep and rollback passed");
    return 0;
}
