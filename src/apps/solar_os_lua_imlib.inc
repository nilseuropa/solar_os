#if SOLAR_OS_PACKAGE_SERVICE_IMLIB
#include "solar_os_imlib.h"
static bool solua_imlib_inflight;
static bool solua_imlib_active(void)
{ return __atomic_load_n(&solua_imlib_inflight, __ATOMIC_ACQUIRE); }
static uint32_t solua_imlib_uint(lua_State *L, int options, const char *key, uint32_t fallback)
{
    lua_getfield(L, options, key);
    lua_Integer n = lua_isnil(L, -1) ? fallback : luaL_checkinteger(L, -1);
    if (n < 0 || (uint64_t)n > UINT32_MAX) luaL_error(L, "vision integer out of range");
    lua_pop(L, 1); return n;
}
static bool solua_imlib_bool(lua_State *L, int options, const char *key, bool fallback)
{
    lua_getfield(L, options, key);
    bool value = fallback;
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN); value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1); return value;
}
static solar_os_imlib_options_t solua_imlib_options(lua_State *L, int options,
    solar_os_imlib_operation_t op)
{
    solar_os_imlib_options_t o = solar_os_imlib_default_options();
    if (lua_isnoneornil(L, options)) return o;
    options = lua_absindex(L, options); luaL_checktype(L, options, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, options)) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "vision option keys must be strings");
        size_t n; const char *key = lua_tolstring(L, -2, &n);
        if (memchr(key, 0, n) || !solar_os_imlib_option_allowed(op, key)) luaL_error(L, "unsupported vision option");
        lua_pop(L, 1);
    }
    lua_getfield(L, options, "format");
    if (!lua_isnil(L, -1)) {
        size_t n; const char *text = luaL_checklstring(L, -1, &n);
        if (n == 4 && !memcmp(text, "gray", 4)) o.image.format = SOLAR_OS_RASTER_IMAGE_GRAY8;
        else if (n == 6 && !memcmp(text, "rgb565", 6)) o.image.format = SOLAR_OS_RASTER_IMAGE_RGB565_LE;
        else luaL_error(L, "format must be gray or rgb565");
    }
    lua_pop(L, 1);
    o.image.x = solua_imlib_uint(L, options, "x", o.image.x);
    o.image.y = solua_imlib_uint(L, options, "y", o.image.y);
    o.image.width = solua_imlib_uint(L, options, "width", o.image.width);
    o.image.height = solua_imlib_uint(L, options, "height", o.image.height);
    o.image.output_width = solua_imlib_uint(L, options, "output_width", o.image.output_width);
    o.image.output_height = solua_imlib_uint(L, options, "output_height", o.image.output_height);
    o.timeout_ms = solua_imlib_uint(L, options, "timeout_ms", o.timeout_ms);
    o.ksize = solua_imlib_uint(L, options, "ksize", o.ksize);
    o.bins = solua_imlib_uint(L, options, "bins", o.bins);
    o.x_stride = solua_imlib_uint(L, options, "x_stride", o.x_stride);
    o.y_stride = solua_imlib_uint(L, options, "y_stride", o.y_stride);
    o.area_threshold = solua_imlib_uint(L, options, "area_threshold", o.area_threshold);
    o.pixels_threshold = solua_imlib_uint(L, options, "pixels_threshold", o.pixels_threshold);
    o.margin = solua_imlib_uint(L, options, "margin", o.margin);
    o.max_blobs = solua_imlib_uint(L, options, "max_blobs", o.max_blobs);
    o.invert = solua_imlib_bool(L, options, "invert", o.invert);
    o.merge = solua_imlib_bool(L, options, "merge", o.merge);
    lua_getfield(L, options, "thresholds");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE); size_t count = lua_rawlen(L, -1);
        if (count > SOLAR_OS_IMLIB_THRESHOLDS_MAX) luaL_error(L, "too many thresholds");
        o.threshold_count = count;
        for (size_t i = 0; i < count; ++i) {
            lua_rawgeti(L, -1, i+1); luaL_checktype(L, -1, LUA_TTABLE);
            size_t n = lua_rawlen(L, -1);
            if (n != (o.image.format == SOLAR_OS_RASTER_IMAGE_GRAY8 ? 2U : 6U))
                luaL_error(L, "expected gray pair or LAB six limits");
            int16_t v[6] = {0};
            for (size_t j = 0; j < n; ++j) {
                lua_rawgeti(L, -1, j+1); lua_Integer x = luaL_checkinteger(L, -1);
                if (x < -128 || x > 255) luaL_error(L, "threshold out of range");
                v[j] = x; lua_pop(L, 1);
            }
            o.thresholds[i] = (solar_os_imlib_threshold_t){v[0],v[1],v[2],v[3],v[4],v[5]};
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1); return o;
}
static void solua_imlib_number(lua_State *L, int table, const char *name, lua_Number value)
{ table = lua_absindex(L, table); lua_pushnumber(L, value); lua_setfield(L, table, name); }
typedef struct { solar_os_imlib_result_t *result; size_t slot; bool record; } solua_imlib_render_t;
static int solua_imlib_result(lua_State *L)
{
    solua_imlib_render_t *render = lua_touserdata(L, 1);
    const solar_os_imlib_result_t *r = render->result;
    if (r->image && !render->record) { lua_pushinteger(L, render->slot+1); return 1; }
    lua_newtable(L); int result = lua_gettop(L);
    solua_set_str(L, result, "operation", solar_os_imlib_operation_name(r->operation));
    solua_set_str(L, result, "format", r->image_options.format == SOLAR_OS_RASTER_IMAGE_GRAY8 ? "gray" : "rgb565");
    solua_set_int(L, result, "processed_width", r->image_options.output_width);
    solua_set_int(L, result, "processed_height", r->image_options.output_height);
    solua_set_int(L, result, "preprocess_us", r->preprocess_us);
    solua_set_int(L, result, "process_us", r->process_us);
    solua_set_int(L, result, "output_us", r->output_us);
    solua_set_int(L, result, "elapsed_us", r->elapsed_us);
    solua_set_int(L, result, "workspace_peak_bytes", r->workspace_peak_bytes);
    lua_newtable(L);
    uint32_t roi[4] = {r->image_options.x,r->image_options.y,r->image_options.width,r->image_options.height};
    for (unsigned i = 0; i < 4; ++i) { lua_pushinteger(L, roi[i]); lua_rawseti(L, -2, i+1); }
    lua_setfield(L, result, "roi");
    if (r->image) solua_set_int(L, result, "image", render->slot+1);
    if (r->operation <= SOLAR_OS_IMLIB_STATISTICS) {
        lua_newtable(L); int channels = lua_gettop(L);
        for (unsigned i = 0; i < r->channels; ++i) {
            const char *name = r->channels == 1 ? "gray" : i == 0 ? "l" : i == 1 ? "a" : "b";
            lua_newtable(L);
            if (r->operation == SOLAR_OS_IMLIB_HISTOGRAM) {
                for (unsigned j = 0; j < r->bins[i]; ++j) { lua_pushnumber(L, r->histogram[i][j]); lua_rawseti(L, -2, j+1); }
            } else {
                const solar_os_imlib_statistics_t *s = &r->statistics[i];
                solua_set_int(L, -1, "mean", s->mean); solua_set_int(L, -1, "median", s->median);
                solua_set_int(L, -1, "mode", s->mode); solua_set_int(L, -1, "stdev", s->stdev);
                solua_set_int(L, -1, "min", s->min); solua_set_int(L, -1, "max", s->max);
                solua_set_int(L, -1, "lower_quartile", s->lower_quartile);
                solua_set_int(L, -1, "upper_quartile", s->upper_quartile);
            }
            lua_setfield(L, channels, name);
        }
        lua_setfield(L, result, "channels");
    }
    if (r->operation == SOLAR_OS_IMLIB_BLOBS) {
        lua_newtable(L);
        for (size_t i = 0; i < r->count; ++i) {
            const solar_os_imlib_blob_t *b = &r->blobs[i]; lua_newtable(L);
            solua_set_int(L, -1, "x", b->x); solua_set_int(L, -1, "y", b->y);
            solua_set_int(L, -1, "width", b->width); solua_set_int(L, -1, "height", b->height);
            solua_set_int(L, -1, "pixels", b->pixels); solua_set_int(L, -1, "code", b->code);
            solua_set_int(L, -1, "count", b->count);
            solua_imlib_number(L, -1, "cx", b->cx); solua_imlib_number(L, -1, "cy", b->cy);
            solua_imlib_number(L, -1, "rotation", b->rotation); lua_rawseti(L, -2, i+1);
        }
        lua_setfield(L, result, "blobs"); solua_set_bool(L, result, "truncated", r->truncated);
        solua_set_str(L, result, "coordinates", "source_pixels");
    }
    return 1;
}
static int solua_imlib_call(lua_State *L, int source, int reference, int options,
    solar_os_imlib_operation_t op, bool record)
{
    luaL_checkstack(L, 8, "vision result");
    solar_os_raster_image_t *image = solua.images[solua_image_slot(L, source)];
    solar_os_raster_image_t *other = !reference || lua_isnoneornil(L, reference) ? NULL :
        solua.images[solua_image_slot(L, reference)];
    solar_os_imlib_options_t o = solua_imlib_options(L, options, op);
    size_t slot = SOLUA_RASTER_IMAGE_MAX;
    if (solar_os_imlib_is_transform(op)) {
        for (size_t i = 0; i < SOLUA_RASTER_IMAGE_MAX; ++i) if (!solua.images[i]) { slot = i; break; }
        if (slot == SOLUA_RASTER_IMAGE_MAX) return luaL_error(L, "too many open images");
    }
    solar_os_imlib_result_t *r = NULL;
    __atomic_store_n(&solua_imlib_inflight, true, __ATOMIC_RELEASE);
    esp_err_t error = solar_os_imlib_run(image, other, op, &o, solua_should_cancel, NULL, &r);
    if (error) {
        __atomic_store_n(&solua_imlib_inflight, false, __ATOMIC_RELEASE);
        return solua_check_esp(L, error);
    }
    solua_imlib_render_t render = {.result = r, .slot = slot, .record = record};
    lua_pushcfunction(L, solua_imlib_result); lua_pushlightuserdata(L, &render);
    int status = lua_pcall(L, 1, 1, 0);
    if (status == LUA_OK && r->image) { solua.images[slot] = r->image; r->image = NULL; }
    solar_os_imlib_result_free(r);
    __atomic_store_n(&solua_imlib_inflight, false, __ATOMIC_RELEASE);
    if (status != LUA_OK) return lua_error(L);
    return 1;
}
static int solua_vision_process(lua_State *L)
{
    if (lua_gettop(L) < 2 || lua_gettop(L) > 4) return luaL_error(L, "expected image, operation [, options [, reference]]");
    size_t length; const char *name = luaL_checklstring(L, 2, &length);
    if (memchr(name, 0, length)) return luaL_error(L, "invalid operation");
    for (unsigned i = 0; i < SOLAR_OS_IMLIB_OPERATIONS_COUNT; ++i)
        if (!strcmp(name, solar_os_imlib_operation_name(i))) return solua_imlib_call(L, 1, 4, 3, i, true);
    return luaL_error(L, "unknown vision operation");
}
#define SOLUA_IMLIB_FUNCTION(name, op) \
    static int solua_vision_##name(lua_State *L) \
    { if (lua_gettop(L) < 1 || lua_gettop(L) > 2) return luaL_error(L, "expected image [, options]"); \
      return solua_imlib_call(L, 1, 0, 2, op, false); }
