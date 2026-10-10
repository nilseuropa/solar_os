/* Copy pixels into interpreter-owned storage; native image ownership never
 * escapes through a borrowed buffer. Resize before copying large pictures. */
static mp_obj_t solaros_image_to_rgb(size_t n_args, const mp_obj_t *args)
{
    if (n_args == 2U) {
        mp_raise_ValueError(MP_ERROR_TEXT("width and height must be supplied together"));
    }
    const solar_os_raster_image_t *image =
        python_app.images[python_image_slot_from_obj(args[0])];
    const uint32_t width = n_args == 3U ?
        python_u16_from_size(python_size_from_obj(args[1])) : solar_os_raster_image_width(image);
    const uint32_t height = n_args == 3U ?
        python_u16_from_size(python_size_from_obj(args[2])) : solar_os_raster_image_height(image);
    if (!width || !height || (uint64_t)width * height > 2U * 1024U * 1024U) {
        mp_raise_ValueError(MP_ERROR_TEXT("image dimensions out of range"));
    }
    const size_t bytes = (size_t)width * height * 3U;
    /* mp_obj_new_bytearray copies its source unconditionally. Use the public
     * bytearray(length) constructor for GC-owned, writable storage instead. */
    mp_obj_t result = mp_call_function_1(MP_OBJ_FROM_PTR(&mp_type_bytearray),
        mp_obj_new_int_from_uint(bytes));
    mp_buffer_info_t buffer;
    mp_get_buffer_raise(result, &buffer, MP_BUFFER_WRITE);
    const solar_os_raster_image_convert_options_t options = {
        .output_width = width, .output_height = height,
        .format = SOLAR_OS_RASTER_IMAGE_RGB888,
    };
    python_check_esp(solar_os_raster_image_convert(image, &options,
        buffer.buf, buffer.len, 0));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_image_to_rgb_obj, 1, 3, solaros_image_to_rgb);
