static int solua_image_to_rgb(lua_State *L)
{
    const int count = lua_gettop(L);
    if (count != 1 && count != 3) {
        return luaL_error(L, "expected handle [, width, height]");
    }
    const solar_os_raster_image_t *image = solua.images[solua_image_slot(L, 1)];
    const uint32_t width = count == 3 ? solua_check_u16_size(L, 2) :
        solar_os_raster_image_width(image);
    const uint32_t height = count == 3 ? solua_check_u16_size(L, 3) :
        solar_os_raster_image_height(image);
    if (!width || !height || (uint64_t)width * height > 2U * 1024U * 1024U) {
        return luaL_error(L, "image dimensions out of range");
    }
    const size_t bytes = (size_t)width * height * 3U;
    luaL_Buffer buffer;
    char *data = luaL_buffinitsize(L, &buffer, bytes);
    const solar_os_raster_image_convert_options_t options = {
        .output_width = width, .output_height = height,
        .format = SOLAR_OS_RASTER_IMAGE_RGB888,
    };
    (void)solua_check_esp(L, solar_os_raster_image_convert(image, &options,
        (uint8_t *)data, bytes, 0));
    luaL_pushresultsize(&buffer, bytes);
    return 1;
}
