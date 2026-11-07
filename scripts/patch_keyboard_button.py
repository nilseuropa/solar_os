#!/usr/bin/env python3
"""SolarOS integration overlay for espressif/keyboard_button 1.0.2.

Keep Espressif's scanner, debounce and task. Use open-drain active-low drive
for bare switch matrices, allow RC settling, start after callback registration,
check task creation, and fix the full-matrix release overread. Power-save is
disabled by the SolarOS adapter; the component must not own global sleep policy.
"""
import argparse
from pathlib import Path


def patch(component: Path) -> None:
    edits = {
        "include/keyboard_button.h": [
            ("    BaseType_t core_id;", "    bool start_paused; /*!< SolarOS: start explicitly after registering callbacks. */\n    BaseType_t core_id;"),
            ("esp_err_t keyboard_button_delete(keyboard_btn_handle_t kbd_handle);",
             "esp_err_t keyboard_button_delete(keyboard_btn_handle_t kbd_handle);\n\nesp_err_t solar_os_keyboard_button_start(keyboard_btn_handle_t kbd_handle);"),
        ],
        "src/kbd_gpio.c": [
            ("gpio_conf.mode = GPIO_MODE_OUTPUT;",
             "gpio_conf.mode = config->active_level ? GPIO_MODE_OUTPUT : GPIO_MODE_OUTPUT_OD; /* SolarOS: release inactive lines. */"),
            ("        gpio_config(&gpio_conf);",
             "        if (config->gpio_mode == KBD_GPIO_MODE_OUTPUT) {\n"
             "            gpio_set_level(config->gpios[i], !config->active_level);\n"
             "        }\n"
             "        ret = gpio_config(&gpio_conf);\n"
             "        ESP_RETURN_ON_FALSE(ret == ESP_OK, ret, TAG, \"GPIO configuration failed\");"),
        ],
        "keyboard_button.c": [
            ('#include "keyboard_button.h"', '#include "keyboard_button.h"\n#include "esp_rom_sys.h"'),
            ("        /*!< Read the input level */",
             "        esp_rom_delay_us(5); /* SolarOS: settle weak pull-ups between scan lines. */\n        /*!< Read the input level */"),
            ("for (int l = k; l < kbd->key_pressed_num; l++)",
             "for (int l = k; l + 1 < kbd->key_pressed_num; l++)"),
            ("    if (!kbd->gptimer_start) {\n        ret = kbd_gptimer_start",
             "    if (!kbd_cfg->start_paused && !kbd->gptimer_start) {\n        ret = kbd_gptimer_start"),
            ('    xTaskCreatePinnedToCore(kbd_task, "kbd_task", 4096, kbd, priority, NULL, kbd_cfg->core_id);',
             '    ESP_GOTO_ON_FALSE(xTaskCreatePinnedToCore(kbd_task, "kbd_task", 4096, kbd, priority, NULL, kbd_cfg->core_id) == pdPASS,\n'
             '                      ESP_ERR_NO_MEM, exit, TAG, "Failed to create keyboard task");'),
            ("esp_err_t keyboard_button_delete(keyboard_btn_handle_t kbd_handle)\n{",
             "esp_err_t solar_os_keyboard_button_start(keyboard_btn_handle_t kbd_handle)\n"
             "{\n"
             "    if (kbd_handle == NULL) return ESP_ERR_INVALID_ARG;\n"
             "    keyboard_btn_t *kbd = (keyboard_btn_t *)kbd_handle;\n"
             "    if (kbd->gptimer_start) return ESP_OK;\n"
             "    esp_err_t ret = kbd_gptimer_start(kbd->gptimer_handle);\n"
             "    if (ret == ESP_OK) kbd->gptimer_start = true;\n"
             "    return ret;\n"
             "}\n\n"
             "esp_err_t keyboard_button_delete(keyboard_btn_handle_t kbd_handle)\n{"),
        ],
        "src/kbd_gptimer.c": [
            ('    ESP_RETURN_ON_FALSE(err == ESP_OK, err, TAG, "Failed to stop gptimer");',
             '    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "Failed to stop gptimer");'),
            ('    ESP_RETURN_ON_FALSE(err == ESP_OK, err, TAG, "Failed to disable gptimer");',
             '    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "Failed to disable gptimer");'),
        ],
    }
    pending = {}
    for relative, replacements in edits.items():
        path = component / relative
        source = path.read_text()
        for old, new in replacements:
            if new in source:
                continue
            # The timer stop/disable checks occur in both stop and deinit.
            expected = 2 if relative == "src/kbd_gptimer.c" else 1
            if source.count(old) != expected:
                raise ValueError(f"keyboard_button 1.0.2 changed: review overlay for {relative}: {old!r}")
            source = source.replace(old, new)
        pending[path] = source
    for path, source in pending.items():
        if path.read_text() != source:
            path.write_text(source)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("component", type=Path)
    patch(parser.parse_args().component)
