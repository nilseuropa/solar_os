"""Exercise the SC2 worker notification and room handoff without ESP-IDF."""
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
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef unsigned solar_os_messaging_provider_id_t;
#define ESP_OK 0
#define SOLAR_OS_PACKAGE_JOB_SC2_SYNC 1
static struct { atomic_bool running, stop, reconfigure; } s;
static unsigned notifications, statuses;
static void *const wake = (void *)1;
static int xSemaphoreGive(void *target) {
    assert(target == wake);
    notifications++;
    return 1;
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
    provider_changed(7, true);
    assert(!s.running && !s.reconfigure);
    assert(statuses == 1 && notifications == 0);
    provider_changed(7, false);
    assert(statuses == 1 && notifications == 0 && !s.running);

    s.running = true;
    provider_changed(7, true);
    assert(s.running && s.reconfigure && notifications == 1);
    s.reconfigure = false;
    provider_changed(7, false);
    assert(s.running && s.reconfigure && notifications == 2);

    /* A change during job stop must not wake or restart the worker. */
    s.stop = true;
    s.reconfigure = false;
    provider_changed(7, true);
    provider_changed(7, false);
    assert(s.stop && !s.reconfigure && notifications == 2 && statuses == 2);
    s.running = false;
    provider_changed(7, true);
    assert(!s.running && s.stop && notifications == 2 && statuses == 3);
}
'''
        harness = harness.replace("/* CALLBACK */", source[start:end])
        self.run_harness(harness)

    def test_room_handoff_on_stop_and_timeout(self):
        source = (ROOT / "src/services/solar_os_sc2_sync.c").read_text()

        def section(first, last):
            start = source.index(first)
            return source[start:source.index(last, start)]

        declarations = section("enum { RQ_IDLE", "\ntypedef struct {\n    TaskHandle_t task;")
        finish = section("static void rq_release(", "\nstatic ")
        finish += "\n" + section("static void rq_finish(esp_err_t err, const char *fmt, ...)\n{", "\nstatic ")
        room = section("esp_err_t solar_os_sc2_sync_room(", "\nstatic void worker(")
        harness = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ESP_OK 0
#define ESP_ERR_INVALID_SIZE 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define SC2_INVITE_TEXT_MAX 100
#define SC2_DEVICE_ID_HEX_LEN 32
#define SOLAR_OS_SC2_ROOM_OUT_MAX 200
#define SOLAR_OS_TASK_STOP_POLL_MS 20
#define pdMS_TO_TICKS(ticks) (ticks)
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
typedef int esp_err_t;
typedef unsigned TickType_t;
typedef unsigned solar_os_sc2_room_op_t;

static void (*after_unlock)(void);
static void portENTER_CRITICAL(portMUX_TYPE *lock) {
    assert((*lock)++ == 0);
}
static void portEXIT_CRITICAL(portMUX_TYPE *lock) {
    assert(--(*lock) == 0);
    if (after_unlock) {
        void (*hook)(void) = after_unlock;
        after_unlock = NULL;
        hook();
    }
}
/* DECLARATIONS */
static sc2s_rq_t request;
static struct { atomic_bool running, stop; sc2s_rq_t *rq; } s = {.rq = &request};
static unsigned now, behavior;
static const unsigned wake = 1;
static uint64_t mono_ms(void) { return now; }
static void vTaskDelay(TickType_t delay) { now += delay; }
static void sc2_memzero(void *p, size_t size) { memset(p, 0, size); }
static int xSemaphoreGive(unsigned signal);
/* FINISH */
/* ROOM */
static int xSemaphoreGive(unsigned signal) {
    assert(signal == wake);
    assert(request.state == RQ_POSTED && !strcmp(request.arg, "invite-secret"));
    if (behavior == 1) {
        request.state = RQ_WORKING;
        strcpy(request.room, "room");
        strcpy(request.out, "response-secret");
        rq_finish(ESP_OK, NULL);
    } else if (behavior == 2) {
        request.state = RQ_WORKING;
    } else if (behavior == 3) {
        close_room_requests();
    }
    return 1;
}
static void restarted(void) {
    assert(request.state == RQ_CLAIMED);
    close_room_requests();
    s.running = true;
}

int main(void) {
    char id[33], out[200], why[96];
    for (behavior = 0; behavior < 4; ++behavior) {
        s.running = true;
        assert(request.state == RQ_IDLE);
        const int error = solar_os_sc2_sync_room(1, "invite-secret", id, out, sizeof(out),
                                                why, sizeof(why), 20);
        if (behavior == 0) {
            assert(error == ESP_ERR_TIMEOUT && strstr(why, "did not take"));
        } else if (behavior == 1) {
            assert(error == ESP_OK && !strcmp(id, "room") && !strcmp(out, "response-secret"));
        } else if (behavior == 2) {
            assert(error == ESP_ERR_TIMEOUT && request.state == RQ_ABANDONED);
            assert(!strcmp(request.arg, "invite-secret"));
            strcpy(request.out, "response-secret");
            rq_finish(ESP_OK, NULL);
        } else {
            assert(error == ESP_ERR_INVALID_STATE && strstr(why, "has stopped") && !s.running);
        }
        assert(request.state == RQ_IDLE && !request.arg[0] && !request.out[0]);
    }

    s.running = true;
    after_unlock = restarted;
    assert(solar_os_sc2_sync_room(1, "invite-secret", id, out, sizeof(out),
                                 why, sizeof(why), 20) == ESP_ERR_INVALID_STATE);
    assert(request.state == RQ_IDLE && !request.arg[0]);
}
'''
        harness = harness.replace("/* DECLARATIONS */", declarations)
        harness = harness.replace("/* FINISH */", finish)
        harness = harness.replace("/* ROOM */", room)
        self.run_harness(harness)

    @staticmethod
    def run_harness(harness):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(harness)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(path / "test.c"), "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)

if __name__ == "__main__":
    unittest.main()
