from pathlib import Path
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class DisplayRelaxTest(unittest.TestCase):
    def test_recovery_lifecycle_and_cancellation(self):
        source = (ROOT / "src/services/solar_os_display.c").read_text()
        functions = []
        for name in ("display_overlay_active", "display_relax_cancelled",
                     "display_relax_hold", "solar_os_display_relax"):
            match = re.search(r"^(?:static )?(?:bool|esp_err_t) " + name
                              + r"\([^;]*?\n\{.*?^\}", source, re.M | re.S)
            self.assertIsNotNone(match, name)
            functions.append(match.group())
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "solar_os_display_maintenance.h"
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE,
       ESP_ERR_NOT_FOUND, ESP_ERR_NOT_SUPPORTED };
typedef unsigned TickType_t;
typedef void *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(ms) (ms)
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
typedef struct {
    esp_err_t (*relax)(void *, solar_os_display_relax_phase_t);
    void *relax_context;
} solar_os_display_target_t;
typedef struct { uint32_t cycles; bool cancelled; } solar_os_display_relax_result_t;
typedef struct {
    bool active, overlay_active, power_state_active, relax_active;
    uint32_t generation;
    unsigned refs;
    SemaphoreHandle_t present_mutex;
    solar_os_display_target_t target;
    void *board_display;
} display_target_slot_t;
static display_target_slot_t display_targets[1];
static int display_targets_lock;
#if SOLAR_OS_BOARD_HAS_DISPLAY
static bool display_primary_suspended;
static void *display_handle = (void *)1;
#endif
static int64_t now_us;
static bool held, begun, nested;
static unsigned claims, begins, blacks, whites, ends;
static int fail_phase, cancel_source;
static char claimed_by[32];
static int64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(TickType_t ticks) { assert(ticks); now_us += ticks * 1000LL; }
static void xSemaphoreTake(SemaphoreHandle_t mutex, unsigned timeout) {
    (void)timeout; assert(mutex == (void *)1 && !held); held = true;
}
static void xSemaphoreGive(SemaphoreHandle_t mutex) { assert(mutex == (void *)1 && held); held = false; }
static int display_find_slot_locked(const char *name) { return !strcmp(name, "display0") ? 0 : -1; }
static esp_err_t solar_os_display_claim(const char *name, const char *owner, char *busy, size_t size) {
    if (busy && size) busy[0] = 0;
    if (display_find_slot_locked(name) < 0) return ESP_ERR_NOT_FOUND;
    if (claims && strcmp(owner, claimed_by)) {
        if (busy && size) { assert(size >= strlen(claimed_by) + 1); strcpy(busy, claimed_by); }
        return ESP_ERR_INVALID_STATE;
    }
    strcpy(claimed_by, owner); ++claims; return ESP_OK;
}
static esp_err_t solar_os_display_release(const char *name, const char *owner) {
    assert(!strcmp(name, "display0") && !strcmp(owner, claimed_by) && claims);
    if (--claims == 0) claimed_by[0] = 0;
    return ESP_OK;
}
static void display_release_slot_ref(size_t index, uint32_t generation) {
    assert(index == 0 && display_targets[0].generation == generation && display_targets[0].refs);
    --display_targets[0].refs;
}
esp_err_t solar_os_display_relax(const char *, const char *, unsigned,
    bool (*)(void *), void *, solar_os_display_relax_result_t *, char *, size_t);
