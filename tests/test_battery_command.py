from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]


class BatteryCommandTest(unittest.TestCase):
    def test_short_report_and_explicit_full_status(self):
        source = (ROOT / "src/shell/solar_os_shell_hardware.c").read_text()
        functions = (
            "battery_format_voltage", "battery_format_minutes", "battery_print_config",
            "battery_print_window_stats", "battery_print_time_left",
            "battery_print_monitor_status", "battery_print_status", "solar_os_shell_cmd_battery",
        )
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "battery_command.inc").write_text(production_functions(source, functions))
            binary = stage / "battery_command"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(stage), "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"),
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/battery_command_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_voltage_reads_can_overlap_smoothing_resets(self):
        source = (ROOT / "src/services/solar_os_battery.c").read_text()
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#define BATTERY_VOLTAGE_AVG_WINDOW 8U
#define portENTER_CRITICAL(lock) assert(pthread_mutex_lock(lock) == 0)
#define portEXIT_CRITICAL(lock) assert(pthread_mutex_unlock(lock) == 0)
static pthread_mutex_t battery_voltage_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t battery_voltage_samples[8];
static size_t battery_voltage_sample_count, battery_voltage_sample_index;
static uint32_t battery_voltage_sample_sum;
static bool battery_voltage_external_power_valid, battery_voltage_external_power;
'''
        harness += production_functions(source, (
            "battery_reset_voltage_average", "battery_push_voltage_average",
        ))
        harness += r'''
static void *sample(void *reset) {
    for (unsigned i = 0; i < 100000U; i++)
        assert(battery_push_voltage_average(3800U, false, false, reset != NULL) == 3800U);
    return NULL;
}
int main(void) {
    pthread_t reader, monitor;
    int reset = 1;
    assert(pthread_create(&reader, NULL, sample, NULL) == 0);
    assert(pthread_create(&monitor, NULL, sample, &reset) == 0);
    assert(pthread_join(reader, NULL) == 0 && pthread_join(monitor, NULL) == 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "average.c").write_text(harness)
            binary = stage / "average"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", str(stage / "average.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
