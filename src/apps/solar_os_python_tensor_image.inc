#if SOLAR_OS_PACKAGE_SERVICE_IMAGE
#include "solar_os_tensor_image.h"
static void python_tensor_image_vector(mp_obj_t dict, const char *key, double *values, unsigned channels)
{
    mp_obj_t value = python_get_dict_obj(dict, key, false);
    if (value == MP_OBJ_NULL) return;
    size_t count; mp_obj_t *items; mp_obj_get_array(value, &count, &items);
    if (count != 1 && count != channels) mp_raise_ValueError(MP_ERROR_TEXT("expected one value or one per channel"));
    for (unsigned c = 0; c < channels; ++c) {
        mp_obj_t item = items[count == 1 ? 0 : c];
        if (item == mp_const_true || item == mp_const_false) mp_raise_TypeError(MP_ERROR_TEXT("expected numeric channel value"));
        values[c] = mp_obj_get_float(item);
    }
}
static mp_obj_t solaros_inference_prepare_image(size_t n, const mp_obj_t *args)
{
    const solar_os_inference_model_info_t *info = python_inference_info(args[0]);
    const char *name = python_inference_name(args[1]);
    size_t index = 0;
    while (index < info->input_count && strcmp(name, info->inputs[index].name)) ++index;
    if (index == info->input_count) mp_raise_ValueError(MP_ERROR_TEXT("unknown input name"));
    solar_os_raster_image_pixels_t source;
    python_check_esp(solar_os_raster_image_pixels(
        python_app.images[python_image_slot_from_obj(args[2])], &source));
    solar_os_tensor_image_options_t options; solar_os_tensor_image_defaults(&options);
    if (n == 4 && args[3] != mp_const_none) {
        if (!mp_obj_is_type(args[3], &mp_type_dict)) mp_raise_TypeError(MP_ERROR_TEXT("expected image options dict"));
        mp_obj_dict_t *dict = MP_OBJ_TO_PTR(args[3]);
        for (size_t k = 0; k < dict->map.alloc; ++k) {
            if (!mp_map_slot_is_filled(&dict->map, k)) continue;
            const char *key = python_inference_name(dict->map.table[k].key);
            mp_obj_t value = dict->map.table[k].value;
            if (!strcmp(key,"layout") || !strcmp(key,"color") || !strcmp(key,"resize"))
                python_check_esp(solar_os_tensor_image_option(&options,key,python_inference_name(value)));
            else if (!strcmp(key,"x")) options.x = python_u32_from_obj(value);
            else if (!strcmp(key,"y")) options.y = python_u32_from_obj(value);
            else if (!strcmp(key,"width")) options.width = python_u32_from_obj(value);
            else if (!strcmp(key,"height")) options.height = python_u32_from_obj(value);
            else if (strcmp(key,"mean") && strcmp(key,"std") && strcmp(key,"pad"))
                mp_raise_ValueError(MP_ERROR_TEXT("unknown image option"));
        }
        const unsigned channels = options.color == SOLAR_OS_TENSOR_GRAY ? 1 : 3;
        python_tensor_image_vector(args[3],"mean",options.mean,channels);
        python_tensor_image_vector(args[3],"std",options.std,channels);
        double pad[3] = {0}; python_tensor_image_vector(args[3],"pad",pad,channels);
        for (unsigned c = 0; c < channels; ++c) {
            if (!isfinite(pad[c]) || pad[c] < 0 || pad[c] > 255 || floor(pad[c]) != pad[c])
                mp_raise_ValueError(MP_ERROR_TEXT("pad values must be integers 0..255"));
            options.pad[c] = pad[c];
        }
    }
    solar_os_tensor_image_transform_t transform;
    python_check_esp(solar_os_tensor_image_inspect(&source,&info->inputs[index],&options,&transform));
    mp_obj_t data = mp_call_function_1(MP_OBJ_FROM_PTR(&mp_type_bytearray),mp_obj_new_int_from_uint(transform.bytes));
    mp_buffer_info_t buffer; mp_get_buffer_raise(data,&buffer,MP_BUFFER_WRITE);
    python_check_esp(solar_os_tensor_image_prepare(&source,&info->inputs[index],&options,
        buffer.buf,buffer.len,&transform,python_should_cancel,NULL));
    mp_obj_t mapping = mp_obj_new_dict(0), result = mp_obj_new_dict(0);
#define PYTHON_TENSOR_IMAGE_FIELD(field) python_dict_store_uint(mapping,#field,transform.field)
    PYTHON_TENSOR_IMAGE_FIELD(source_width); PYTHON_TENSOR_IMAGE_FIELD(source_height);
    PYTHON_TENSOR_IMAGE_FIELD(input_width); PYTHON_TENSOR_IMAGE_FIELD(input_height);
    PYTHON_TENSOR_IMAGE_FIELD(crop_x); PYTHON_TENSOR_IMAGE_FIELD(crop_y);
    PYTHON_TENSOR_IMAGE_FIELD(crop_width); PYTHON_TENSOR_IMAGE_FIELD(crop_height);
    PYTHON_TENSOR_IMAGE_FIELD(resized_width); PYTHON_TENSOR_IMAGE_FIELD(resized_height);
    PYTHON_TENSOR_IMAGE_FIELD(pad_left); PYTHON_TENSOR_IMAGE_FIELD(pad_top);
#undef PYTHON_TENSOR_IMAGE_FIELD
    mp_obj_dict_store(result,MP_OBJ_NEW_QSTR(qstr_from_str("data")),data);
    mp_obj_dict_store(result,MP_OBJ_NEW_QSTR(qstr_from_str("transform")),mapping);
    python_dict_store_u64(result,"preprocess_us",transform.preprocess_us);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_inference_prepare_image_obj,3,4,solaros_inference_prepare_image);
#endif