SOLUA_IMLIB_FUNCTION(histogram, SOLAR_OS_IMLIB_HISTOGRAM)
SOLUA_IMLIB_FUNCTION(statistics, SOLAR_OS_IMLIB_STATISTICS)
SOLUA_IMLIB_FUNCTION(binary, SOLAR_OS_IMLIB_BINARY)
SOLUA_IMLIB_FUNCTION(invert, SOLAR_OS_IMLIB_INVERT)
SOLUA_IMLIB_FUNCTION(mean, SOLAR_OS_IMLIB_MEAN)
SOLUA_IMLIB_FUNCTION(gaussian, SOLAR_OS_IMLIB_GAUSSIAN)
SOLUA_IMLIB_FUNCTION(median, SOLAR_OS_IMLIB_MEDIAN)
SOLUA_IMLIB_FUNCTION(erode, SOLAR_OS_IMLIB_ERODE)
SOLUA_IMLIB_FUNCTION(dilate, SOLAR_OS_IMLIB_DILATE)
SOLUA_IMLIB_FUNCTION(opening, SOLAR_OS_IMLIB_OPENING)
SOLUA_IMLIB_FUNCTION(closing, SOLAR_OS_IMLIB_CLOSING)
SOLUA_IMLIB_FUNCTION(blobs, SOLAR_OS_IMLIB_BLOBS)
#undef SOLUA_IMLIB_FUNCTION
static int solua_vision_difference(lua_State *L)
{
    if (lua_gettop(L) < 2 || lua_gettop(L) > 3) return luaL_error(L, "expected image, reference [, options]");
    return solua_imlib_call(L, 1, 2, 3, SOLAR_OS_IMLIB_DIFFERENCE, false);
}
#else
static bool solua_imlib_active(void) { return false; }
#endif
