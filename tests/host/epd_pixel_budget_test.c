#include "epd_pixel_budget.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_sparse_matches_dense(void)
{
    uint8_t dense_pixels[64], sparse_pixels[64];
    uint32_t active[64];
    uint16_t dense_history[64], sparse_history[64];
    epd_pixel_budget_t dense, sparse;
    assert(epd_pixel_budget_init(&dense, dense_pixels, 64) == ESP_OK);
    assert(epd_pixel_budget_init(&sparse, sparse_pixels, 64) == ESP_OK);
    assert(epd_pixel_budget_set_drive_history(&dense, dense_history, 64) == ESP_OK);
    assert(epd_pixel_budget_set_drive_history(&sparse, sparse_history, 63) == ESP_ERR_INVALID_ARG);
    assert(epd_pixel_budget_set_drive_history(&sparse, sparse_history, 64) == ESP_OK);
    assert(epd_pixel_budget_set_active_storage(&sparse, active, 63) == ESP_ERR_INVALID_ARG);
    assert(epd_pixel_budget_set_active_storage(&sparse, active, 64) == ESP_OK);
    uint32_t random = 12345;
    for (unsigned step = 0; step < 10000; ++step) {
        random = random * 1664525U + 1013904223U;
        size_t p = (random >> 8) % 64;
        const bool white = (random & 0x80U) != 0;
        if (step % 137 == 0) {
            epd_pixel_budget_reset(&dense); epd_pixel_budget_reset(&sparse);
        } else if (step % 11 == 0) {
            /* Seed then retarget before compaction must not double-enlist. */
            epd_pixel_budget_seed(&dense, p, white);
            epd_pixel_budget_seed(&sparse, p, white);
            epd_pixel_budget_target(&dense, p, !white, 2);
            epd_pixel_budget_target(&sparse, p, !white, 2);
        } else if (step % 3 == 0) {
            epd_pixel_budget_advance(&dense); epd_pixel_budget_advance(&sparse);
            assert(sparse.active_count == sparse.pending);
        } else {
            const uint8_t cycles = (random >> 16) % 32 + 1;
            epd_pixel_budget_target(&dense, p, white, cycles);
            epd_pixel_budget_target(&sparse, p, white, cycles);
        }
        assert(dense.pending == sparse.pending && sparse.active_count <= sparse.count);
        assert(memcmp(dense_history, sparse_history, sizeof(dense_history)) == 0);
        assert(dense.peak_drive_cycles == sparse.peak_drive_cycles);
        for (size_t i = 0; i < 64; ++i) {
            assert(epd_pixel_budget_drive(&dense, i) == epd_pixel_budget_drive(&sparse, i));
            assert(dense_pixels[i] == (sparse_pixels[i] & 0xbfU));
        }
    }
}

static void test_drive_history(void)
{
    uint8_t pixels[3];
    uint16_t history[3];
    epd_pixel_budget_t b;
    assert(epd_pixel_budget_init(&b, pixels, 3) == ESP_OK);
    assert(epd_pixel_budget_set_drive_history(&b, NULL, 3) == ESP_ERR_INVALID_ARG);
    assert(epd_pixel_budget_set_drive_history(&b, history, 3) == ESP_OK);
    epd_pixel_budget_seed(&b, 0, true);
    epd_pixel_budget_target(&b, 0, false, 3);
    assert(b.peak_drive_cycles == 0); /* Submissions do not age exposure. */
    assert(epd_pixel_budget_set_drive_history(&b, history, 3) == ESP_ERR_INVALID_ARG);
    epd_pixel_budget_advance(&b);
    epd_pixel_budget_target(&b, 0, true, 2); /* Reverse an unfinished transition. */
    epd_pixel_budget_advance(&b); epd_pixel_budget_advance(&b);
    assert(history[0] == 3 && history[1] == 0 && b.peak_drive_cycles == 3);
    epd_pixel_budget_advance(&b);
    assert(history[0] == 3); /* Neutral/settled pixels do not accumulate drives. */
    epd_pixel_budget_seed(&b, 0, true);
    assert(history[0] == 3); /* Only a complete baseline reset clears history. */
    history[0] = UINT16_MAX - 1;
    epd_pixel_budget_target(&b, 0, false, 3);
    for (unsigned i = 0; i < 3; ++i) epd_pixel_budget_advance(&b);
    assert(history[0] == UINT16_MAX && b.peak_drive_cycles == UINT16_MAX);
    epd_pixel_budget_reset(&b);
    assert(b.peak_drive_cycles == 0 && history[0] == 0);
}

