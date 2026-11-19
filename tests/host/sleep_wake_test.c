#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_power.h"
#include "solar_os_gpio_controller.h"

#define GPIO_NUM_0 0
#define GPIO_NUM_2 2
#define GPIO_NUM_3 3
#define GPIO_NUM_18 18
#define GPIO_NUM_NC -1
#include "board.inc"
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
#define pdMS_TO_TICKS(ms) (ms)
#define SOLAR_OS_LOGW(...) log_message(__VA_ARGS__)
#define SOLAR_OS_LOGI(...) log_message(__VA_ARGS__)
#define ESP_ERR_SLEEP_REJECT ESP_ERR_INVALID_STATE

typedef int gpio_num_t;
typedef int esp_sleep_wakeup_cause_t;
typedef int esp_sleep_ext1_wakeup_mode_t;
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
static bool ext0_active;
static bool display_suspended;
static unsigned display_suspends;
static uint32_t button_held_until;
static bool irq_latched, ack_failure, key_read_failure, display_irq, wake_button_held, wake_read_fails;
static unsigned acknowledgements;
solar_os_gpio_line_ref_t key_line;
bool key_line_ready, key_line_raw, key_line_pressed;
uint32_t key_line_polled_ms, key_line_changed_ms;
static solar_os_power_status_t status;
static jmp_buf deep_return;
static int key_release_action;
static bool key_irq_pending, key_pressed, key_long_press_fired, key_ignore_until_released;
static uint32_t key_pressed_ms, last_status_update_ms;
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
static bool key_level_is_pressed(int level);
static bool key_button_is_pressed(void) { return key_level_is_pressed(key_line_ready && key_line_pressed ? 0 : 1); }
static void update_status(void) {}
static void draw_terminal_if_needed(void) {}
static int gpio_get_level(gpio_num_t pin) { (void)pin; return irq_latched || now < line_active_until ? 0 : 1; }
static int rtc_gpio_get_level(gpio_num_t pin) { return gpio_get_level(pin); }
bool solar_os_gpio_line_parse(const char *name, solar_os_gpio_line_ref_t *line)
{ assert(!strcmp(name,"gpiox0:3"));strcpy(line->controller,"gpiox0");line->line=3;return true; }
esp_err_t solar_os_gpio_line_configure(const solar_os_gpio_line_ref_t *line,
    solar_os_gpio_line_mode_t mode, solar_os_gpio_line_pull_t pull)
{ assert(line->line==3 && mode==SOLAR_OS_GPIO_LINE_MODE_INPUT && pull==SOLAR_OS_GPIO_LINE_PULL_NONE);return ESP_OK; }
esp_err_t solar_os_gpio_line_read(const solar_os_gpio_line_ref_t *line,bool *level)
{ assert(line->line==3);*level=now>=button_held_until;return key_read_failure ? ESP_FAIL : ESP_OK; }
esp_err_t solar_os_gpio_line_acknowledge_interrupt(const solar_os_gpio_line_ref_t *line)
{ assert(line->line==3);acknowledgements++;if (ack_failure) return ESP_FAIL;irq_latched=false;return ESP_OK; }
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
static void exit_suspend(const char *reason)
{ (void)reason; status.suspend_active = display_suspended = false; wakes++; }
static void resume_display_after_sleep(uint32_t tick) { (void)tick; assert(!display_suspended); resumes++; }
void solar_os_power_note_activity(uint32_t tick) { status.last_activity_ms = tick; }
void solar_os_power_note_sleep_enter(uint32_t tick) { (void)tick; }
void solar_os_power_note_sleep_exit(uint32_t tick, int cause, uint64_t mask, bool success)
{ (void)tick; (void)cause; assert(mask == (ext0_active ? 0 : wake_mask) && success); }
esp_err_t solar_os_power_hold_automatic_light_sleep(uint32_t duration) { (void)duration; return ESP_OK; }
static esp_err_t solar_os_display_show_power_state(const char *text) { assert(text[0]); return ESP_OK; }
static esp_err_t solar_os_display_suspend_primary(void)
{ display_suspended=true;display_suspends++;if(display_irq)irq_latched=true;return ESP_OK; }
static esp_err_t solar_os_display_resume_primary(void) { display_suspended=false;return ESP_OK; }
static esp_err_t solar_os_display_set_overlay_active(void *display, bool active) { (void)display; assert(!active); return ESP_OK; }
static esp_err_t esp_sleep_disable_wakeup_source(int source)
{ assert(source == ESP_SLEEP_WAKEUP_ALL); active_wake_mask = 0; return ESP_OK; }
static esp_err_t esp_sleep_pd_config(int domain, int option) { (void)domain; (void)option; return ESP_OK; }
static esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask, int mode)
{ ext0_active=false;active_wake_mask = wake_mask = mask; wake_mode = mode; return wake_failure ? ESP_FAIL : ESP_OK; }
esp_err_t esp_sleep_enable_ext0_wakeup(int pin,int level)
{ assert(pin==2 && !level);ext0_active=true;active_wake_mask=wake_mask=1ULL<<pin;wake_mode=ESP_EXT1_WAKEUP_ANY_LOW;return wake_failure ? ESP_FAIL : ESP_OK; }
static esp_err_t esp_sleep_enable_timer_wakeup(uint64_t delay) { assert(delay == 1000000); return ESP_OK; }
static bool solar_os_schedule_next_wake_us(uint64_t *delay) { *delay = 1000000; return true; }
static esp_err_t solar_os_rtc_get_info(solar_os_rtc_info_t *info)
{ *info = (solar_os_rtc_info_t){.interrupt_gpio = extra_rtc, .interrupt_active_level = 0}; return ESP_OK; }
static esp_err_t esp_light_sleep_start(void) {
    assert(!irq_latched && now>=button_held_until);sleeps++;
    if(wake_button_held)button_held_until=now+1000;
    if(wake_read_fails)key_read_failure=true;
    return ESP_OK;
}
static void esp_deep_sleep_start(void) { assert(!irq_latched && now>=button_held_until);deeps++;longjmp(deep_return,1); }
static int esp_sleep_get_wakeup_cause(void) { return ext0_active ? 2 : 3; }
static uint64_t esp_sleep_get_ext1_wakeup_status(void) { return ext0_active ? 0 : active_wake_mask; }

