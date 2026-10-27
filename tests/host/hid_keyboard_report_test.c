#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_hid_keyboard_report.h"
#include "solar_os_ble_hid_report_map.h"

static bool state_has_key(const solar_os_hid_keyboard_report_state_t *state,
                          uint8_t key)
{
    for (size_t i = 0; i < SOLAR_OS_HID_KEYBOARD_REPORT_KEYS; i++) {
        if (state->keys[i] == key) {
            return true;
        }
    }
    return false;
}

static void check_descriptor_layouts(void)
{
    /* Report 1: modifier byte followed immediately by ten key-array slots.
     * Report 2: boot-style reserved byte and six slots. Report IDs are carried
     * in HOGP metadata, so they do not occupy a notification payload byte. */
    const uint8_t map[] = {
        0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
        0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
        0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
        0x19, 0x00, 0x29, 0x65, 0x75, 0x08, 0x95, 0x0a, 0x81, 0x00,
        0x85, 0x02, 0x19, 0xe0, 0x29, 0xe7,
        0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
        0x75, 0x08, 0x95, 0x01, 0x81, 0x01,
        0x19, 0x00, 0x29, 0x65, 0x95, 0x06, 0x81, 0x00, 0xc0,
    };
    solar_os_hid_keyboard_report_layout_t compact, boot;
    bool ids[256];
    assert(solar_os_ble_hid_report_map(map, sizeof(map), ids));
    assert(ids[1] && ids[2] && !ids[0]);
    assert(solar_os_ble_hid_report_layout(map, sizeof(map), 1, &compact));
    assert(compact.modifiers_offset == 0 && compact.keys_offset == 1 &&
           compact.key_count == 10 && compact.report_size == 11);
    assert(solar_os_ble_hid_report_layout(map, sizeof(map), 2, &boot));
    assert(boot.modifiers_offset == 0 && boot.keys_offset == 2 &&
           boot.key_count == 6 && boot.report_size == 8);
    solar_os_hid_keyboard_report_layout_t invalid;
    assert(!solar_os_ble_hid_report_layout(map, sizeof(map), 3, &invalid));
    assert(invalid.key_count == 0);

    solar_os_hid_keyboard_report_tracker_t tracker = {0};
    solar_os_hid_keyboard_report_state_t state;
    const uint8_t idle[11] = {0};
    /* Replay the captured T, G, B taps and A-held combinations. */
    const uint8_t usages[] = {0x17, 0x0a, 0x05};
    for (size_t i = 0; i < sizeof(usages); i++) {
        const uint8_t tap[11] = {0, usages[i]};
        assert(solar_os_hid_keyboard_report_update_with_layout(
            &tracker, 0, 1, tap, sizeof(tap), &compact, &state));
        assert(state.keys[0] == usages[i] && state.keys[1] == 0);
        assert(solar_os_hid_keyboard_report_update_with_layout(
            &tracker, 0, 1, idle, sizeof(idle), &compact, &state));
        assert(state.keys[0] == 0);
        const uint8_t chord[11] = {0, 0x04, usages[i]};
        assert(solar_os_hid_keyboard_report_update_with_layout(
            &tracker, 0, 1, chord, sizeof(chord), &compact, &state));
        assert(state_has_key(&state, 0x04) && state_has_key(&state, usages[i]));
        const uint8_t held[11] = {0, 0x04};
        assert(solar_os_hid_keyboard_report_update_with_layout(
            &tracker, 0, 1, held, sizeof(held), &compact, &state));
        assert(state.keys[0] == 0x04 && state.keys[1] == 0);
        assert(solar_os_hid_keyboard_report_update_with_layout(
            &tracker, 0, 1, idle, sizeof(idle), &compact, &state));
        assert(state.keys[0] == 0);
    }
    /* Read every declared array slot, and retain the existing six-key limit. */
    const uint8_t last_slot[11] = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x04};
    assert(solar_os_hid_keyboard_report_update_with_layout(
        &tracker, 0, 1, last_slot, sizeof(last_slot), &compact, &state));
    assert(state.modifiers == 0x02 && state.keys[0] == 0x04);
    const uint8_t full[11] = {0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
    assert(solar_os_hid_keyboard_report_update_with_layout(
        &tracker, 0, 1, full, sizeof(full), &compact, &state));
    for (size_t i = 0; i < SOLAR_OS_HID_KEYBOARD_REPORT_KEYS; i++)
        assert(state.keys[i] == 4 + i);
    const solar_os_hid_keyboard_report_state_t previous = state;
    assert(!solar_os_hid_keyboard_report_update_with_layout(
        &tracker, 0, 1, idle, sizeof(idle) - 1, &compact, &state));
    assert(!memcmp(&previous, &state, sizeof(state)));

    /* The same payload length can describe a reserved byte plus nine slots.
     * Offsets must follow the descriptor rather than a length=11 heuristic. */
    uint8_t padded[sizeof(map)];
    memcpy(padded, map, sizeof(map));
    padded[sizeof(map) - 4] = 9;
    assert(solar_os_ble_hid_report_layout(padded, sizeof(padded), 2, &boot));
    assert(boot.report_size == 11 && boot.keys_offset == 2);
    solar_os_hid_keyboard_report_reset(&tracker);
    const uint8_t padded_t[11] = {0, 0, 0x17};
    assert(solar_os_hid_keyboard_report_update_with_layout(
        &tracker, 0, 2, padded_t, sizeof(padded_t), &boot, &state));
    assert(state.keys[0] == 0x17);

    /* Global Push/Pop restores field size/count. Output and Feature fields
     * have separate payloads and must not advance the Input bit offset. */
    const uint8_t offset_map[] = {
        0x05, 1, 0x09, 6, 0xa1, 1, 0x05, 7,
        0x75, 8, 0x95, 1, 0x81, 1,
        0x95, 4, 0xa4,
        0x19, 0xe0, 0x29, 0xe7, 0x75, 1, 0x95, 8, 0x81, 2,
        0xb4, 0x91, 1, 0xb1, 1,
        0x19, 0, 0x29, 0x65, 0x81, 0, 0xc0,
    };
    solar_os_hid_keyboard_report_layout_t offset_layout;
    assert(solar_os_ble_hid_report_layout(offset_map, sizeof(offset_map), 0, &offset_layout));
    assert(offset_layout.modifiers_offset == 1 && offset_layout.keys_offset == 2 &&
           offset_layout.key_count == 4 && offset_layout.report_size == 6);
    const uint8_t shifted_t[] = {0xff, 0x02, 0x17, 0, 0, 0};
    solar_os_hid_keyboard_report_reset(&tracker);
    assert(solar_os_hid_keyboard_report_update_with_layout(
        &tracker, 0, 0, shifted_t, sizeof(shifted_t), &offset_layout, &state));
    assert(state.modifiers == 0x02 && state.keys[0] == 0x17);
    uint8_t unsupported[sizeof(offset_map)];
    memcpy(unsupported, offset_map, sizeof(offset_map));
    unsupported[sizeof(unsupported) - 2] = 2; /* Variable fields are not key arrays. */
    assert(!solar_os_ble_hid_report_layout(unsupported, sizeof(unsupported), 0, &invalid));
    assert(invalid.key_count == 0);

    for (size_t i = 1; i < sizeof(map); i++) {
        uint8_t *cut = malloc(i);
        assert(cut);
        memcpy(cut, map, i);
        (void)solar_os_ble_hid_report_layout(cut, i, 1, &invalid);
        free(cut);
    }
}