static bool display_overlay_active(size_t, uint32_t);
static esp_err_t panel(void *context, solar_os_display_relax_phase_t phase) {
    assert(context == &now_us && held && display_targets[0].refs == 1);
    assert(display_overlay_active(0, 1));
    assert(display_targets[0].relax_active);
    if (phase == SOLAR_OS_DISPLAY_RELAX_BEGIN) {
        ++begins;
        if (nested) {
            solar_os_display_relax_result_t other;
            assert(solar_os_display_relax("display0", "test", 1, NULL, NULL, &other, NULL, 0) == ESP_ERR_INVALID_STATE);
            assert(claims == 1 && display_targets[0].refs == 1);
        }
        if (fail_phase == (int)phase) return ESP_FAIL;
        assert(!begun); begun = true;
    } else {
        assert(begun);
        if (phase == SOLAR_OS_DISPLAY_RELAX_BLACK) {
            ++blacks;
            if (cancel_source == 2) display_targets[0].power_state_active = true;
#if SOLAR_OS_BOARD_HAS_DISPLAY
            if (cancel_source == 3) display_primary_suspended = true;
#endif
        }
        if (phase == SOLAR_OS_DISPLAY_RELAX_WHITE) ++whites;
        if (phase == SOLAR_OS_DISPLAY_RELAX_END) { ++ends; begun = false; }
        if (fail_phase == (int)phase) return ESP_FAIL;
    }
    return ESP_OK;
}
static bool cancelled(void *user) { assert(user == &now_us); return cancel_source == 1 && blacks > 0; }
static void reset(void) {
    now_us = 0; claims = begins = blacks = whites = ends = 0;
    held = begun = nested = false; fail_phase = -1; cancel_source = 0;
    claimed_by[0] = 0;
    display_targets[0] = (display_target_slot_t){.active = true, .generation = 1,
        .present_mutex = (void *)1, .board_display = (void *)1,
        .target = {.relax = panel, .relax_context = &now_us}};
#if SOLAR_OS_BOARD_HAS_DISPLAY
    display_primary_suspended = false;
#endif
}
static esp_err_t run(unsigned seconds, solar_os_display_relax_result_t *result) {
    esp_err_t err = solar_os_display_relax("display0", "test", seconds, cancelled, &now_us, result, NULL, 0);
    assert(!held && !begun && !claims && !display_targets[0].refs && !display_targets[0].relax_active);
    return err;
}
'''
        harness += "\n".join(functions)
        harness += r'''
int main(void) {
    solar_os_display_relax_result_t result;
    reset(); nested = true;
    assert(run(3, &result) == ESP_OK);
    assert(begins == 1 && ends == 1 && result.cycles == 2 && !result.cancelled);
    assert(blacks == whites && now_us == 3000000);
    assert(!display_overlay_active(0, 1));
    for (int source = 1; source <= 2 + SOLAR_OS_BOARD_HAS_DISPLAY; ++source) {
        reset(); cancel_source = source;
        assert(run(60, &result) == ESP_OK && result.cancelled);
        assert(blacks == 1 && whites == 0 && ends == 1 && now_us == 0);
    }
    for (int phase = SOLAR_OS_DISPLAY_RELAX_BEGIN; phase <= SOLAR_OS_DISPLAY_RELAX_END; ++phase) {
        reset(); fail_phase = phase;
        assert(run(2, &result) == ESP_FAIL);
        assert(begins == 1 && ends == (phase != SOLAR_OS_DISPLAY_RELAX_BEGIN));
    }
    reset(); display_targets[0].target.relax = NULL;
    assert(run(1, &result) == ESP_ERR_NOT_SUPPORTED && begins == 0);
    reset(); assert(run(0, &result) == ESP_ERR_INVALID_ARG && begins == 0);
    assert(run(86401, &result) == ESP_ERR_INVALID_ARG && begins == 0);
    reset(); claims = 1; strcpy(claimed_by, "another-session");
    char busy[32];
    assert(solar_os_display_relax("display0", "test", 1, NULL, NULL, &result, busy, sizeof(busy)) == ESP_ERR_INVALID_STATE);
    assert(!strcmp(busy, "another-session") && claims == 1 && begins == 0);
    reset();
    assert(solar_os_display_relax("missing", "test", 1, NULL, NULL, &result, busy, sizeof(busy)) == ESP_ERR_NOT_FOUND);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(harness)
            for has_display in (0, 1):
                with self.subTest(has_display=has_display):
                    compiled = subprocess.run([
                        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        f"-DSOLAR_OS_BOARD_HAS_DISPLAY={has_display}",
                        "-I", str(ROOT / "src/services"), str(path / "test.c"),
                        "-o", str(path / "test"),
                    ], capture_output=True, text=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
