#pragma once

#include <stddef.h>

void solar_os_lua_write(const char *text, size_t length);
void solar_os_lua_warning(const char *format, const char *text);

#define lua_writestring(text, length) solar_os_lua_write((text), (length))
#define lua_writeline() solar_os_lua_write("\n", 1)
#define lua_writestringerror(format, text) solar_os_lua_warning((format), (text))
#define lua_getlocaledecpoint() '.'