int main(void)
{
    check_descriptor_layouts();
    solar_os_hid_keyboard_report_tracker_t tracker = {0};
    solar_os_hid_keyboard_report_state_t state;

    const uint8_t boot_a[8] = {0, 0, 0x04};
    assert(solar_os_hid_keyboard_report_update(&tracker,
                                               0,
                                               1,
                                               boot_a,
                                               sizeof(boot_a),
                                               &state));
    assert(state.modifiers == 0);
    assert(state_has_key(&state, 0x04));
    const uint8_t no_keys[SOLAR_OS_HID_KEYBOARD_REPORT_KEYS] = {0};
    assert(solar_os_hid_keyboard_report_has_new_press(0, no_keys, &state));
    assert(!solar_os_hid_keyboard_report_has_new_press(0,
                                                       state.keys,
                                                       &state));
    const solar_os_hid_keyboard_report_state_t modifier_only = {
        .modifiers = 0x01,
    };
    assert(solar_os_hid_keyboard_report_has_new_press(0,
                                                       no_keys,
                                                       &modifier_only));
    assert(!solar_os_hid_keyboard_report_has_new_press(0x01,
                                                        no_keys,
                                                        &modifier_only));

    /* An idle report from another collection must not release map 0's key. */
    const uint8_t boot_idle[8] = {0};
    assert(solar_os_hid_keyboard_report_update(&tracker,
                                               1,
                                               2,
                                               boot_idle,
                                               sizeof(boot_idle),
                                               &state));
    assert(state_has_key(&state, 0x04));

    /* Mirrored reports are de-duplicated and remain held until both release. */
    assert(solar_os_hid_keyboard_report_update(&tracker,
                                               1,
                                               2,
                                               boot_a,
                                               sizeof(boot_a),
                                               &state));
    assert(state.keys[0] == 0x04);
    assert(state.keys[1] == 0);
    assert(solar_os_hid_keyboard_report_update(&tracker,
                                               0,
                                               1,
                                               boot_idle,
                                               sizeof(boot_idle),
                                               &state));
    assert(state_has_key(&state, 0x04));
    assert(solar_os_hid_keyboard_report_update(&tracker,
                                               1,
                                               2,
                                               boot_idle,
                                               sizeof(boot_idle),
                                               &state));
    assert(!state_has_key(&state, 0x04));

    /* Report protocol can omit the boot report's reserved byte. */
    const uint8_t report_shift_b[7] = {0x02, 0x05};
    assert(solar_os_hid_keyboard_report_update(&tracker,
                                               2,
                                               3,
                                               report_shift_b,
                                               sizeof(report_shift_b),
                                               &state));
    assert(state.modifiers == 0x02);
    assert(state_has_key(&state, 0x05));
    assert(solar_os_hid_keyboard_report_has_new_press(0, no_keys, &state));

    const uint8_t held_b[SOLAR_OS_HID_KEYBOARD_REPORT_KEYS] = {0x05};
    const solar_os_hid_keyboard_report_state_t release_b = {0};
    assert(!solar_os_hid_keyboard_report_has_new_press(0x02,
                                                       held_b,
                                                       &release_b));

    const solar_os_hid_keyboard_report_state_t previous = state;
    const uint8_t invalid[6] = {0};
    assert(!solar_os_hid_keyboard_report_update(&tracker,
                                                2,
                                                3,
                                                invalid,
                                                sizeof(invalid),
                                                &state));
    assert(memcmp(&state, &previous, sizeof(state)) == 0);

    solar_os_hid_keyboard_report_reset(&tracker);
    puts("HID keyboard report tests: ok");
    return 0;
}
