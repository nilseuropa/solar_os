from pathlib import Path
import re
import unittest

from tests.test_shutdown_policy import function, run_c


ROOT = Path(__file__).resolve().parents[1]
FLAGS = ("-I", str(ROOT / "src/services"), "-I", str(ROOT / "include"))


class PowerKeyTest(unittest.TestCase):
    def test_runtime_gestures_and_wake_consumption(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "solar_os_power.h"
#include "solar_os_keys.h"
#define SOLAR_OS_PACKAGE_SERVICE_BLE 1
#define SOLAR_OS_BOARD_CAP_KEY 1
#define SOLAR_OS_BOARD_CAP_BLE 2
#define SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY 0x09
#define KEY_SHORT_PRESS_MIN_MS 30U
#define KEY_LONG_PRESS_MS 1200U
#define SOLAR_OS_LOGI(...) ((void)0)
#define SOLAR_OS_LOGW(...) ((void)0)
static bool down, key_interrupt_ready, key_irq_pending, key_pressed;
static bool key_long_press_fired, key_ignore_until_released, ble_enabled = true;
static uint32_t now, key_pressed_ms, last_status_update_ms;
static unsigned chars, pairs, forgets, suspends, wakes, sleeps, deeps;
static uint8_t last_char;
static int key_input_source = 1;
static solar_os_power_status_t status;
static solar_os_power_key_action_t key_release_action;
static bool board_has(int cap) { (void)cap; return true; }
static bool key_button_is_pressed(void) { return down; }
static uint32_t millis_u32(void) { return now; }
void solar_os_power_get_status(solar_os_power_status_t *out) { *out = status; }
void solar_os_power_note_activity(uint32_t ms) { status.last_activity_ms = ms; }
static bool solar_os_ble_keyboard_enabled_for_current_boot(void) { return ble_enabled; }
static esp_err_t solar_os_ble_keyboard_forget(void) { forgets++; return ESP_OK; }
static esp_err_t solar_os_ble_keyboard_start_pairing(void) { pairs++; return ESP_OK; }
static void update_status(void) { }
static void draw_terminal_if_needed(void) { }
static esp_err_t solar_os_input_write_char(int src, char ch) {
    assert(src == 1); chars++; last_char = (uint8_t)ch; return ESP_OK;
}
static void enter_light_sleep(const char *reason) {
    (void)reason; sleeps++; if (down) assert(key_ignore_until_released);
}
static void enter_deep_sleep(const char *reason) {
    (void)reason; deeps++; if (down) assert(key_ignore_until_released);
}
static void enter_suspend(const char *reason) {
    (void)reason; suspends++; status.suspend_active = true;
    key_ignore_until_released = down;
}
static void exit_suspend(const char *reason) {
    (void)reason; wakes++; status.suspend_active = false;
}
'''
        for name in ("handle_key_pairing", "handle_key_input", "handle_key_action", "handle_key_short_press",
                     "poll_key_button"):
            source += function("src/main.c", name) + "\n"
        run_c(source + r'''
static void poll(bool pressed, uint32_t elapsed) {
    down = pressed; now += elapsed; key_irq_pending = true; poll_key_button();
}
static void tap(void) { poll(true, 10U); poll(false, 50U); }
int main(void) {
    status.key_action = SOLAR_OS_POWER_KEY_ACTION_ENTER;
    status.key_long_action = SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR;
    tap(); assert(chars == 1U && last_char == SOLAR_OS_KEY_ENTER);
    poll(true, 10U); poll(true, 1200U);
    assert(pairs == 1U && forgets == 1U);
    poll(true, 5000U); poll(false, 10U); assert(pairs == 1U && chars == 1U);
    ble_enabled = false;
    poll(true, 10U); poll(true, 1200U); poll(false, 10U);
    assert(pairs == 1U && forgets == 1U); ble_enabled = true;
    status.key_long_action = SOLAR_OS_POWER_KEY_ACTION_SUSPEND;
    poll(true, 10U); poll(true, 1200U);
    assert(suspends == 1U && status.suspend_active);
    poll(true, 5000U); assert(status.suspend_active && wakes == 0U);
    poll(false, 10U); assert(status.suspend_active);
    const uint32_t durations[] = {1U, 29U, 30U, 1199U, 1200U, 5000U};
    for (unsigned i = 0; i < sizeof(durations) / sizeof(durations[0]); i++) {
        status.suspend_active = true;
        poll(true, 1U); assert(!status.suspend_active && wakes == i + 1U);
        poll(true, durations[i]); poll(false, 1U);
        assert(chars == 1U && pairs == 1U && suspends == 1U);
        assert(status.last_activity_ms == now);
    }
    /* A wake/boot key still held when GPIO monitoring starts is consumed. */
    key_ignore_until_released = true;
    poll(true, 5000U); poll(false, 1U); assert(pairs == 1U && chars == 1U);
    status.key_long_action = SOLAR_OS_POWER_KEY_ACTION_SLEEP;
    poll(true, 10U); poll(true, 1200U); poll(true, 5000U);
    assert(sleeps == 0U); poll(false, 1U);
    assert(sleeps == 1U && chars == 1U && pairs == 1U);
    status.key_long_action = SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP;
    poll(true, 10U); poll(true, 1200U); poll(true, 5000U);
    assert(deeps == 0U); poll(false, 1U); assert(deeps == 1U);
    status.key_action = SOLAR_OS_POWER_KEY_ACTION_SUSPEND;
    tap(); assert(status.suspend_active && suspends == 2U);
    poll(true, 1U); poll(false, 1U); assert(!status.suspend_active);
    status.key_action = SOLAR_OS_POWER_KEY_ACTION_APP_EXIT;
    tap(); assert(chars == 2U && last_char == SOLAR_OS_KEY_APP_EXIT);
    status.key_action = SOLAR_OS_POWER_KEY_ACTION_OFF;
    tap(); assert(chars == 2U && suspends == 2U);
    status.key_action = SOLAR_OS_POWER_KEY_ACTION_SLEEP;
    tap(); assert(sleeps == 2U);
    status.key_action = SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP;
    tap(); assert(deeps == 2U);
    status.key_action = SOLAR_OS_POWER_KEY_ACTION_DEFAULT;
    tap(); assert(chars == 3U && last_char == 0x09);
}
''', *FLAGS)

    def test_cli_and_tui_select_independent_actions(self):
        command = (ROOT / "src/shell/solar_os_shell_commands.c").read_text()
        start = command.index('    if (strcmp(argv[1], "powerkey") == 0')
        end = command.index('    if (strcmp(argv[1], "keyrate")', start)
        source = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_power.h"
typedef void solar_os_shell_io_t;
static solar_os_power_status_t status;
static unsigned short_saves, long_saves, errors;
static char output[512], edit_text[32];
void solar_os_power_get_status(solar_os_power_status_t *out) { *out = status; }
esp_err_t solar_os_power_set_key_action(solar_os_power_key_action_t action) {
    status.key_action = action; short_saves++; return ESP_OK;
}
esp_err_t solar_os_power_set_key_long_action(solar_os_power_key_action_t action) {
    status.key_long_action = action; long_saves++; return ESP_OK;
}
static void solar_os_shell_io_printf(void *io, const char *fmt, ...) {
    (void)io; va_list args; va_start(args, fmt);
    vsnprintf(output + strlen(output), sizeof(output) - strlen(output), fmt, args);
    va_end(args);
}
static void solar_os_shell_diag_unexpected(void *io, const char *cmd, const char *arg, const char *usage) {
    (void)io; (void)cmd; (void)arg; (void)usage; errors++;
}
static void solar_os_shell_diag_invalid(void *io, const char *cmd, const char *kind,
    const char *arg, const char *values, const char *usage, bool secret) {
    (void)io; (void)cmd; (void)kind; (void)arg; (void)values; (void)usage; (void)secret; errors++;
}
static void setterm_print_save_result(void *io, const char *label, const char *arg, esp_err_t err) {
    (void)io; (void)label; (void)arg; assert(err == ESP_OK);
}
'''
        for name in ("solar_os_power_key_action_name", "solar_os_power_parse_key_action"):
            # The shared extractor also accepts these return types here.
            text = (ROOT / "src/services/solar_os_power.c").read_text()
            match = re.search(r"^(?:const char \*|bool )" + name + r"\([^;]*?\n\{.*?^\}", text, re.M | re.S)
            self.assertIsNotNone(match)
            source += match.group() + "\n"
        source += "static void cli(int argc, char **argv) { void *term = NULL;\n" + command[start:end] + "}\n"
        tui = (ROOT / "src/shell/solar_os_shell_setterm_tui.c").read_text()
        apply_start = tui.index("    case SETTERM_TUI_POWERKEY:\n", tui.index("static bool setterm_tui_apply"))
        apply_end = tui.index("    case SETTERM_TUI_KEYRATE:", apply_start)
        source += r'''
enum { SETTERM_TUI_POWERKEY, SETTERM_TUI_POWERKEY_LONG };
static struct { int selected; char edit_text[32]; } setterm_tui;
static bool apply(void) { switch(setterm_tui.selected) {
''' + tui[apply_start:apply_end] + "default: return false; }}\n"
        run_c(source + r'''
int main(void) {
    (void)edit_text;
    status.key_action = SOLAR_OS_POWER_KEY_ACTION_ENTER;
    status.key_long_action = SOLAR_OS_POWER_KEY_ACTION_SUSPEND;
    char *query[] = {"setterm", "powerkey"}; cli(2, query);
    assert(strstr(output, "KEY short press: enter") && strstr(output, "KEY long press: suspend"));
    char *legacy[] = {"setterm", "powerkey", "sleep"}; cli(3, legacy);
    assert(short_saves == 1U && long_saves == 0U && status.key_action == SOLAR_OS_POWER_KEY_ACTION_SLEEP);
    char *hold[] = {"setterm", "powerkey", "long", "deepsleep"}; cli(4, hold);
    assert(short_saves == 1U && long_saves == 1U && status.key_long_action == SOLAR_OS_POWER_KEY_ACTION_DEEPSLEEP);
    char *tap[] = {"setterm", "key", "short", "enter"}; cli(4, tap);
    assert(short_saves == 2U && status.key_action == SOLAR_OS_POWER_KEY_ACTION_ENTER);
    char *bad[] = {"setterm", "powerkey", "long", "invalid"}; cli(4, bad);
    char *extra[] = {"setterm", "powerkey", "long", "sleep", "extra"}; cli(5, extra);
    assert(errors == 2U && short_saves == 2U && long_saves == 1U);
    setterm_tui.selected = SETTERM_TUI_POWERKEY_LONG;
    strcpy(setterm_tui.edit_text, "ble-pair"); assert(apply());
    assert(long_saves == 2U && status.key_long_action == SOLAR_OS_POWER_KEY_ACTION_BLE_PAIR);
    setterm_tui.selected = SETTERM_TUI_POWERKEY;
    strcpy(setterm_tui.edit_text, "default"); assert(apply());
    assert(short_saves == 3U && status.key_action == SOLAR_OS_POWER_KEY_ACTION_DEFAULT);
}
''', *FLAGS)

    def test_wake_boot_and_completion_routes(self):
        main = (ROOT / "src/main.c").read_text()
        init = function("src/main.c", "key_button_init")
        self.assertIn("key_ignore_until_released = key_button_is_pressed();", init)
        sleep = function("src/main.c", "enter_sleep")
        self.assertIn("key_ignore_until_released = key_button_is_pressed();", sleep)
        self.assertIn("key_ignore_until_released = key_button_is_pressed();", function("src/main.c", "enter_suspend"))
        self.assertIn("solar_os_ble_keyboard_enabled_for_current_boot()", function("src/main.c", "handle_key_pairing"))
        self.assertNotIn("#ifdef SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY", function("src/main.c", "handle_key_short_press"))
        shell = (ROOT / "src/apps/solar_os_shell.c").read_text()
        for route in ("path_setterm_powerkey_short", "path_setterm_powerkey_long",
                      "path_setterm_key_short", "path_setterm_key_long",
                      "path_power_key_short", "path_power_key_long"):
            self.assertIn(f"SHELL_COMPLETION_STATIC({route}, powerkey_action_values)", shell)


if __name__ == "__main__":
    unittest.main()
