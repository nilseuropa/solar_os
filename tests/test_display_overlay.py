from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]


class DisplayOverlayTest(unittest.TestCase):
    def test_overlay_clear_repaints_its_own_terminal(self):
        functions = production_functions(
            (ROOT / "src/services/solar_os_display.c").read_text(),
            ("solar_os_display_terminal_draw_state", "solar_os_display_set_overlay_active"),
        )
        functions += production_functions(
            (ROOT / "src/services/solar_os_terminal.c").read_text(),
            ("solar_os_terminal_needs_draw",),
        )
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
typedef int esp_err_t;
enum {ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE};
typedef struct {int id;} u8g2_t;
typedef void *SemaphoreHandle_t;
typedef struct {
    bool active, overlay_active, overlay_pending, terminal_blocked;
    uint32_t generation, terminal_epoch;
    unsigned refs, overlay_tile_x, overlay_tile_y, overlay_tile_width, overlay_tile_height;
    uint8_t *overlay_buffer;
    SemaphoreHandle_t present_mutex;
    struct {u8g2_t *u8g2;} target;
} display_target_slot_t;
static display_target_slot_t display_targets[2];
static int display_targets_lock;
static bool refuse_lock;
static unsigned freed;
#define portMAX_DELAY 0xffffffffU
#define pdTRUE 1
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int display_find_slot_by_u8g2_locked(const u8g2_t *screen)
{
    for (unsigned i = 0; i < 2; i++)
        if (display_targets[i].active && display_targets[i].target.u8g2 == screen) return i;
    return -1;
}
static int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned wait)
{assert(mutex && wait == portMAX_DELAY); return !refuse_lock;}
static void xSemaphoreGive(SemaphoreHandle_t mutex) {assert(mutex);}
static void display_release_slot_ref(size_t index, uint32_t generation)
{assert(display_targets[index].generation == generation && display_targets[index].refs == 1); display_targets[index].refs--;}
static void solar_os_memory_free(void *buffer) {if (buffer) {freed++; free(buffer);}}
typedef int solar_os_terminal_cursor_shape_t;
typedef struct {
    u8g2_t *u8g2;
    bool dirty, rendered_cursor_blink, rendered_cursor_on;
    uint32_t rendered_display_epoch, rendered_font_pack_generation;
    solar_os_terminal_cursor_shape_t rendered_cursor_shape;
} solar_os_terminal_t;
static void solar_os_terminal_get_cursor_preference_for_display(const u8g2_t *screen,
    solar_os_terminal_cursor_shape_t *shape, bool *blink)
{(void)screen; *shape = 0; *blink = false;}
static unsigned solar_os_font_pack_generation(void) {return 0;}
static bool terminal_cursor_is_drawn(const solar_os_terminal_t *terminal, bool blink)
{(void)terminal; (void)blink; return false;}
/* FUNCTIONS */
int main(void)
{
    u8g2_t screens[3] = {{0}, {1}, {2}};
    solar_os_terminal_t terminals[2] = {
        {.u8g2 = &screens[0], .rendered_display_epoch = 5},
        {.u8g2 = &screens[1], .rendered_display_epoch = 5},
    };
    for (unsigned i = 0; i < 2; i++) {
        display_targets[i].active = true;
        display_targets[i].generation = 9 + i;
        display_targets[i].terminal_epoch = 5;
        display_targets[i].target.u8g2 = &screens[i];
        display_targets[i].present_mutex = (void *)(uintptr_t)(i + 1);
    }
    assert(!solar_os_terminal_needs_draw(&terminals[0]));
    assert(!solar_os_terminal_needs_draw(&terminals[1]));
    display_targets[1].overlay_buffer = malloc(8);
    assert(display_targets[1].overlay_buffer);
    display_targets[1].overlay_tile_x = display_targets[1].overlay_tile_y = 1;
    display_targets[1].overlay_tile_width = display_targets[1].overlay_tile_height = 1;
    assert(solar_os_display_set_overlay_active(&screens[1], true) == ESP_OK);
    assert(display_targets[1].terminal_epoch == 5);
    assert(solar_os_display_set_overlay_active(&screens[1], false) == ESP_OK);
    assert(solar_os_terminal_needs_draw(&terminals[1])); /* Detached right shell. */
    assert(!solar_os_terminal_needs_draw(&terminals[0])); /* Foreground left shell. */
    assert(display_targets[1].terminal_epoch == 6 && !display_targets[1].overlay_buffer);
    assert(!display_targets[1].overlay_active && !display_targets[1].overlay_pending && freed == 1);
    assert(!display_targets[1].overlay_tile_x && !display_targets[1].overlay_tile_y);
    assert(!display_targets[1].overlay_tile_width && !display_targets[1].overlay_tile_height);

    terminals[1].rendered_display_epoch = 6;
    assert(solar_os_display_set_overlay_active(&screens[1], false) == ESP_OK);
    assert(!solar_os_terminal_needs_draw(&terminals[1])); /* Idempotent clear. */
    display_targets[1].overlay_pending = true;
    display_targets[1].overlay_buffer = malloc(8);
    assert(display_targets[1].overlay_buffer);
    assert(solar_os_display_set_overlay_active(&screens[1], false) == ESP_OK);
    assert(solar_os_terminal_needs_draw(&terminals[1]) && freed == 2);

    display_targets[0].overlay_active = true;
    assert(solar_os_display_set_overlay_active(&screens[0], false) == ESP_OK);
    assert(solar_os_terminal_needs_draw(&terminals[0])); /* Switch away from left. */
    uint32_t epoch = display_targets[1].terminal_epoch;
    assert(solar_os_display_set_overlay_active(NULL, false) == ESP_ERR_INVALID_ARG);
    assert(solar_os_display_set_overlay_active(&screens[2], false) == ESP_ERR_NOT_FOUND);
    refuse_lock = true;
    assert(solar_os_display_set_overlay_active(&screens[1], false) == ESP_ERR_INVALID_STATE);
    assert(display_targets[1].terminal_epoch == epoch);
    assert(!display_targets[0].refs && !display_targets[1].refs);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(harness.replace("/* FUNCTIONS */", functions))
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                str(path / "test.c"), "-o", str(path / "test"),
            ], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
