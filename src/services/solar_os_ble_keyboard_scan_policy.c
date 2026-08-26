#include "solar_os_ble_keyboard_scan_policy.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static bool contains_case_insensitive(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }

    const size_t needle_len = strlen(needle);
    for (const char *start = haystack; *start != '\0'; start++) {
        size_t index = 0U;
        while (index < needle_len && start[index] != '\0' &&
               tolower((unsigned char)start[index]) ==
                   tolower((unsigned char)needle[index])) {
            index++;
        }
        if (index == needle_len) {
            return true;
        }
    }
    return false;
}

bool solar_os_ble_keyboard_scan_name_is_keyboard_like(const char *name)
{
    return contains_case_insensitive(name, "keyboard") ||
        contains_case_insensitive(name, "kbd") ||
        contains_case_insensitive(name, "keychron");
}

bool solar_os_ble_keyboard_scan_candidate_should_replace(
    bool current_valid,
    bool current_keyboard_like,
    int8_t current_rssi,
    bool next_has_hid_service,
    bool next_keyboard_like,
    int8_t next_rssi)
{
    if (!next_has_hid_service && !next_keyboard_like) {
        return false;
    }
    if (!current_valid) {
        return true;
    }
    if (current_keyboard_like != next_keyboard_like) {
        return next_keyboard_like;
    }
    return next_rssi > current_rssi;
}
