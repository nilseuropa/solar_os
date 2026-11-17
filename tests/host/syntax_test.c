#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_syntax.h"

static void expect(const char *line, const uint8_t *styles, const char *token,
                   solar_os_syntax_style_t style)
{
    const char *found = strstr(line, token);
    assert(found != NULL);
    for (size_t i = 0; i < strlen(token); i++) {
        assert(styles[(size_t)(found - line) + i] == style);
    }
}

static void highlight(solar_os_syntax_language_t language, solar_os_syntax_state_t *state,
                      const char *line, uint8_t *styles)
{
    assert(strlen(line) < 256);
    solar_os_syntax_highlight_line(language, state, line, strlen(line), 0, styles, strlen(line));
}

static void expect_same_state(const solar_os_syntax_state_t *a, const solar_os_syntax_state_t *b)
{
    assert(a->mode == b->mode && a->lua_long_equals == b->lua_long_equals);
    assert(a->c_escaped == b->c_escaped && a->c_continued == b->c_continued);
    assert(a->c_directive == b->c_directive && a->c_include == b->c_include);
}

static void test_extensions(void)
{
    assert(solar_os_syntax_language_for_path("/dev/hello.c") == SOLAR_OS_SYNTAX_C);
    assert(solar_os_syntax_language_for_path("/include/SDK.H") == SOLAR_OS_SYNTAX_C);
    assert(solar_os_syntax_language_for_path("HELLO.C") == SOLAR_OS_SYNTAX_C);
    assert(solar_os_syntax_language_for_path("main.c.py") == SOLAR_OS_SYNTAX_PYTHON);
    assert(solar_os_syntax_language_for_path("/x/main.pyw") == SOLAR_OS_SYNTAX_PYTHON);
    assert(solar_os_syntax_language_for_path("/x/main.lua") == SOLAR_OS_SYNTAX_LUA);
    assert(solar_os_syntax_language_for_path("/x/main.c.txt") == SOLAR_OS_SYNTAX_NONE);
    assert(solar_os_syntax_language_for_path(NULL) == SOLAR_OS_SYNTAX_NONE);
}

