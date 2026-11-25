#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "solar_os_native_client_ui.h"

static void invariant(const solar_os_ui_viewport_t *view, size_t count, size_t visible)
{
    if (!count || !visible) { assert(!view->cursor && !view->top); return; }
    assert(view->cursor < count && view->top <= view->cursor);
    assert(view->cursor - view->top < visible);
    assert(view->top <= (count > visible ? count - visible : 0));
}
int main(void)
{
    solar_os_ui_viewport_t view = {0, 0};
    assert(!solar_os_ui_viewport_key(&view, SOLAR_OS_KEY_UP, 12, 5, false));
    assert(solar_os_ui_viewport_key(&view, SOLAR_OS_KEY_UP, 12, 5, true));
    assert(view.cursor == 11 && view.top == 7);
    assert(solar_os_ui_viewport_key(&view, SOLAR_OS_KEY_DOWN, 12, 5, true));
    assert(!view.cursor && !view.top);
    assert(solar_os_ui_viewport_key(&view, SOLAR_OS_KEY_PAGE_DOWN, 12, 5, false));
    assert(view.cursor == 5 && view.top == 1);
    assert(solar_os_ui_viewport_key(&view, SOLAR_OS_KEY_END, 12, 5, false));
    assert(view.cursor == 11 && view.top == 7);
    assert(!solar_os_ui_viewport_key(&view, SOLAR_OS_KEY_DOWN, 12, 5, false));
    assert(solar_os_ui_viewport_key(&view, SOLAR_OS_KEY_HOME, 12, 5, false));
    assert(!view.cursor && !view.top);
    assert(!solar_os_ui_viewport_key(&view, 'q', 12, 5, false));
    const size_t sizes[] = {0, 1, 2, 7, 99, SIZE_MAX / 2, SIZE_MAX - 1, SIZE_MAX};
    const uint8_t keys[] = {SOLAR_OS_KEY_UP, SOLAR_OS_KEY_DOWN, SOLAR_OS_KEY_PAGE_UP,
        SOLAR_OS_KEY_PAGE_DOWN, SOLAR_OS_KEY_HOME, SOLAR_OS_KEY_END, 'q'};
    for (size_t c = 0; c < sizeof(sizes) / sizeof(*sizes); c++)
    for (size_t v = 0; v < sizeof(sizes) / sizeof(*sizes); v++)
    for (size_t initial = 0; initial < sizeof(sizes) / sizeof(*sizes); initial++)
    for (size_t top = 0; top < sizeof(sizes) / sizeof(*sizes); top++) {
        view = (solar_os_ui_viewport_t){sizes[initial], sizes[top]};
        size_t count = sizes[c], visible = sizes[v];
        solar_os_ui_viewport_reconcile(&view, count, visible);
        invariant(&view, count, visible);
        solar_os_ui_viewport_t before = view;
        solar_os_ui_viewport_reconcile(&view, count, visible);
        assert(view.cursor == before.cursor && view.top == before.top);
        for (size_t k = 0; k < sizeof(keys) / sizeof(*keys); k++) {
            for (unsigned wrap = 0; wrap < 2; wrap++) {
                solar_os_ui_viewport_t step = view;
                bool moved = solar_os_ui_viewport_key(&step, keys[k], count, visible, wrap != 0);
                invariant(&step, count, visible);
                assert(moved == (step.cursor != view.cursor));
            }
        }
    }
    solar_os_ui_viewport_reconcile(NULL, 1, 1);
    assert(!solar_os_ui_viewport_key(NULL, SOLAR_OS_KEY_DOWN, 1, 1, true));
    puts("Shared UI viewport: navigation, wrapping, page fill and full-size boundary invariants passed");
    return 0;
}
