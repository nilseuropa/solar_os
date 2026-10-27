from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class BacklightTest(unittest.TestCase):
    def test_power_policy_blanks_and_restores_lighting(self):
        source = r'''
#define main service_tests
#include "backlight_test.c"
#undef main
#include "solar_os_power.h"
static const char *TAG = "power-test";
static solar_os_power_status_t power_status;
static uint32_t power_millis(void) { return 0; }
static void update_automatic_light_sleep_holdoff_status(uint32_t now) { (void)now; }
static void profile_pm_config(solar_os_power_profile_t profile, uint32_t *min, uint32_t *max, bool *automatic) {
    (void)profile; *min = 80; *max = 240; *automatic = false;
}
static esp_err_t apply_bt_sleep_policy(bool automatic) { (void)automatic; return ESP_OK; }
const char *solar_os_power_profile_name(solar_os_power_profile_t profile) { (void)profile; return "test"; }
'''
        source += production_functions((ROOT / "src/services/solar_os_power.c").read_text(), (
            "solar_os_power_apply_runtime_policy", "solar_os_power_begin_suspend",
            "solar_os_power_end_suspend", "solar_os_power_begin_explicit_sleep",
            "solar_os_power_end_explicit_sleep"))
        source += r'''
int main(void) {
    light_t light = {.fail_at = -1};
    assert(solar_os_backlight_toggle() == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_backlight_register(set_light, &light) == ESP_OK);
    /* T-Deck starts dark; its shortcut enables only the current boot. */
    if (!SOLAR_OS_BOARD_BACKLIGHT_ENABLED_ON_BOOT) {
        assert(light.level == 0);
        assert(solar_os_backlight_toggle() == ESP_OK && light.level == 50);
    }
    for (unsigned cycle = 0; cycle < 2; cycle++) {
        assert(solar_os_power_begin_suspend() == ESP_OK && light.level == 0);
        assert(solar_os_power_end_suspend() == ESP_OK && light.level == 50);
        assert(solar_os_power_begin_explicit_sleep() == ESP_OK && light.level == 0);
        assert(solar_os_power_end_explicit_sleep() == ESP_OK && light.level == 50);
    }
    assert(solar_os_backlight_toggle() == ESP_OK && light.level == 0);
    assert(solar_os_backlight_display_changed(90) == ESP_OK && light.level == 0);
    assert(solar_os_power_begin_suspend() == ESP_OK && light.level == 0);
    assert(solar_os_power_end_suspend() == ESP_OK && light.level == 0);
    assert(solar_os_power_begin_explicit_sleep() == ESP_OK && light.level == 0);
    assert(solar_os_power_end_explicit_sleep() == ESP_OK && light.level == 0);
    /* Failed toggles keep the previous awake choice. */
    light.fail_at = 50;
    assert(solar_os_backlight_toggle() == ESP_FAIL && light.level == 0);
    assert(solar_os_power_begin_suspend() == ESP_OK);
    assert(solar_os_power_end_suspend() == ESP_OK && light.level == 0);
    /* A shortcut received during suspend updates only the eventual wake state. */
    assert(solar_os_power_begin_suspend() == ESP_OK);
    assert(solar_os_backlight_toggle() == ESP_OK && light.level == 0);
    assert(solar_os_power_end_suspend() == ESP_OK && light.level == 50);
    assert(stored == -1 && commits == 0);
    assert(solar_os_backlight_set_brightness(0) == ESP_OK && stored == 0);
    const unsigned saved_commits = commits;
    assert(solar_os_backlight_toggle() == ESP_OK && light.level == 50);
    assert(solar_os_power_begin_explicit_sleep() == ESP_OK && light.level == 0);
    assert(solar_os_power_end_explicit_sleep() == ESP_OK && light.level == 50);
    assert(stored == 0 && commits == saved_commits);
    assert(solar_os_backlight_set_brightness(80) == ESP_OK);
    assert(solar_os_power_begin_suspend() == ESP_OK && light.level == 0);
    assert(solar_os_power_begin_suspend() == ESP_ERR_INVALID_STATE);
    assert(solar_os_power_end_suspend() == ESP_OK && light.level == 80);
    assert(solar_os_power_end_suspend() == ESP_ERR_INVALID_STATE);
    assert(solar_os_power_begin_explicit_sleep() == ESP_OK && light.level == 0);
    assert(solar_os_backlight_set_brightness(30) == ESP_OK && light.level == 0);
    assert(solar_os_power_end_explicit_sleep() == ESP_OK && light.level == 30);
    light.fail_at = 0;
    assert(solar_os_power_begin_suspend() == ESP_FAIL);
    assert(!power_status.suspend_active && light.level == 30);
    light.fail_at = 0;
    assert(solar_os_power_begin_explicit_sleep() == ESP_FAIL);
    assert(!power_status.explicit_sleep_active && light.level == 30);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "power.c")
            path.write_text(source)
            binary = Path(tmp, "power")
            for enabled in (0, 1):
                subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                f"-DSOLAR_OS_BOARD_BACKLIGHT_ENABLED_ON_BOOT={enabled}",
                                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                                str(path), str(ROOT / "src/services/solar_os_backlight.c"),
                                "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)

    def test_command_query_validation_and_follow(self):
        commands = (ROOT / "src/shell/solar_os_shell_commands.c").read_text()
        branch = commands.split('    if (strcmp(argv[1], "backlight") == 0) {', 1)[1]
        branch = '    if (strcmp(argv[1], "backlight") == 0) {' + branch.split(
            '    if (strcmp(argv[1], "brightness") == 0) {', 1)[0]
        source = r'''