static void test_c_tokens(void)
{
    solar_os_syntax_state_t state;
    uint8_t styles[256];
    solar_os_syntax_state_init(&state);
    const char *include = "  #include <stdio.h> // header";
    highlight(SOLAR_OS_SYNTAX_C, &state, include, styles);
    expect(include, styles, "#include", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    expect(include, styles, "<stdio.h>", SOLAR_OS_SYNTAX_STYLE_STRING);
    expect(include, styles, "// header", SOLAR_OS_SYNTAX_STYLE_COMMENT);

    const char *types = "static const uint32_t mask = 0x2aUL; float x = .5f + 1e-3;";
    highlight(SOLAR_OS_SYNTAX_C, &state, types, styles);
    expect(types, styles, "static", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    expect(types, styles, "const", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    expect(types, styles, "uint32_t", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    expect(types, styles, "mask", SOLAR_OS_SYNTAX_STYLE_NORMAL);
    expect(types, styles, "0x2aUL", SOLAR_OS_SYNTAX_STYLE_NUMBER);
    expect(types, styles, ".5f", SOLAR_OS_SYNTAX_STYLE_NUMBER);
    expect(types, styles, "1e-3", SOLAR_OS_SYNTAX_STYLE_NUMBER);

    const char *text = "const char *s = \"/* text */ //\"; char ch = '\\n'; // tail";
    highlight(SOLAR_OS_SYNTAX_C, &state, text, styles);
    expect(text, styles, "\"/* text */ //\"", SOLAR_OS_SYNTAX_STYLE_STRING);
    expect(text, styles, "'\\n'", SOLAR_OS_SYNTAX_STYLE_STRING);
    expect(text, styles, "// tail", SOLAR_OS_SYNTAX_STYLE_COMMENT);

    const char *prefixed = "u8\"text\" L'x' U\"wide\" u'z' return_value iffy NULL true";
    highlight(SOLAR_OS_SYNTAX_C, &state, prefixed, styles);
    expect(prefixed, styles, "u8\"text\"", SOLAR_OS_SYNTAX_STYLE_STRING);
    expect(prefixed, styles, "L'x'", SOLAR_OS_SYNTAX_STYLE_STRING);
    expect(prefixed, styles, "U\"wide\"", SOLAR_OS_SYNTAX_STYLE_STRING);
    expect(prefixed, styles, "u'z'", SOLAR_OS_SYNTAX_STYLE_STRING);
    expect(prefixed, styles, "return_value", SOLAR_OS_SYNTAX_STYLE_NORMAL);
    expect(prefixed, styles, "iffy", SOLAR_OS_SYNTAX_STYLE_NORMAL);
    expect(prefixed, styles, "NULL", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    expect(prefixed, styles, "true", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
}

static void test_c_multiline(void)
{
    solar_os_syntax_state_t state;
    uint8_t styles[256];
    solar_os_syntax_state_init(&state);
    highlight(SOLAR_OS_SYNTAX_C, &state, "/* start", styles);
    assert(state.mode == SOLAR_OS_SYNTAX_MODE_C_COMMENT);
    highlight(SOLAR_OS_SYNTAX_C, &state, "", styles);
    assert(state.mode == SOLAR_OS_SYNTAX_MODE_C_COMMENT);
    const char *end = "comment */ return 42;";
    highlight(SOLAR_OS_SYNTAX_C, &state, end, styles);
    expect(end, styles, "comment */", SOLAR_OS_SYNTAX_STYLE_COMMENT);
    expect(end, styles, "return", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    expect(end, styles, "42", SOLAR_OS_SYNTAX_STYLE_NUMBER);
    assert(state.mode == SOLAR_OS_SYNTAX_MODE_NORMAL);

    highlight(SOLAR_OS_SYNTAX_C, &state, "// continued \134", styles);
    highlight(SOLAR_OS_SYNTAX_C, &state, "return 1;", styles);
    expect("return 1;", styles, "return 1;", SOLAR_OS_SYNTAX_STYLE_COMMENT);
    highlight(SOLAR_OS_SYNTAX_C, &state, "return 2;", styles);
    expect("return 2;", styles, "return", SOLAR_OS_SYNTAX_STYLE_KEYWORD);

    highlight(SOLAR_OS_SYNTAX_C, &state, "\"continued \134", styles);
    const char *quoted = "text\"; return 3;";
    highlight(SOLAR_OS_SYNTAX_C, &state, quoted, styles);
    expect(quoted, styles, "text\"", SOLAR_OS_SYNTAX_STYLE_STRING);
    expect(quoted, styles, "return", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    highlight(SOLAR_OS_SYNTAX_C, &state, "\"continued \134\134", styles);
    assert(state.c_escaped);
    const char *escaped = "\"escaped\"; return 5;";
    highlight(SOLAR_OS_SYNTAX_C, &state, escaped, styles);
    expect(escaped, styles, "\"escaped\"", SOLAR_OS_SYNTAX_STYLE_STRING);
    expect(escaped, styles, "return", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    highlight(SOLAR_OS_SYNTAX_C, &state, "'\134", styles);
    const char *character = "x'; int y;";
    highlight(SOLAR_OS_SYNTAX_C, &state, character, styles);
    expect(character, styles, "x'", SOLAR_OS_SYNTAX_STYLE_STRING);
    expect(character, styles, "int", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    highlight(SOLAR_OS_SYNTAX_C, &state, "\"unterminated", styles);
    highlight(SOLAR_OS_SYNTAX_C, &state, "return 4;", styles);
    expect("return 4;", styles, "return", SOLAR_OS_SYNTAX_STYLE_KEYWORD);

    highlight(SOLAR_OS_SYNTAX_C, &state, "# \134", styles);
    highlight(SOLAR_OS_SYNTAX_C, &state, "include \134", styles);
    expect("include \134", styles, "include", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    highlight(SOLAR_OS_SYNTAX_C, &state, " <stdint.h>", styles);
    expect(" <stdint.h>", styles, "<stdint.h>", SOLAR_OS_SYNTAX_STYLE_STRING);
    highlight(SOLAR_OS_SYNTAX_C, &state, "#include \\\r", styles);
    highlight(SOLAR_OS_SYNTAX_C, &state, "\"local.h\"\r", styles);
    expect("\"local.h\"\r", styles, "\"local.h\"", SOLAR_OS_SYNTAX_STYLE_STRING);
    highlight(SOLAR_OS_SYNTAX_C, &state, "#define VALUE \134", styles);
    const char *replacement = "# include <x>";
    highlight(SOLAR_OS_SYNTAX_C, &state, replacement, styles);
    expect(replacement, styles, "# include <x>", SOLAR_OS_SYNTAX_STYLE_NORMAL);
}

static void check_windows(solar_os_syntax_language_t language, solar_os_syntax_state_t *state,
                          const char *line, size_t len)
{
    uint8_t full[256];
    assert(len <= sizeof(full));
    solar_os_syntax_state_t before = *state;
    solar_os_syntax_highlight_line(language, state, line, len, 0, full, len);
    for (size_t offset = 0; offset <= len + 1; offset++) {
        uint8_t window[7];
        memset(window, 0xa5, sizeof(window));
        solar_os_syntax_state_t clipped = before;
        solar_os_syntax_highlight_line(language, &clipped, line, len, offset, window + 1, 5);
        assert(window[0] == 0xa5 && window[6] == 0xa5);
        for (size_t i = 0; i < 5; i++) {
            assert(window[i + 1] == (offset + i < len ? full[offset + i] : SOLAR_OS_SYNTAX_STYLE_NORMAL));
        }
        expect_same_state(state, &clipped);
    }
    solar_os_syntax_state_t hidden = before;
    solar_os_syntax_highlight_line(language, &hidden, line, len, 0, NULL, 0);
    expect_same_state(state, &hidden);
}

static void test_bounded_buffers_and_scrolling(void)
{
    solar_os_syntax_state_t state;
    solar_os_syntax_state_init(&state);
    /* Deliberately not NUL-terminated, as slices into the editor buffer aren't. */
    const char quote[] = {'"', 'a', '\\', '"', 'b', '"', ' ', '\'', '\\', '\'', '\''};
    uint8_t styles[sizeof(quote)];
    solar_os_syntax_highlight_line(SOLAR_OS_SYNTAX_C, &state, quote, sizeof(quote),
                                   0, styles, sizeof(styles));
    for (size_t i = 0; i < sizeof(quote); i++) {
        assert(styles[i] == (i == 6 ? SOLAR_OS_SYNTAX_STYLE_NORMAL : SOLAR_OS_SYNTAX_STYLE_STRING));
    }
    assert(state.mode == SOLAR_OS_SYNTAX_MODE_NORMAL);
    check_windows(SOLAR_OS_SYNTAX_C, &state, quote, sizeof(quote));
    const char block[] = {'/', '*', 'x', '*', '/', 'i', 'n', 't'};
    check_windows(SOLAR_OS_SYNTAX_C, &state, block, sizeof(block));
    const char slash[] = {'/'};
    check_windows(SOLAR_OS_SYNTAX_C, &state, slash, sizeof(slash));
    const char *lines[] = {
        "#include <stdio.h>", "int main(int argc, char **argv)", "{",
        "    puts(\"Hello from SolarOS C\");", "    return 0;", "}",
        "/* block", "", "comment */ static int n = 0x1UL;",
        "// continued \134", "int still_comment;", "int code;",
        "\"continued \134\134", "\"escaped quote\";", "#include \134", " <stdio.h>",
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        check_windows(SOLAR_OS_SYNTAX_C, &state, lines[i], strlen(lines[i]));
    }
}

static void test_other_languages(void)
{
    solar_os_syntax_state_t state;
    uint8_t styles[256];
    solar_os_syntax_state_init(&state);
    highlight(SOLAR_OS_SYNTAX_PYTHON, &state, "def hello(): # note", styles);
    expect("def hello(): # note", styles, "def", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    expect("def hello(): # note", styles, "# note", SOLAR_OS_SYNTAX_STYLE_COMMENT);
    const char *python[] = {"text = '''start", "still a string", "end''' # comment"};
    for (size_t i = 0; i < 3; i++) check_windows(SOLAR_OS_SYNTAX_PYTHON, &state, python[i], strlen(python[i]));
    solar_os_syntax_state_init(&state);
    highlight(SOLAR_OS_SYNTAX_LUA, &state, "local x = 42 -- note", styles);
    expect("local x = 42 -- note", styles, "local", SOLAR_OS_SYNTAX_STYLE_KEYWORD);
    expect("local x = 42 -- note", styles, "-- note", SOLAR_OS_SYNTAX_STYLE_COMMENT);
    const char *lua[] = {"--[=[comment", "still a comment", "]=] return 1"};
    for (size_t i = 0; i < 3; i++) check_windows(SOLAR_OS_SYNTAX_LUA, &state, lua[i], strlen(lua[i]));
}

int main(void)
{
    test_extensions();
    test_c_tokens();
    test_c_multiline();
    test_bounded_buffers_and_scrolling();
    test_other_languages();
    puts("Syntax highlighting: C tokens, continuations, bounded buffers, scrolling, Python and Lua passed");
    return 0;
}
