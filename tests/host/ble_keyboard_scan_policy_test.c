#include <assert.h>
#include <stdio.h>

#include "solar_os_ble_keyboard_scan_policy.h"

int main(void)
{
    assert(solar_os_ble_keyboard_scan_name_is_keyboard_like("BLE Keyboard"));
    assert(solar_os_ble_keyboard_scan_name_is_keyboard_like("KEYCHRON K3"));
    assert(solar_os_ble_keyboard_scan_name_is_keyboard_like("tiny-kbd"));
    assert(!solar_os_ble_keyboard_scan_name_is_keyboard_like("mouse"));
    assert(!solar_os_ble_keyboard_scan_name_is_keyboard_like(NULL));

    assert(!solar_os_ble_keyboard_scan_candidate_should_replace(
        false, false, 0, false, false, -20));
    assert(solar_os_ble_keyboard_scan_candidate_should_replace(
        false, false, 0, true, false, -80));
    assert(solar_os_ble_keyboard_scan_candidate_should_replace(
        true, false, -20, false, true, -90));
    assert(!solar_os_ble_keyboard_scan_candidate_should_replace(
        true, true, -90, true, false, -10));
    assert(solar_os_ble_keyboard_scan_candidate_should_replace(
        true, true, -70, true, true, -60));
    assert(!solar_os_ble_keyboard_scan_candidate_should_replace(
        true, true, -60, true, true, -60));

    puts("BLE keyboard scan policy tests: ok");
    return 0;
}