int main(void)
{
    test_sparse_matches_dense();
    test_drive_history();
    epd_pulse_tuning_t redrive = epd_pulse_default_tuning();
    assert(redrive.black_redrives == 0 && redrive.white_redrives == 0);
    assert(epd_pulse_tuning_set(&redrive, "black-redrives=31") == ESP_OK);
    assert(epd_pulse_tuning_set(&redrive, "white-redrives=0") == ESP_OK);
    assert(epd_pulse_tuning_set(&redrive, "changed-drive-frames=4") == ESP_OK && redrive.pulse_frames == 4);
    redrive.drive_cycles = 32;
    assert(epd_pulse_target_cycles(&redrive, false) == 63);
    assert(epd_pulse_target_cycles(&redrive, true) == 32);
    uint8_t pixels[4];
    epd_pixel_budget_t b;
    assert(epd_pixel_budget_init(&b, pixels, 4) == ESP_OK);
    for (size_t i = 0; i < 4; ++i)
        epd_pixel_budget_seed(&b, i, true);
    epd_pixel_budget_target(&b, 0, false, 3);
    epd_pixel_budget_target(&b, 1, false, 1);
    assert(b.pending == 2);
    assert(epd_pixel_budget_drive(&b, 2) == EPD_PIXEL_HOLD);
    epd_pixel_budget_advance(&b);
    assert(b.pending == 1 && epd_pixel_budget_drive(&b, 1) == EPD_PIXEL_HOLD);
    epd_pixel_budget_target(&b, 0, false, 3); /* Must not refill 2 remaining. */
    epd_pixel_budget_advance(&b);
    epd_pixel_budget_advance(&b);
    assert(b.pending == 0);
    epd_pixel_budget_target(&b, 0, true, 3);
    epd_pixel_budget_advance(&b);
    epd_pixel_budget_target(&b, 0, false, 2); /* Mid-transition reversal. */
    assert(epd_pixel_budget_drive(&b, 0) == EPD_PIXEL_DRIVE_BLACK);
    epd_pixel_budget_advance(&b);
    assert(b.pending == 1);
    epd_pixel_budget_seed(&b, 0, true); /* Known full-cleanup state. */
    assert(b.pending == 0);
    epd_pixel_budget_target(&b, 0, false, 32);
    for (unsigned i = 0; i < 32; ++i)
        epd_pixel_budget_advance(&b);
    assert(b.pending == 0 && epd_pixel_budget_drive(&b, 0) == EPD_PIXEL_HOLD);
    epd_pixel_budget_target(&b, 0, true, 63);
    for (unsigned i = 0; i < 63; ++i) {
        assert(epd_pixel_budget_drive(&b, 0) == EPD_PIXEL_DRIVE_WHITE);
        epd_pixel_budget_advance(&b);
    }
    assert(b.pending == 0);
    epd_pixel_budget_target(&b, 0, true, 0);
    epd_pixel_budget_target(&b, 4, true, 1);
    assert(b.pending == 0);

    epd_pulse_tuning_t t = epd_pulse_default_tuning();
    assert(epd_pulse_tuning_set(&t, "drive-cycles=6") == ESP_OK);
    assert(t.drive_cycles == 6);
    assert(epd_pulse_tuning_set(&t, "pulse-frames=8") == ESP_OK);
    assert(epd_pulse_tuning_set(&t, "pulse-frames=16") == ESP_OK && t.pulse_frames == 16);
    assert(epd_pulse_tuning_set(&t, "pulse-frames=32") == ESP_OK && t.pulse_frames == 32);
    assert(epd_pulse_tuning_set(&t, "waveform-fr=4") == ESP_OK);
    assert(epd_pulse_tuning_set(&t, "cleanup-every=128") == ESP_OK);
    assert(epd_pulse_tuning_set(&t, "cleanup-policy=adaptive") == ESP_OK && t.cleanup_adaptive);
    assert(epd_pulse_tuning_set(&t, "cleanup-policy=fixed") == ESP_OK && !t.cleanup_adaptive);
    const epd_pulse_tuning_t before = t;
    const char *bad[] = {"drive-cycles=0", "drive-cycles=33", "drive-cycles=-1",
        "drive-cycles=+1", "drive-cycles=1x", "drive-cycles=99999999999999",
        "drive-cycles=", "pulse-frames=0", "pulse-frames=33", "waveform-fr=13", "cleanup-every=129",
        "cleanup-policy=", "cleanup-policy=auto", "cleanup-policy=adaptive1",
        "black-redrives=32", "white-redrives=-1", "black-redrives=", "changed-drive-frames=0"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(epd_pulse_tuning_set(&t, bad[i]) == ESP_ERR_INVALID_ARG);
        assert(memcmp(&t, &before, sizeof(t)) == 0);
    }
    assert(epd_pulse_tuning_set(&t, "refresh=pulse") == ESP_ERR_NOT_FOUND);
    puts("EPD pixel budgeting tests: ok");
}