#define main service_tests
#include "backlight_test.c"
#undef main
#include <errno.h>
typedef void solar_os_shell_io_t;
static char output[512];
static void solar_os_shell_io_printf(void *term, const char *fmt, ...) {
    (void)term; va_list args; va_start(args, fmt);
    vsnprintf(output + strlen(output), sizeof(output) - strlen(output), fmt, args);
    va_end(args);
}
static void solar_os_shell_io_writeln(void *term, const char *text) {
    solar_os_shell_io_printf(term, "%s\n", text);
}
static const char *solar_os_shell_error_text(esp_err_t err) { (void)err; return "error"; }
static void solar_os_shell_diag_unexpected(void *term, const char *command, const char *arg, const char *usage) {
    (void)command; (void)arg; (void)usage; solar_os_shell_io_writeln(term, "unexpected");
}
static void solar_os_shell_diag_invalid(void *term, const char *command, const char *arg, const char *value,
    const char *expected, const char *usage, bool sensitive) {
    (void)command; (void)arg; (void)value; (void)expected; (void)usage; (void)sensitive;
    solar_os_shell_io_writeln(term, "invalid");
}
'''
        source += production_functions((ROOT / "src/shell/solar_os_shell_common.c").read_text(),
                                       ("solar_os_shell_parse_size_arg",)).replace(
            "solar_os_shell_parse_size_arg(", "parse_size_arg(")
        source += '\nstatic void command(int argc, char **argv) { void *term = NULL;\n' + branch + '}\n'
        source += r'''
