#pragma once
/* Shared frontend value models. These pure SDK helpers require no service
 * lookup, allocation, rendering object or application scope. TUI and GUI views
 * use the same cursor/top model; only their visible-row calculation differs. */
#include <string.h>
#include "solar_os_tui_types_v1.h"
#include "solar_os_key_codes.h"
#include "solar_os_native_client_ui_utf8.h"

typedef solar_os_tui_viewport_t solar_os_ui_viewport_t;

static inline void solar_os_ui_viewport_reconcile(solar_os_ui_viewport_t *view,
    size_t count, size_t visible)
{
    if (!view) return;
    if (!count || !visible) { view->cursor = view->top = 0; return; }
    if (view->cursor >= count) view->cursor = count - 1;
    if (view->top > view->cursor) view->top = view->cursor;
    if (view->cursor - view->top >= visible) view->top = view->cursor - visible + 1;
    const size_t last_top = count > visible ? count - visible : 0;
    if (view->top > last_top) view->top = last_top;
}

/* Return whether selection changed. Unrecognized keys are left to the app.
 * Empty/hidden views do not consume navigation; reconcile them when laying
 * out the next frame. Page movement saturates even at SIZE_MAX boundaries. */
static inline bool solar_os_ui_viewport_key(solar_os_ui_viewport_t *view,
    uint8_t key, size_t count, size_t visible, bool wrap)
{
    if (!view || !count || !visible) return false;
    solar_os_ui_viewport_reconcile(view, count, visible);
    const size_t before = view->cursor;
    switch (key) {
    case SOLAR_OS_KEY_UP:
        view->cursor = before ? before - 1 : (wrap ? count - 1 : 0); break;
    case SOLAR_OS_KEY_DOWN:
        view->cursor = before < count - 1 ? before + 1 : (wrap ? 0 : before); break;
    case SOLAR_OS_KEY_PAGE_UP:
        view->cursor = before > visible ? before - visible : 0; break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        view->cursor = visible < count - 1 - before ? before + visible : count - 1; break;
    case SOLAR_OS_KEY_HOME:
        view->cursor = 0; break;
    case SOLAR_OS_KEY_END:
        view->cursor = count - 1; break;
    default:
        return false;
    }
    solar_os_ui_viewport_reconcile(view, count, visible);
    return view->cursor != before;
}

/* Input positions are UTF-8 byte offsets. Initialize cursor to a character
 * boundary (usually strlen(text)); key editing preserves those boundaries.
 * Keys are normalized logical bytes. Prefilled UTF-8 text can be navigated and
 * deleted by whole codepoint. No pointers or buffers are retained. */
typedef solar_os_tui_input_state_t solar_os_ui_input_state_t;
typedef solar_os_tui_input_action_t solar_os_ui_input_action_t;
#define SOLAR_OS_UI_INPUT_NONE SOLAR_OS_TUI_INPUT_NONE
#define SOLAR_OS_UI_INPUT_CHANGED SOLAR_OS_TUI_INPUT_CHANGED
#define SOLAR_OS_UI_INPUT_SUBMIT SOLAR_OS_TUI_INPUT_SUBMIT
#define SOLAR_OS_UI_INPUT_CANCEL SOLAR_OS_TUI_INPUT_CANCEL

static inline bool solar_os_ui_input_space(unsigned char c)
{ return c == ' ' || (c >= '\t' && c <= '\r'); }

static inline size_t solar_os_ui_input_length(const char *text, size_t capacity)
{
    size_t length = 0;
    if (text) while (length < capacity && text[length]) length++;
    return length;
}

static inline void solar_os_ui_input_reconcile(const char *text,
    solar_os_ui_input_state_t *state, size_t visible_cells)
{
    if (!text || !state) return;
    const size_t len = strlen(text);
    if (state->cursor > len) state->cursor = len;
    size_t view = state->cursor;
    if (!visible_cells) visible_cells = 1;
    for (size_t cells = 1; cells < visible_cells && view; cells++)
        view = solar_os_ui_utf8_previous(text, view);
    state->view = view;
}

