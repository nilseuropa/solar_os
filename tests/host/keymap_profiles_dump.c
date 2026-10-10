/* Cross-language profile/JSON validation harness, run by test_keymap_tool.py. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_input_keymap_json.h"
#include "solar_os_inputronic_keyboard_profile.h"
#include "solar_os_lilygo_pager_keyboard.h"

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    solar_os_matrix_keyboard_map_t map;
    if (strcmp(argv[1], "inputronic-keyboard") == 0) solar_os_inputronic_keyboard_map(&map);
    else if (strcmp(argv[1], "lilygo-pager-keyboard") == 0) solar_os_lilygo_pager_keyboard_map(&map);
    else if (strcmp(argv[1], "tca8418") == 0) {
        if (solar_os_matrix_keyboard_default_map(&map, 8, 10) != ESP_OK) return 2;
    } else return 2;
    if (argc == 3) {
        FILE *file = fopen(argv[2], "rb");
        if (file == NULL) return 2;
        char json[16386];
        const size_t length = fread(json, 1, sizeof(json) - 1, file);
        const bool error = ferror(file) || length > 16384 || memchr(json, 0, length) != NULL;
        fclose(file);
        if (error) return 1;
        json[length] = 0;
        if (solar_os_input_keymap_parse(json, &map, &map) != ESP_OK) return 1;
    }
    printf("{\"rows\":%u,\"cols\":%u,\"first\":%u,\"stride\":%u,\"keys\":[",
           map.rows, map.cols, map.first, map.stride);
    bool separator = false;
    for (unsigned layer = 0; layer < 2; layer++) {
        for (unsigned slot = 1; slot <= map.slot_count; slot++) {
            if (map.physical[slot] == 0) continue;
            solar_os_input_keymap_key_t key = map.keys[layer][slot];
            printf("%s[%u,%u,%u,%u,%u,%u]", separator ? "," : "",
                layer, map.physical[slot], key.usage, key.key, key.shifted_key, key.flags);
            separator = true;
        }
    }
    puts("]}");
    return 0;
}
