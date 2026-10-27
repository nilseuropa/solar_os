from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function(path, name):
    source = (ROOT / path).read_text()
    match = re.search(r"^(?:static )?(?:esp_err_t|void|bool) " + name
                      + r"\([^;]*?\n\{.*?^\}", source, re.M | re.S)
    assert match, name
    return match.group()


def run_c(source, *flags):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        (path / "test.c").write_text(source)
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-I", str(ROOT / "tests/host"), *flags,
                        str(path / "test.c"), "-o", str(path / "test")],
                       check=True, capture_output=True, text=True)
        subprocess.run([str(path / "test")], check=True)


class ShutdownPolicyTest(unittest.TestCase):
    def test_resume_redraws_foreground_auxiliary_and_legacy_apps(self):
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SOLAR_OS_SESSION_MAX 3
#define SOLAR_OS_EVENT_RESUME 1
typedef struct { int type; struct { uint32_t tick_ms; } data; } solar_os_event_t;
typedef struct { void (*resume)(void *); } app_t;
typedef struct {
    bool used, suspended, started, terminal_redraw_requested;
    int *terminal;
    const app_t *app;
} solar_os_session_entry_t;
typedef struct { int unused; } solar_os_session_context_snapshot_t;
static struct {
    solar_os_session_entry_t sessions[3];
    solar_os_session_entry_t *foreground_session;
    const app_t *foreground_app;
    void *ctx;
} session_state;
static int paints[3], legacy, restored, requests, addressed;
static void session_context_capture(solar_os_session_context_snapshot_t *s) { (void)s; }
static void session_context_restore(const solar_os_session_context_snapshot_t *s) { (void)s; }
static void session_prepare_context(solar_os_session_entry_t *s) { assert(s->used); }
static bool process_addressed_session_requests(solar_os_session_entry_t *s) { assert(s->used); addressed++; return false; }
static void session_draw_terminal_if_needed(solar_os_session_entry_t *s) { assert(s->used); }
static void solar_os_terminal_invalidate_render(int *terminal) { (*terminal)++; }
static void dispatch_session_event(solar_os_session_entry_t *s, const solar_os_event_t *e) {
    assert(e->type == SOLAR_OS_EVENT_RESUME && e->data.tick_ms == 123);
    /* A dedicated resume callback is selected by the redraw request. */
    assert(s->terminal_redraw_requested);
    s->terminal_redraw_requested = false; paints[s - session_state.sessions]++;
}
static void solar_os_sessions_dispatch_foreground_event(const solar_os_event_t *e) {
    assert(session_state.foreground_session);
    dispatch_session_event(session_state.foreground_session, e);
}
static void restore_foreground_context(void) { restored++; }
static void solar_os_sessions_process_requests(void) { requests++; }
static void resume_legacy(void *ctx) { assert(ctx == &legacy && restored); legacy++; }
''' + function("src/services/solar_os_sessions.c", "solar_os_sessions_dispatch_resume") + r'''
int main(void) {
    int terminal[3] = {0};
    const app_t app = {.resume = resume_legacy};
    for (int i = 0; i < 3; i++) session_state.sessions[i] = (solar_os_session_entry_t){
        .used = true, .started = true, .terminal = terminal + i, .app = &app};
    session_state.sessions[2].suspended = true;
    session_state.foreground_session = session_state.sessions;
    solar_os_sessions_dispatch_resume(123);
    assert(paints[0] == 1 && paints[1] == 1 && paints[2] == 0);
    assert(addressed == 1);
    assert(terminal[0] == 1 && terminal[1] == 1 && terminal[2] == 0);
    session_state.foreground_session = NULL;
    session_state.foreground_app = &app; session_state.ctx = &legacy;
    solar_os_sessions_dispatch_resume(123);
    assert(legacy == 1 && restored == 1 && requests == 2 && addressed == 3);
}
''')

    def test_storage_images_before_parent_and_failures_stop_cleanup(self):
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
enum { SOLAR_OS_STORAGE_MOUNT_IMAGE, SOLAR_OS_STORAGE_MOUNT_RAMFS, FLASH };
typedef struct { int type; const char *mount_point; } solar_os_storage_mount_info_t;
static solar_os_storage_mount_info_t mounts[3];
static size_t count;
static int calls;
static esp_err_t unmount_error;
static bool remove_mount = true;
static size_t solar_os_storage_mount_count(void) { return count; }
static bool solar_os_storage_get_mount(size_t i, solar_os_storage_mount_info_t *out) {
    if (i >= count) return false;
    *out = mounts[i]; return true;
}
static esp_err_t solar_os_storage_unmount_volume(const char *path) {
    calls++;
    if (unmount_error) return unmount_error;
    if (!remove_mount) return ESP_OK;
    for (size_t i = 0; i < count; i++) if (!strcmp(mounts[i].mount_point, path)) {
        if (mounts[i].type == FLASH) assert(count == 2);
        assert(mounts[i].type != SOLAR_OS_STORAGE_MOUNT_RAMFS);
        memmove(mounts + i, mounts + i + 1, (--count - i) * sizeof(*mounts)); return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
''' + function("src/services/solar_os_storage.c", "solar_os_storage_prepare_shutdown") + r'''
static void reset(void) {
    mounts[0] = (solar_os_storage_mount_info_t){FLASH, "/flash"};
    mounts[1] = (solar_os_storage_mount_info_t){SOLAR_OS_STORAGE_MOUNT_RAMFS, "/ram"};
    mounts[2] = (solar_os_storage_mount_info_t){SOLAR_OS_STORAGE_MOUNT_IMAGE, "/image"};
    count = 3; calls = 0;
}
int main(void) {
    reset(); assert(solar_os_storage_prepare_shutdown() == ESP_OK && count == 1 && calls == 2);
    reset(); unmount_error = ESP_FAIL;
    assert(solar_os_storage_prepare_shutdown() == ESP_FAIL && count == 3 && calls == 1);
    reset(); unmount_error = ESP_OK; remove_mount = false;
    assert(solar_os_storage_prepare_shutdown() == ESP_ERR_INVALID_STATE && count == 3 && calls == 1);
}
''')

    def test_power_splash_on_primary_and_auxiliary_then_resume(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#define SOLAR_OS_BOARD_HAS_DISPLAY 0
#define SOLAR_OS_DISPLAY_TARGET_MAX 3
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portMAX_DELAY 0
#define pdTRUE 1
typedef struct { int rendered, sent, published, awake, id; } u8g2_t;
typedef struct { u8g2_t *u8g2; } solar_os_gfx_t;
typedef int SemaphoreHandle_t;
typedef struct {
    bool ready, black_is_one;
    u8g2_t *u8g2;
    esp_err_t (*present_power_state)(void *);
    void *power_state_context;
} solar_os_display_target_t;
typedef struct {
    bool active, power_state_active, overlay_active, overlay_pending;
    uint32_t generation;
    size_t refs;
    SemaphoreHandle_t present_mutex;
    solar_os_display_target_t target;
} display_target_slot_t;
static display_target_slot_t display_targets[3];
static int display_targets_lock, held;
static esp_err_t hook_error;
static int xSemaphoreTake(int mutex, int timeout) { assert(mutex && !held); (void)timeout; held = mutex; return pdTRUE; }
static void xSemaphoreGive(int mutex) { assert(held == mutex); held = 0; }
static void display_release_slot_ref(size_t i, uint32_t gen) { assert(display_targets[i].generation == gen); display_targets[i].refs--; }
static void solar_os_gfx_init(solar_os_gfx_t *g, u8g2_t *u) { g->u8g2 = u; }
static void solar_os_gfx_set_black_is_one(solar_os_gfx_t *g, bool black) { (void)g; (void)black; }
static void solar_os_splash_render(solar_os_gfx_t *g, const char *status) { assert(status && held); g->u8g2->rendered++; }
static void u8g2_SendBuffer(u8g2_t *u) { assert(held); u->sent++; }
static void u8g2_SetPowerSave(u8g2_t *u, int state) { assert(held); assert(state == 0); u->awake++; }
static void display_publish_frame(u8g2_t *u) { assert(held && display_targets[u->id].refs == 1); u->published++; }
static esp_err_t synchronous_hook(void *context) { u8g2_t *u = context; assert(held && u->rendered); u->sent++; return hook_error; }
'''
        for name in ("solar_os_display_show_power_state", "solar_os_display_clear_power_state"):
            source += function("src/services/solar_os_display.c", name) + "\n"
        run_c(source + r'''
int main(void) {
    u8g2_t primary = {.id = 0}, aux = {.id = 1}, unused = {.id = 2};
    display_targets[0] = (display_target_slot_t){.active = true, .generation = 1, .present_mutex = 1,
        .target = {.ready = true, .u8g2 = &primary}};
    display_targets[1] = (display_target_slot_t){.active = true, .generation = 1, .present_mutex = 2,
        .target = {.ready = true, .u8g2 = &aux, .present_power_state = synchronous_hook, .power_state_context = &aux}};
    display_targets[2].target.u8g2 = &unused;
    assert(solar_os_display_show_power_state("Sleeping") == ESP_OK);
    assert(primary.sent == 1 && aux.sent == 1 && unused.sent == 0);
    assert(primary.published == 1 && aux.published == 1);
    assert(display_targets[0].power_state_active && display_targets[1].power_state_active);
    solar_os_display_clear_power_state();
    assert(primary.awake == 1 && aux.awake == 1 && unused.awake == 0);
    assert(!display_targets[0].power_state_active && !display_targets[1].power_state_active);
    hook_error = ESP_FAIL;
    assert(solar_os_display_show_power_state("Powered off") == ESP_FAIL);
    assert(aux.published == 1 && display_targets[1].refs == 0 && held == 0);
}
''')

    def test_cl32_exact_command_and_transport_errors(self):
        run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#define CL32_CORE_REG_STATUS 0x01
typedef struct { bool active; const char *i2c_bus; uint8_t address; } solar_os_cl32_core_device_t;
static int writes;
static esp_err_t result;
static esp_err_t solar_os_bus_i2c_write_reg(const char *bus, uint8_t addr,
                                           uint8_t reg, const uint8_t *data, size_t size) {
    assert(bus && addr == 0x55 && reg == 0x01 && size == 1 && *data == 0xff);
    writes++;
    return result;
}
''' + function("src/services/solar_os_cl32_core.c", "power_off") + r'''
int main(void) {
    solar_os_cl32_core_device_t d = {true, "i2c0", 0x55};
    assert(power_off(&d) == ESP_OK && writes == 1);
    result = ESP_FAIL;
    assert(power_off(&d) == ESP_FAIL && writes == 2);
    d.active = false;
    assert(power_off(&d) == ESP_ERR_INVALID_STATE && writes == 2);
    assert(power_off(NULL) == ESP_ERR_INVALID_STATE);
}
''')

    def test_board_provider_selection_and_detach(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define SOLAR_OS_BOARD_POWEROFF_PROVIDER "power0"
typedef esp_err_t (*solar_os_power_off_fn_t)(void *);
static int off_lock, calls;
static solar_os_power_off_fn_t off_provider;
static void *off_user;
static esp_err_t callback(void *user) { assert(user); calls++; return ESP_FAIL; }
'''
        for name in ("solar_os_power_register_off_provider",
                     "solar_os_power_unregister_off_provider", "solar_os_power_off"):
            source += function("src/services/solar_os_power.c", name) + "\n"
        run_c(source + r'''
int main(void) {
    int a, b;
    assert(solar_os_power_off() == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_power_register_off_provider("expansion-pmic", callback, &a) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_power_register_off_provider("power0", callback, &a) == ESP_OK);
    assert(solar_os_power_register_off_provider("power0", callback, &b) == ESP_ERR_INVALID_STATE);
    solar_os_power_unregister_off_provider(&b);
    assert(solar_os_power_off() == ESP_FAIL && calls == 1);
    solar_os_power_unregister_off_provider(&a);
    assert(solar_os_power_off() == ESP_ERR_NOT_SUPPORTED && calls == 1);
}
''')

    def test_shutdown_key_only_cold_boot_fallback(self):
        source = (ROOT / "src/main.c").read_text()
        power = source.split("case SOLAR_OS_SHUTDOWN_POWER: {", 1)[1].split(
            "\n    default:", 1)[0].rsplit("}", 1)[0]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"
#define ESP_SLEEP_WAKEUP_ALL 99
#define ESP_PD_DOMAIN_RTC_PERIPH 1
#define ESP_PD_OPTION_ON 1
#define SOLAR_OS_BOARD_CAP_KEY 1
#define SOLAR_OS_BOARD_PIN_KEY 4
#define KEY_WAKE_MASK (1ULL << 4)
#define KEY_WAKE_MODE 1
#define KEY_RELEASE_STABLE_MS 50
#define KEY_RELEASE_STABLE_TIMEOUT_MS 1000
#define pdMS_TO_TICKS(x) (x)
static int disabled, enabled, slept, cutoff, delays;
static bool key, rtc_valid, released = true;
static esp_err_t key_result, off_result;
static bool board_has(int cap) { assert(cap == 1); return key; }
static bool rtc_gpio_is_valid_gpio(int pin) { assert(pin == 4); return rtc_valid; }
static esp_err_t esp_sleep_disable_wakeup_source(int src) { assert(src == 99); disabled++; return ESP_OK; }
static esp_err_t key_prepare_rtc_wakeup(void) { return key_result; }
static bool wait_key_rtc_released_stable(int stable, int timeout) { assert(stable == 50 && timeout == 1000); return released; }
static esp_err_t esp_sleep_pd_config(int domain, int option) { assert(domain == 1 && option == 1); return ESP_OK; }
static esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask, int mode) { assert(mask == KEY_WAKE_MASK && mode == 1); enabled++; return ESP_OK; }
static esp_err_t solar_os_power_off(void) { cutoff++; return off_result; }
static void vTaskDelay(int delay) { assert(delay == 100); delays++; }
static void esp_deep_sleep_start(void) { slept++; }
static esp_err_t power_step(void) {
''' + power + r'''
}
int main(void) {
    off_result = ESP_ERR_NOT_SUPPORTED;
    assert(power_step() == ESP_FAIL && slept == 1 && enabled == 0 && disabled == 1);
    key = rtc_valid = true;
    assert(power_step() == ESP_FAIL && slept == 2 && enabled == 1 && disabled == 2);
    off_result = ESP_OK;
    assert(power_step() == ESP_FAIL && slept == 3 && delays == 1);
    off_result = ESP_FAIL;
    assert(power_step() == ESP_FAIL && slept == 3);
    key_result = ESP_ERR_TIMEOUT;
    assert(power_step() == ESP_ERR_TIMEOUT && slept == 3 && cutoff == 4);
    key_result = ESP_OK; rtc_valid = false; off_result = ESP_ERR_NOT_SUPPORTED;
    assert(power_step() == ESP_FAIL && slept == 4 && enabled == 3);
    rtc_valid = true; released = false;
    assert(power_step() == ESP_ERR_NOT_FINISHED && slept == 4 && cutoff == 5);
}
''')


if __name__ == "__main__":
    unittest.main()