#include "sleep.inc"

int main(void)
{
    key_capability = SOLAR_OS_BOARD_HAS_KEY;
    assert(key_button_configure_gpio()==ESP_OK);
    enter_suspend("shell");
    assert(status.suspend_active && suspends == 1);
    maybe_exit_suspend_for_input(); assert(status.suspend_active);
    input_generation++;
    maybe_exit_suspend_for_input(); assert(!status.suspend_active && wakes == 1);
    /* A wired keyboard supports suspend even with no physical KEY. */
    key_capability = false;
    enter_suspend("shell");
    assert(status.suspend_active == (SOLAR_OS_BOARD_HAS_WIRED_KEYBOARD != 0));
    status.suspend_active = display_suspended = false; key_capability = SOLAR_OS_BOARD_HAS_KEY;
    const unsigned suspends_before_light = display_suspends;

    enter_sleep("shell", false);
    assert(sleeps == 1 && resumes == 1 && ends == 1 && !status.explicit_sleep_active);
    assert(display_suspends == suspends_before_light + SOLAR_OS_BOARD_SLEEP_SUSPEND_DISPLAY && !display_suspended);
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
#ifdef SOLAR_OS_BOARD_KEY_GPIO_LINE
    assert(KEY_WAKE_MASK==(1ULL<<2) && SLEEP_WAKE_MASK==KEY_WAKE_MASK);
    unsigned previous=sleeps;
    irq_latched=true;
    enter_sleep("shared IRQ",false);
    assert(sleeps==previous+1 && !irq_latched && acknowledgements>0);
    button_held_until=now+SLEEP_WAKE_RELEASE_TIMEOUT_MS+100;
    enter_sleep("held wheel with cleared IRQ",false);
    assert(sleeps==previous+1);button_held_until=0;
    ack_failure=true;enter_sleep("failed IRQ acknowledgement",false);ack_failure=false;
    assert(sleeps==previous+1);
    key_read_failure=true;enter_sleep("failed key read",false);key_read_failure=false;
    assert(sleeps==previous+1);
    /* Display rail changes after the earlier release check must be drained. */
    display_irq=true;
    enter_sleep("display IRQ",false);
    assert(sleeps==previous+2 && !irq_latched);
    unsigned deep_before=deeps;
    if(!setjmp(deep_return)){enter_sleep("deep display IRQ",true);assert(false);}
    assert(deeps==deep_before+1 && !irq_latched);status.explicit_sleep_active=false;
    display_irq=false;wake_button_held=true;
    enter_sleep("wake hold",false);
    assert(sleeps==previous+3 && key_ignore_until_released && key_line_pressed);
    wake_button_held=false;button_held_until=0;
    assert(key_button_configure_gpio()==ESP_OK && !key_button_is_pressed());
    wake_read_fails=true;
    enter_sleep("first post-wake read failed",false);
    assert(sleeps==previous+4 && key_line_ready && key_ignore_until_released && key_line_pressed);
    key_read_failure=wake_read_fails=false;
    assert(key_button_configure_gpio()==ESP_OK && !key_button_is_pressed());
#endif
    puts("Board wake input, suspend, light/deep sleep and rollback passed");
    return 0;
}
