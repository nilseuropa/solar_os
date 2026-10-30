from pathlib import Path
import unittest

from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class St7305PowerTest(unittest.TestCase):
    def test_splash_scans_before_display_off_and_resume_restores_policy(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
typedef enum { RLCD_POWER_POLICY_AUTO, RLCD_POWER_POLICY_HPM, RLCD_POWER_POLICY_LPM } rlcd_power_policy_t;
typedef struct { int unused; } u8g2_t;
typedef struct { const char *name; uint8_t power_mode_cmd; } rlcd_controller_profile_t;
typedef struct {
    void *spi;
    uint8_t *buffer;
    u8g2_t u8g2;
    uint8_t power_policy;
    const char *controller_mode;
    bool power_state_present, frame_content_changed;
    esp_err_t last_error;
} rlcd_st7305_t;
static const rlcd_controller_profile_t rlcd_controller_profiles[] = {{"hpm", 0x38}, {"lpm", 0x39}};
static unsigned locked, sent, delays, cancelled, invalidated;
static uint8_t command, mode;
static esp_err_t send_error, mode_error;
static bool rlcd_take_lock(rlcd_st7305_t *d, unsigned timeout)
{ assert(d && !locked); (void)timeout; locked = 1; return true; }
static void rlcd_give_lock(rlcd_st7305_t *d) { assert(d && locked); locked = 0; }
static void rlcd_cancel_idle_lpm_timer(rlcd_st7305_t *d) { assert(d && locked); cancelled++; }
static void rlcd_invalidate_shadow(rlcd_st7305_t *d) { assert(d && locked); invalidated++; }
static bool rlcd_checked_cmd(rlcd_st7305_t *d, uint8_t cmd)
{ assert(d && locked); command = cmd; return true; }
static esp_err_t rlcd_apply_controller_power_mode(rlcd_st7305_t *d, const rlcd_controller_profile_t *p)
{ assert(d && locked); command = mode = p->power_mode_cmd; d->last_error = mode_error; return mode_error; }
static const rlcd_controller_profile_t *rlcd_current_controller_profile(const rlcd_st7305_t *d)
{ assert(d); return &rlcd_controller_profiles[mode == 0x39]; }
static void u8g2_SendBuffer(u8g2_t *u)
{
    rlcd_st7305_t *d = (void *)((char *)u - offsetof(rlcd_st7305_t, u8g2));
    assert(!locked && mode == 0x38 && d->power_policy == RLCD_POWER_POLICY_HPM);
    d->last_error = send_error; sent++;
}
static void vTaskDelay(unsigned ticks) { assert(locked && sent && mode == 0x38 && ticks >= 130); delays++; }
'''
        source += production_functions(
            (ROOT / "src/drivers/rlcd_st7305.c").read_text(),
            ("rlcd_frame_power_profile", "rlcd_apply_frame_power_mode",
             "rlcd_set_power_save", "rlcd_st7305_present_power_state"),
        )
        run_c(source + r'''
int main(void)
{
    uint8_t buffer = 0;
    rlcd_st7305_t d = {.spi = &buffer, .buffer = &buffer, .power_policy = 2};
    for (unsigned cycle = 0; cycle < 9; cycle++) {
        d.power_policy = cycle % 3;
        assert(rlcd_st7305_present_power_state(&d) == ESP_OK);
        assert(sent == cycle + 1 && delays == cycle + 1 && !locked);
        assert(mode == 0x39 && d.power_state_present && d.power_policy == cycle % 3);
        assert(rlcd_take_lock(&d, 0));
        assert(rlcd_set_power_save(&d, true) == ESP_OK && command == 0x28);
        assert(rlcd_set_power_save(&d, false) == ESP_OK && command == 0x29);
        assert(!d.power_state_present);
        const uint8_t restored_mode = d.power_policy == RLCD_POWER_POLICY_LPM ? 0x39 : 0x38;
        assert(mode == restored_mode);
        /* The display service may also clear the power state after resume. */
        assert(rlcd_set_power_save(&d, false) == ESP_OK && mode == restored_mode);
        assert(rlcd_set_power_save(&d, true) == ESP_OK && command == 0x28);
        rlcd_give_lock(&d);
    }
    send_error = ESP_FAIL;
    assert(rlcd_st7305_present_power_state(&d) == ESP_FAIL);
    assert(delays == 9 && d.power_policy == 2 && !locked);
    send_error = ESP_OK; mode_error = ESP_FAIL;
    assert(rlcd_st7305_present_power_state(&d) == ESP_FAIL);
    assert(delays == 9 && d.power_policy == 2 && !locked);
    /* A failed mode restore must retain the state so a wake can retry it. */
    d.power_policy = RLCD_POWER_POLICY_HPM;
    mode = 0x39;
    assert(rlcd_take_lock(&d, 0));
    assert(rlcd_set_power_save(&d, false) == ESP_FAIL && d.power_state_present);
    mode_error = ESP_OK;
    mode = 0x39;
    assert(rlcd_set_power_save(&d, false) == ESP_OK && !d.power_state_present && mode == 0x38);
    rlcd_give_lock(&d);
    assert(cancelled && invalidated);
    assert(rlcd_st7305_present_power_state(NULL) == ESP_ERR_INVALID_STATE);
}
''')
