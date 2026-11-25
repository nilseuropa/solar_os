#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "solar_os_native_client_ui.h"

int main(void)
{
    char text[40] = "";
    solar_os_ui_input_state_t state = {0, 0};
    assert(solar_os_ui_input_key(text, sizeof(text), &state, 'a', 4) == SOLAR_OS_UI_INPUT_CHANGED);
    assert(solar_os_ui_input_key(text, sizeof(text), &state, 'c', 4) == SOLAR_OS_UI_INPUT_CHANGED);
    assert(solar_os_ui_input_key(text, sizeof(text), &state, SOLAR_OS_KEY_LEFT, 4) == SOLAR_OS_UI_INPUT_NONE);
    assert(solar_os_ui_input_key(text, sizeof(text), &state, 'b', 4) == SOLAR_OS_UI_INPUT_CHANGED);
    assert(!strcmp(text, "abc") && state.cursor == 2);
    assert(solar_os_ui_input_key(text, sizeof(text), &state, SOLAR_OS_KEY_ENTER, 4) == SOLAR_OS_UI_INPUT_SUBMIT);
    assert(solar_os_ui_input_key(text, sizeof(text), &state, '\r', 4) == SOLAR_OS_UI_INPUT_SUBMIT);
    assert(solar_os_ui_input_key(text, sizeof(text), &state, SOLAR_OS_KEY_ESCAPE, 4) == SOLAR_OS_UI_INPUT_CANCEL);
    strcpy(text, "T\xc3\xb6ne"); state.cursor = strlen(text); state.view = 0;
    solar_os_ui_input_key(text, sizeof(text), &state, SOLAR_OS_KEY_LEFT, 3);
    assert(state.cursor == 4 && state.view == 1);
    assert(solar_os_ui_input_key(text, sizeof(text), &state, '\b', 3) == SOLAR_OS_UI_INPUT_CHANGED);
    assert(!strcmp(text, "T\xc3\xb6" "e") && state.cursor == 3);
    assert(solar_os_ui_input_key(text, sizeof(text), &state, SOLAR_OS_KEY_DELETE, 3) == SOLAR_OS_UI_INPUT_CHANGED);
    assert(!strcmp(text, "T\xc3\xb6"));
    solar_os_ui_input_key(text, sizeof(text), &state, '\b', 3);
    assert(!strcmp(text, "T") && state.cursor == 1);
    strcpy(text, "one two three"); state.cursor = strlen(text);
    solar_os_ui_input_key(text, sizeof(text), &state, SOLAR_OS_KEY_CTRL_LEFT, 5);
    assert(state.cursor == 8);
    solar_os_ui_input_key(text, sizeof(text), &state, SOLAR_OS_KEY_CTRL_LEFT, 5);
    assert(state.cursor == 4);
    solar_os_ui_input_key(text, sizeof(text), &state, SOLAR_OS_KEY_CTRL_RIGHT, 5);
    assert(state.cursor == 8);
    solar_os_ui_input_key(text, sizeof(text), &state, SOLAR_OS_KEY_HOME, 5);
    assert(!state.cursor && !state.view);
    solar_os_ui_input_key(text, sizeof(text), &state, SOLAR_OS_KEY_END, 5);
    assert(state.cursor == strlen(text) && state.view == state.cursor - 4);
    strcpy(text, "abc"); state.cursor = state.view = SIZE_MAX;
    assert(solar_os_ui_input_key(text, 4, &state, 'd', SIZE_MAX) == SOLAR_OS_UI_INPUT_NONE);
    assert(!strcmp(text, "abc") && state.cursor == 3 && state.view == 0);
    char unterminated[4] = {'a','b','c','d'};
    state.cursor = state.view = SIZE_MAX;
    assert(solar_os_ui_input_key(unterminated, sizeof(unterminated), &state, 'x', 4) == SOLAR_OS_UI_INPUT_NONE);
    assert(state.cursor == SIZE_MAX && unterminated[3] == 'd');
    assert(solar_os_ui_input_key(NULL, 4, &state, 'x', 4) == SOLAR_OS_UI_INPUT_NONE);
    assert(solar_os_ui_input_key(text, 0, &state, 'x', 4) == SOLAR_OS_UI_INPUT_NONE);
    assert(solar_os_ui_input_key(text, 4, NULL, 'x', 4) == SOLAR_OS_UI_INPUT_NONE);
    solar_os_ui_input_reconcile(NULL, &state, 4);
    solar_os_ui_input_reconcile(text, NULL, 4);
    const uint32_t scalars[] = {1, 0x7f, 0x80, 0x7ff, 0x800, 0xd7ff, 0xe000, 0xffff, 0x10000, 0x10ffff};
    for (size_t i = 0; i < sizeof(scalars) / sizeof(*scalars); i++) {
        char bytes[5] = {0}; uint32_t decoded = 0;
        size_t length = solar_os_ui_utf8_encode(scalars[i], bytes);
        assert(length && length <= 4 && solar_os_ui_utf8_decode(bytes, &decoded) == length);
        assert(decoded == scalars[i]);
    }
    char bytes[4] = {'x','x','x','x'};
    const char bounded[] = {'\xf0', '\x90', '\x80', '\x80'};
    for (size_t length = 1; length <= 4; length++) {
        uint32_t decoded = 0;
        assert(solar_os_ui_utf8_decode_n(bounded, length, &decoded) == (length == 4 ? 4 : 1));
        assert(decoded == (length == 4 ? 0x10000 : '?'));
    }
    uint32_t decoded = 42;
    assert(!solar_os_ui_utf8_decode_n(bounded, 0, &decoded) && decoded == 42);
    assert(!solar_os_ui_utf8_decode_n(NULL, 4, &decoded));
    assert(!solar_os_ui_utf8_decode_n(bounded, 4, NULL));
    strcpy(text, "ab"); state = (solar_os_ui_input_state_t){1, 0};
    const char umlaut[] = {'\xc3', '\xb6'};
    assert(solar_os_ui_input_text(text, sizeof(text), &state, umlaut, 2, 4) == SOLAR_OS_UI_INPUT_CHANGED);
    assert(!strcmp(text, "a\xc3\xb6" "b") && state.cursor == 3 && !state.view);
    assert(solar_os_ui_input_text(text, sizeof(text), &state, bounded, 4, 4) == SOLAR_OS_UI_INPUT_CHANGED);
    assert(!strcmp(text, "a\xc3\xb6\xf0\x90\x80\x80" "b") && state.cursor == 7);
    const char *rejected[] = {"\n", "\x7f", "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"};
    char saved[sizeof(text)]; memcpy(saved, text, sizeof(text));
    solar_os_ui_input_state_t saved_state = state;
    for (size_t i = 0; i < sizeof(rejected) / sizeof(*rejected); i++) {
        assert(solar_os_ui_input_text(text, sizeof(text), &state, rejected[i], strlen(rejected[i]), 4) == SOLAR_OS_UI_INPUT_NONE);
        assert(!memcmp(saved, text, sizeof(text)) && !memcmp(&state, &saved_state, sizeof(state)));
    }
    assert(solar_os_ui_input_text(text, sizeof(text), &state, umlaut, 1, 4) == SOLAR_OS_UI_INPUT_NONE);
    assert(solar_os_ui_input_text(text, strlen(text) + 1, &state, umlaut, 2, 4) == SOLAR_OS_UI_INPUT_NONE);
    assert(solar_os_ui_input_text(text, sizeof(text), &state, text + 1, 2, 4) == SOLAR_OS_UI_INPUT_NONE);
    assert(!memcmp(saved, text, sizeof(text)) && !memcmp(&state, &saved_state, sizeof(state)));
    assert(solar_os_ui_input_text(text, sizeof(text), &state, "", 0, 4) == SOLAR_OS_UI_INPUT_NONE);
    assert(solar_os_ui_input_text(NULL, sizeof(text), &state, umlaut, 2, 4) == SOLAR_OS_UI_INPUT_NONE);
    assert(solar_os_ui_input_text(text, 0, &state, umlaut, 2, 4) == SOLAR_OS_UI_INPUT_NONE);
    assert(solar_os_ui_input_text(text, sizeof(text), NULL, umlaut, 2, 4) == SOLAR_OS_UI_INPUT_NONE);
    state.cursor = SIZE_MAX;
    assert(solar_os_ui_input_text(text, sizeof(text), &state, "Z", 1, 4) == SOLAR_OS_UI_INPUT_CHANGED);
    assert(state.cursor == strlen(text) && text[state.cursor - 1] == 'Z');
    assert(!solar_os_ui_utf8_encode(0xd800, bytes) && bytes[0] == 'x');
    assert(!solar_os_ui_utf8_encode(0xdfff, bytes) && bytes[0] == 'x');
    assert(!solar_os_ui_utf8_encode(0x110000, bytes) && bytes[0] == 'x');
    assert(!solar_os_ui_utf8_encode('x', NULL));
    const char *invalid[] = {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80", "\xe2\x82"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        uint32_t decoded = 0;
        assert(solar_os_ui_utf8_decode(invalid[i], &decoded) == 1 && decoded == '?');
    }
    puts("Shared UI input: insertion, UTF-8 boundaries, word navigation, bounds and scalar validation passed");
    return 0;
}
