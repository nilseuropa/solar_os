#include "lua_runtime.h"

#include <limits.h>
#include <string.h>

static solar_os_gfx_t *surface(lua_State *L)
{
    if (!lua_runtime.surface) luaL_error(L, "call gfx.begin() before drawing");
    return lua_runtime.surface;
}

static int coordinate(lua_State *L, int index)
{
    return (int)solar_os_lua_integer(L, index, INT16_MIN, INT16_MAX, 0);
}

static int extent(lua_State *L, int index)
{
    return (int)solar_os_lua_integer(L, index, 0, INT16_MAX, 0);
}

static solar_os_gfx_color_t color(lua_State *L, int index, solar_os_gfx_color_t fallback)
{
    const solar_os_gfx_color_t value = (solar_os_gfx_color_t)
        solar_os_lua_integer(L, index, 0, UINT32_MAX, fallback);
    if (!lua_runtime.graphics->color_is_valid(value)) luaL_argerror(L, index, "invalid graphics color");
    return value;
}

static int gfx_begin(lua_State *L)
{
    if (lua_runtime.scene) return luaL_error(L, "close the Jet scene before drawing graphics");
    if (lua_runtime.surface) return 0;
    const char *target = luaL_optstring(L, 1, "");
    int result = lua_runtime.foreground->console_gfx_acquire(&lua_runtime.surface);
    if (result) return solar_os_lua_check(L, result);
    if (*target) {
        char name[32];
        if (!lua_runtime.graphics->display_target_name(lua_runtime.surface, name, sizeof(name)) ||
            strcmp(name, target)) {
            (void)lua_runtime.foreground->console_gfx_release();
            lua_runtime.surface = NULL;
            return luaL_argerror(L, 1, "expected the foreground display name");
        }
    }
    return 0;
}

static int gfx_end(lua_State *L)
{
    if (!lua_runtime.surface) return 0;
    const int result = lua_runtime.foreground->console_gfx_release();
    if (!result) lua_runtime.surface = NULL;
    return solar_os_lua_check(L, result);
}

static int gfx_size(lua_State *L)
{
    uint16_t width = 0, height = 0;
    (void)lua_runtime.foreground->video_dimensions(&width, &height);
    lua_pushinteger(L, width);
    lua_pushinteger(L, height);
    return 2;
}

static int gfx_width(lua_State *L)
{
    gfx_size(L);
    lua_pop(L, 1);
    return 1;
}

static int gfx_height(lua_State *L)
{
    gfx_size(L);
    return 1;
}

static int gfx_clear(lua_State *L)
{
    lua_runtime.graphics->clear(surface(L), color(L, 1, SOLAR_OS_GFX_COLOR_WHITE));
    return 0;
}

static int gfx_color(lua_State *L)
{
    solar_os_gfx_t *gfx = surface(L);
    if (lua_isnoneornil(L, 1)) {
        lua_pushinteger(L, lua_runtime.graphics->color(gfx));
        return 1;
    }
    lua_runtime.graphics->set_color(gfx, color(L, 1, SOLAR_OS_GFX_COLOR_BLACK));
    return 0;
}

static int gfx_font(lua_State *L)
{
    solar_os_gfx_t *gfx = surface(L);
    if (lua_isnoneornil(L, 1)) {
        lua_pushinteger(L, lua_runtime.graphics->font(gfx));
        return 1;
    }
    lua_runtime.graphics->set_font(gfx, (solar_os_gfx_font_t)
        solar_os_lua_integer(L, 1, 0, SOLAR_OS_GFX_FONT_COUNT - 1, SOLAR_OS_GFX_FONT_MONO));
    return 0;
}

static int gfx_theme(lua_State *L)
{
    solar_os_display_theme_v1_t theme;
    solar_os_lua_check(L, lua_runtime.display->get_theme(&theme));
    lua_newtable(L);
    solar_os_lua_set_number(L, "foreground", theme.foreground_rgb888);
    solar_os_lua_set_number(L, "background", theme.background_rgb888);
    return 1;
}

static int gfx_getch(lua_State *L)
{
    const int key = solar_os_lua_read_key((uint32_t)solar_os_lua_integer(L, 1, 0, UINT32_MAX, 0));
    if (key < 0) lua_pushnil(L);
    else lua_pushinteger(L, key);
    return 1;
}

static int gfx_present(lua_State *L)
{
    lua_runtime.graphics->present(surface(L));
    return 0;
}