static void run(int argc, const char *value, const char *expected) {
    char *argv[] = {"setterm", "backlight", (char *)value, "extra"};
    output[0] = 0;
    command(argc, argv);
    assert(strstr(output, expected) != NULL);
}
int main(void) {
    run(2, NULL, "backlight: unsupported");
    run(3, "50", "backlight: unsupported");
    light_t light = {.fail_at = -1};
    assert(solar_os_backlight_register(set_light, &light) == ESP_OK);
    run(2, NULL, "backlight: 50");
    run(3, "follow", "backlight: unsupported");
    assert(solar_os_backlight_display_changed(67) == ESP_OK);
    run(3, "follow", "backlight: follow");
    assert(light.level == 67 && stored == 101);
    run(2, NULL, "follow (brightness 67)");
    run(3, "0", "backlight: 0");
    assert(light.level == 0 && stored == 0);
    run(3, "100", "backlight: 100");
    assert(light.level == 100 && stored == 100);
    const char *invalid[] = {"-1", "101", "99999999999999999999999", "banana", "50x", ""};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) run(3, invalid[i], "invalid");
    run(4, "25", "unexpected");
    assert(light.level == 100 && stored == 100);
    light.fail_at = 25;
    run(3, "25", "backlight: failed");
    assert(light.level == 100 && stored == 100);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "command.c")
            path.write_text(source)
            binary = Path(tmp, "command")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                            str(path), str(ROOT / "src/services/solar_os_backlight.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_settings_follow_suspend_and_failures(self):
        board = tomllib.loads((ROOT / "boards/manifests/t_deck_plus.toml").read_text())
        tdeck_enabled = int(board["defines"]["SOLAR_OS_BOARD_BACKLIGHT_ENABLED_ON_BOOT"])
        self.assertEqual(tdeck_enabled, 0)
        self.assertNotIn("SOLAR_OS_BOARD_BACKLIGHT_DEFAULT_PERCENT", board["defines"])
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "backlight")
            for enabled in (None, tdeck_enabled):
                with self.subTest(boot_enabled=enabled):
                    flags = [] if enabled is None else [
                        f"-DSOLAR_OS_BOARD_BACKLIGHT_ENABLED_ON_BOOT={enabled}"]
                    subprocess.run([
                        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *flags,
                        "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                        str(ROOT / "tests/host/backlight_test.c"),
                        str(ROOT / "src/services/solar_os_backlight.c"), "-o", str(binary),
                    ], check=True)
                    for stored in (-1, 0, 32, 100, 101, 255):
                        subprocess.run([str(binary), str(stored),
                                        str(1 if enabled is None else enabled)], check=True)

    def test_board_transactions(self):
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
typedef struct { char i2c_bus[16]; } tdeck_device_t;
typedef struct { char i2c_bus[16]; uint8_t address; } solar_os_cl32_core_device_t;
typedef struct { int backlight_pin; } pager_device_t, solar_os_stc8h_device_t;
typedef int gpio_num_t;
#define SOLAR_OS_TDECK_KEYBOARD_ADDRESS 0x55U
#define CL32_CORE_REG_BACKLIGHT 0x0fU
#define STC8H_BACKLIGHT_PWM_HZ 5000U
static unsigned address, reg, duty, pin, frequency;
static esp_err_t result;
static esp_err_t solar_os_bus_i2c_transmit(const char *bus, int addr, const void *data, size_t count) {
    assert(strcmp(bus, "i2c0") == 0 && count == 2);
    const uint8_t *bytes = data; address = addr; reg = bytes[0]; duty = bytes[1]; return result;
}
static esp_err_t solar_os_bus_i2c_write_reg(const char *bus, int addr, int command, const void *data, size_t count) {
    assert(strcmp(bus, "i2c0") == 0 && count == 1);
    address = addr; reg = command; duty = *(const uint8_t *)data; return result;
}
static esp_err_t pwm_port_set(int gpio, unsigned hz, unsigned percent) {
    pin = gpio; frequency = hz; duty = percent; return result;
}
'''
        for suffix, file in (
            ("tdeck", "solar_os_tdeck_keyboard.c"),
            ("cl32", "solar_os_cl32_core.c"),
            ("pager", "solar_os_lilygo_pager_keyboard.c"),
            ("m9", "solar_os_stc8h_keyboard.c"),
        ):
            source += production_functions((ROOT / "src/services" / file).read_text(),
                                           ("set_backlight",)).replace(
                "set_backlight(", f"set_{suffix}(") + "\n"
        source += r'''
int main(void) {
    tdeck_device_t tdeck = {.i2c_bus = "i2c0"};
    solar_os_cl32_core_device_t cl32 = {.i2c_bus = "i2c0", .address = 8};
    pager_device_t pager = {.backlight_pin = 46};
    solar_os_stc8h_device_t m9 = {.backlight_pin = 46};
    const unsigned levels[] = {0, 1, 50, 100}, bytes[] = {0, 3, 128, 255};
    for (unsigned i = 0; i < 4; i++) {
        assert(set_tdeck(&tdeck, levels[i]) == ESP_OK);
        assert(address == 0x55 && reg == 1 && duty == bytes[i]);
        assert(set_cl32(&cl32, levels[i]) == ESP_OK);
        assert(address == 8 && reg == 0x0f && duty == bytes[i]);
        assert(set_pager(&pager, levels[i]) == ESP_OK);
        assert(pin == 46 && frequency == 5000 && duty == levels[i]);
        assert(set_m9(&m9, levels[i]) == ESP_OK);
        assert(pin == 46 && frequency == 5000 && duty == levels[i]);
    }
    result = ESP_FAIL;
    assert(set_tdeck(&tdeck, 0) == ESP_FAIL && set_cl32(&cl32, 0) == ESP_FAIL);
    assert(set_pager(&pager, 0) == ESP_FAIL && set_m9(&m9, 0) == ESP_FAIL);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "transactions.c")
            path.write_text(source)
            binary = Path(tmp, "transactions")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "tests/host"), str(path),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
