#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SOLAR_OS_SYNTAX_NONE,
    SOLAR_OS_SYNTAX_PYTHON,
    SOLAR_OS_SYNTAX_LUA,
    SOLAR_OS_SYNTAX_C,
} solar_os_syntax_language_t;

typedef enum {
    SOLAR_OS_SYNTAX_STYLE_NORMAL,
    SOLAR_OS_SYNTAX_STYLE_KEYWORD,
    SOLAR_OS_SYNTAX_STYLE_COMMENT,
    SOLAR_OS_SYNTAX_STYLE_STRING,
    SOLAR_OS_SYNTAX_STYLE_NUMBER,
} solar_os_syntax_style_t;

typedef enum {
    SOLAR_OS_SYNTAX_MODE_NORMAL,
    SOLAR_OS_SYNTAX_MODE_PY_TRIPLE_SINGLE,
    SOLAR_OS_SYNTAX_MODE_PY_TRIPLE_DOUBLE,
    SOLAR_OS_SYNTAX_MODE_LUA_LONG_STRING,
    SOLAR_OS_SYNTAX_MODE_LUA_LONG_COMMENT,
    SOLAR_OS_SYNTAX_MODE_C_COMMENT,
    SOLAR_OS_SYNTAX_MODE_C_LINE_COMMENT,
    SOLAR_OS_SYNTAX_MODE_C_STRING,
    SOLAR_OS_SYNTAX_MODE_C_CHARACTER,
} solar_os_syntax_mode_t;

typedef struct {
    solar_os_syntax_mode_t mode;
    uint8_t lua_long_equals;
    bool c_escaped;
    bool c_continued;
    bool c_directive;
    bool c_include;
} solar_os_syntax_state_t;

solar_os_syntax_language_t solar_os_syntax_language_for_path(const char *path);
void solar_os_syntax_state_init(solar_os_syntax_state_t *state);
void solar_os_syntax_highlight_line(solar_os_syntax_language_t language,
                                    solar_os_syntax_state_t *state,
                                    const char *line,
                                    size_t line_len,
                                    size_t visible_offset,
                                    uint8_t *styles,
                                    size_t visible_len);