static int gfx_pixel(lua_State *L)
{
    lua_runtime.graphics->pixel(surface(L), coordinate(L, 1), coordinate(L, 2));
    return 0;
}

static int gfx_line(lua_State *L)
{
    lua_runtime.graphics->line(surface(L), coordinate(L, 1), coordinate(L, 2),
                               coordinate(L, 3), coordinate(L, 4));
    return 0;
}

static int gfx_rect(lua_State *L)
{
    lua_runtime.graphics->rect(surface(L), coordinate(L, 1), coordinate(L, 2),
                               extent(L, 3), extent(L, 4));
    return 0;
}

static int gfx_fill_rect(lua_State *L)
{
    lua_runtime.graphics->fill_rect(surface(L), coordinate(L, 1), coordinate(L, 2),
                                    extent(L, 3), extent(L, 4));
    return 0;
}

static int gfx_circle(lua_State *L)
{
    lua_runtime.graphics->circle(surface(L), coordinate(L, 1), coordinate(L, 2), extent(L, 3));
    return 0;
}

static int gfx_text(lua_State *L)
{
    lua_runtime.graphics->text(surface(L), coordinate(L, 1), coordinate(L, 2), luaL_checkstring(L, 3));
    return 0;
}

static int gfx_bitmap(lua_State *L)
{
    const int width = extent(L, 3), height = extent(L, 4);
    size_t length;
    const char *data = luaL_checklstring(L, 5, &length);
    if (width < 1 || height < 1 || length != (((size_t)width + 7U) / 8U) * height)
        return luaL_argerror(L, 5, "wrong size for packed monochrome bitmap");
    lua_runtime.graphics->bitmap(surface(L), coordinate(L, 1), coordinate(L, 2),
                                 width, height, (const uint8_t *)data);
    return 0;
}

void solar_os_lua_graphics(lua_State *L)
{
    if (!lua_runtime.graphics) return;
    static const luaL_Reg methods[] = {
        {"begin", gfx_begin}, {"end", gfx_end}, {"size", gfx_size}, {"width", gfx_width},
        {"height", gfx_height}, {"getch", gfx_getch}, {"clear", gfx_clear}, {"color", gfx_color},
        {"font", gfx_font}, {"present", gfx_present}, {"refresh", gfx_present}, {"pixel", gfx_pixel},
        {"line", gfx_line}, {"rect", gfx_rect}, {"fill_rect", gfx_fill_rect},
        {"circle", gfx_circle}, {"text", gfx_text}, {"bitmap", gfx_bitmap}, {"sprite", gfx_bitmap},
        {NULL, NULL},
    };
    luaL_newlib(L, methods);
    if (lua_runtime.display) {
        lua_pushcfunction(L, gfx_theme);
        lua_setfield(L, -2, "theme");
    }
    solar_os_lua_set_number(L, "WHITE", SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_lua_set_number(L, "LIGHT", SOLAR_OS_GFX_COLOR_LIGHT);
    solar_os_lua_set_number(L, "DARK", SOLAR_OS_GFX_COLOR_DARK);
    solar_os_lua_set_number(L, "BLACK", SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_lua_set_number(L, "FONT_MONO_12", SOLAR_OS_GFX_FONT_MONO_12);
    solar_os_lua_set_number(L, "FONT_BOLD_14", SOLAR_OS_GFX_FONT_BOLD_14);
    solar_os_lua_set_number(L, "KEY_ESCAPE", SOLAR_OS_KEY_ESCAPE);
    solar_os_lua_set_number(L, "KEY_UP", SOLAR_OS_KEY_UP);
    solar_os_lua_set_number(L, "KEY_DOWN", SOLAR_OS_KEY_DOWN);
    solar_os_lua_set_number(L, "KEY_LEFT", SOLAR_OS_KEY_LEFT);
    solar_os_lua_set_number(L, "KEY_RIGHT", SOLAR_OS_KEY_RIGHT);
    solar_os_lua_set_number(L, "KEY_PAGE_UP", SOLAR_OS_KEY_PAGE_UP);
    solar_os_lua_set_number(L, "KEY_PAGE_DOWN", SOLAR_OS_KEY_PAGE_DOWN);
    solar_os_lua_set_number(L, "KEY_DELETE", SOLAR_OS_KEY_DELETE);
    lua_setfield(L, -2, "gfx");
}
