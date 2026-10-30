"""Exercise the firmware's provider notification callback without ESP-IDF."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Sc2JobLifecycleTest(unittest.TestCase):
    def test_provider_changes_cannot_start_a_worker(self):
        source = (ROOT / "src/services/solar_os_sc2_sync.c").read_text()
        start = source.index("static void provider_changed(")
        end = source.index("\nvoid solar_os_sc2_sync_install(", start)
        # Compile the actual callback. No job/task start symbols are supplied:
        # any implicit start path is a link failure, even if a case misses it.
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef unsigned solar_os_messaging_provider_id_t;
#define ESP_OK 0
static struct { bool running, stop; void *task; uint64_t checked_at; } s;
static unsigned notifications, statuses;
static void *const task = (void *)1;
static void xTaskNotifyGive(void *target) {
    assert(target == task);
    notifications++;
}
static int solar_os_messaging_provider_set_status(unsigned id, bool running,
                                                  bool connected, int error,
                                                  const char *detail) {
    assert(id == 7 && !running && !connected && error == ESP_OK);
    assert(strstr(detail, "job start sc2-sync"));
    statuses++;
    return ESP_OK;
}
/* CALLBACK */
int main(void) {
    s.checked_at = 123;
    provider_changed(7, true);
    assert(!s.running && !s.task && s.checked_at == 123);
    assert(statuses == 1 && notifications == 0);
    provider_changed(7, false);
    assert(statuses == 1 && notifications == 0 && !s.running);

    s.running = true;
    s.task = task;
    provider_changed(7, true);
    assert(s.running && s.checked_at == 0 && notifications == 1);
    s.checked_at = 456;
    provider_changed(7, false);
    assert(s.running && s.checked_at == 0 && notifications == 2);

    /* A change during job stop must not wake or restart the worker. */
    s.stop = true;
    s.checked_at = 789;
    provider_changed(7, true);
    provider_changed(7, false);
    assert(s.stop && s.checked_at == 789 && notifications == 2 && statuses == 2);
    s.running = false;
    s.task = NULL;
    provider_changed(7, true);
    assert(!s.running && !s.task && s.stop && notifications == 2 && statuses == 3);
}
'''
        harness = harness.replace("/* CALLBACK */", source[start:end])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(harness)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