static inline solar_os_ui_input_action_t solar_os_ui_input_key(
    char *text,
    size_t capacity,
    solar_os_ui_input_state_t *state,
    uint32_t key,
    size_t visible_cells)
{
    if (text == NULL || capacity == 0 || state == NULL) return SOLAR_OS_UI_INPUT_NONE;
    const size_t len = solar_os_ui_input_length(text, capacity);
    if (len >= capacity) return SOLAR_OS_UI_INPUT_NONE;
    if (state->cursor > len) state->cursor = len;
    solar_os_ui_input_action_t action = SOLAR_OS_UI_INPUT_NONE;
    if (key == SOLAR_OS_KEY_ENTER || key == '\r') return SOLAR_OS_UI_INPUT_SUBMIT;
    if (key == SOLAR_OS_KEY_ESCAPE) return SOLAR_OS_UI_INPUT_CANCEL;
    if (key == SOLAR_OS_KEY_LEFT) state->cursor = solar_os_ui_utf8_previous(text, state->cursor);
    else if (key == SOLAR_OS_KEY_RIGHT) state->cursor = solar_os_ui_utf8_next(text, state->cursor);
    else if (key == SOLAR_OS_KEY_CTRL_LEFT) {
        while (state->cursor > 0 && solar_os_ui_input_space((unsigned char)text[state->cursor - 1U])) {
            state->cursor = solar_os_ui_utf8_previous(text, state->cursor);
        }
        while (state->cursor > 0 &&
               !solar_os_ui_input_space((unsigned char)text[state->cursor - 1U])) {
            state->cursor = solar_os_ui_utf8_previous(text, state->cursor);
        }
    } else if (key == SOLAR_OS_KEY_CTRL_RIGHT) {
        while (state->cursor < len &&
               !solar_os_ui_input_space((unsigned char)text[state->cursor])) {
            state->cursor = solar_os_ui_utf8_next(text, state->cursor);
        }
        while (state->cursor < len && solar_os_ui_input_space((unsigned char)text[state->cursor])) {
            state->cursor = solar_os_ui_utf8_next(text, state->cursor);
        }
    } else if (key == SOLAR_OS_KEY_HOME) state->cursor = 0;
    else if (key == SOLAR_OS_KEY_END) state->cursor = len;
    else if (key == '\b' || key == 0x7fU) {
        const size_t previous = solar_os_ui_utf8_previous(text, state->cursor);
        if (previous != state->cursor) {
            memmove(text + previous, text + state->cursor, len - state->cursor + 1U);
            state->cursor = previous;
            action = SOLAR_OS_UI_INPUT_CHANGED;
        }
    } else if (key == SOLAR_OS_KEY_DELETE) {
        const size_t next = solar_os_ui_utf8_next(text, state->cursor);
        if (next != state->cursor) {
            memmove(text + state->cursor, text + next, len - next + 1U);
            action = SOLAR_OS_UI_INPUT_CHANGED;
        }
    } else if (key >= 0x20U && key != 0x7fU && key < SOLAR_OS_KEY_UP) {
        char bytes[4];
        const size_t count = solar_os_ui_utf8_encode(key, bytes);
        if (len + count < capacity) {
            memmove(text + state->cursor + count, text + state->cursor,
                    len - state->cursor + 1U);
            memcpy(text + state->cursor, bytes, count);
            state->cursor += count;
            action = SOLAR_OS_UI_INPUT_CHANGED;
        }
    }
    solar_os_ui_input_reconcile(text, state, visible_cells > 0 ? visible_cells : 1U);
    return action;
}

/* Insert a complete UTF-8 span at the cursor. Validate the whole span and
 * capacity before changing text or state. Source and destination must not
 * overlap; controls and malformed/incomplete codepoints are rejected. */
static inline solar_os_ui_input_action_t solar_os_ui_input_text(char *text,
    size_t capacity, solar_os_ui_input_state_t *state, const char *input,
    size_t length, size_t visible_cells)
{
    if (!text || !capacity || !state || !input || !length) return SOLAR_OS_UI_INPUT_NONE;
    const size_t used = solar_os_ui_input_length(text, capacity);
    if (used >= capacity || length > capacity - used - 1) return SOLAR_OS_UI_INPUT_NONE;
    const uintptr_t source = (uintptr_t)input, target = (uintptr_t)text;
    if (source >= target ? source - target < capacity : target - source < length)
        return SOLAR_OS_UI_INPUT_NONE;
    for (size_t offset = 0; offset < length; ) {
        uint32_t codepoint;
        const size_t bytes = solar_os_ui_utf8_decode_n(input + offset, length - offset, &codepoint);
        if (!bytes || codepoint < 0x20U || codepoint == 0x7fU ||
            (bytes == 1 && (unsigned char)input[offset] >= 0x80U)) return SOLAR_OS_UI_INPUT_NONE;
        offset += bytes;
    }
    size_t cursor = state->cursor < used ? state->cursor : used;
    while (cursor && ((unsigned char)text[cursor] & 0xc0U) == 0x80U) cursor--;
    memmove(text + cursor + length, text + cursor, used - cursor + 1);
    memcpy(text + cursor, input, length);
    state->cursor = cursor + length;
    solar_os_ui_input_reconcile(text, state, visible_cells);
    return SOLAR_OS_UI_INPUT_CHANGED;
}
