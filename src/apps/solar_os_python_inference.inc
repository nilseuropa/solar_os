#if SOLAR_OS_PACKAGE_SERVICE_INFERENCE
#include "solar_os_inference.h"
#include "solar_os_model_bundle.h"
static solar_os_inference_t *python_inference_session;
static bool python_inference_active(void)
{
    return __atomic_load_n(&python_inference_session, __ATOMIC_ACQUIRE) != NULL;
}
static void python_inference_destroy(void)
{
    solar_os_inference_destroy(python_inference_session);
    __atomic_store_n(&python_inference_session, NULL, __ATOMIC_RELEASE);
}
static solar_os_inference_t *python_inference_get(void)
{
    if (!python_inference_session) {
        solar_os_inference_t *created = NULL;
        python_check_esp(solar_os_inference_create(python_should_cancel, NULL, &created));
        __atomic_store_n(&python_inference_session, created, __ATOMIC_RELEASE);
    }
    return python_inference_session;
}
static uint32_t python_inference_timeout(size_t n, const mp_obj_t *args, size_t index)
{
    return n > index ? python_u32_from_obj(args[index]) : 10000U;
}
static const char *python_inference_name(mp_obj_t value)
{
    size_t length;
    const char *name = (const char *)mp_obj_str_get_data(value, &length);
    if (memchr(name, 0, length)) mp_raise_ValueError(MP_ERROR_TEXT("embedded NUL in tensor name/type"));
    return mp_obj_str_get_str(value);
}
static mp_obj_t python_inference_tensor(const solar_os_inference_tensor_t *t)
{
    mp_obj_t d = mp_obj_new_dict(0);
    python_dict_store_cstr(d, "dtype", solar_os_tensor_dtype_name(t->dtype));
    python_dict_store_uint(d, "bytes", t->bytes);
    mp_obj_t shape = mp_obj_new_list(0, NULL), exponents = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < t->rank; ++i) mp_obj_list_append(shape, mp_obj_new_int_from_uint(t->shape[i]));
    for (size_t i = 0; i < t->exponent_count; ++i) mp_obj_list_append(exponents, mp_obj_new_int(t->exponents[i]));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("shape")), shape);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("exponents")), exponents);
    return d;
}
static mp_obj_t python_inference_ports(const solar_os_inference_tensor_t *ports, size_t count)
{
    mp_obj_t d = mp_obj_new_dict(0);
    for (size_t i = 0; i < count; ++i)
        mp_obj_dict_store(d, mp_obj_new_str(ports[i].name, strlen(ports[i].name)), python_inference_tensor(&ports[i]));
    return d;
}
static const solar_os_inference_model_info_t *python_inference_info(mp_obj_t handle)
{
    const solar_os_inference_model_info_t *info = NULL;
    python_check_esp(solar_os_inference_info(python_inference_get(), python_u32_from_obj(handle), &info));
    return info;
}
static mp_obj_t solaros_inference_load(size_t n, const mp_obj_t *args)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    python_resolve_path_obj(args[0], path, sizeof(path));
    uint32_t handle = 0;
    python_check_esp(solar_os_inference_load(python_inference_get(), path,
        python_inference_timeout(n, args, 1), &handle));
    return mp_obj_new_int_from_uint(handle);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_inference_load_obj, 1, 2, solaros_inference_load);
static mp_obj_t solaros_inference_load_bundle(size_t n, const mp_obj_t *args)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX]; python_resolve_path_obj(args[0], path, sizeof(path));
    uint32_t handle;
    python_check_esp(solar_os_inference_load_bundle(python_inference_get(), path,
        python_inference_timeout(n, args, 1), &handle));
    return mp_obj_new_int_from_uint(handle);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_inference_load_bundle_obj, 1, 2, solaros_inference_load_bundle);
static mp_obj_t solaros_inference_inputs(mp_obj_t handle)
{
    const solar_os_inference_model_info_t *i = python_inference_info(handle);
    return python_inference_ports(i->inputs, i->input_count);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_inference_inputs_obj, solaros_inference_inputs);
static mp_obj_t solaros_inference_outputs(mp_obj_t handle)
{
    const solar_os_inference_model_info_t *i = python_inference_info(handle);
    return python_inference_ports(i->outputs, i->output_count);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_inference_outputs_obj, solaros_inference_outputs);
static mp_obj_t solaros_inference_info(mp_obj_t handle)
{
    const solar_os_inference_model_info_t *i = python_inference_info(handle);
    mp_obj_t d = mp_obj_new_dict(0);
    python_dict_store_cstr(d, "backend", "espdl");
    python_dict_store_cstr(d, "backend_version", SOLAR_OS_INFERENCE_BACKEND_VERSION);
    python_dict_store_cstr(d, "target", "esp32s3");
    python_dict_store_cstr(d, "mode", solar_os_inference_mode_name(i->mode));
    python_dict_store_uint(d, "references", i->references);
    python_dict_store_cstr(d, "path", i->path);
    python_dict_store_cstr(d, "bundle_id", i->bundle_id);
    python_dict_store_cstr(d, "bundle_version", i->bundle_version);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("bundle")), mp_obj_new_bool(i->bundle));
    mp_obj_t image_inputs = mp_obj_new_dict(0);
    for (size_t k = 0; k < i->input_count; ++k)
        mp_obj_dict_store(image_inputs, mp_obj_new_str(i->inputs[k].name, strlen(i->inputs[k].name)), mp_obj_new_bool(i->image_inputs[k]));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("image_inputs")), image_inputs);
    python_dict_store_uint(d, "model_bytes", i->model_bytes);
    python_dict_store_uint(d, "internal_bytes", i->internal_bytes);
    python_dict_store_uint(d, "external_bytes", i->external_bytes);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("inputs")), python_inference_ports(i->inputs, i->input_count));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("outputs")), python_inference_ports(i->outputs, i->output_count));
    return d;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_inference_info_obj, solaros_inference_info);
static mp_obj_t python_inference_json(const char *json)
{
    mp_obj_t module = mp_import_name(qstr_from_str("json"), mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    return mp_call_function_1(mp_load_attr(module, qstr_from_str("loads")), mp_obj_new_str(json, strlen(json)));
}
static mp_obj_t python_inference_result(const solar_os_inference_result_t *r)
{
    mp_obj_t d = mp_obj_new_dict(0), outputs = mp_obj_new_dict(0);
    python_dict_store_u64(d, "input_us", r->input_us);
    python_dict_store_u64(d, "inference_us", r->inference_us);
    python_dict_store_u64(d, "output_us", r->output_us);
    python_dict_store_u64(d, "elapsed_us", r->elapsed_us);
    for (size_t i = 0; i < r->count; ++i) {
        const solar_os_inference_tensor_t *t = &r->outputs[i].tensor;
        mp_obj_t tensor = python_inference_tensor(t);
        mp_obj_dict_store(tensor, MP_OBJ_NEW_QSTR(qstr_from_str("data")),
            mp_obj_new_bytes(r->outputs[i].data, t->bytes));
        mp_obj_dict_store(outputs, mp_obj_new_str(t->name, strlen(t->name)), tensor);
    }
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("outputs")), outputs);
    if (r->result_json) {
        mp_obj_t payload = python_inference_json(r->result_json);
        if (!strcmp(r->result_json, "{\"kind\":\"raw\"}"))
            mp_obj_dict_store(payload, MP_OBJ_NEW_QSTR(qstr_from_str("tensors")), outputs);
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("result")), payload);
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(qstr_from_str("transforms")), python_inference_json(r->transforms_json));
        python_dict_store_u64(d, "preprocess_us", r->preprocess_us);
        python_dict_store_u64(d, "postprocess_us", r->postprocess_us);
        python_dict_store_u64(d, "postprocess_ms", r->postprocess_us / 1000);
    }
    return d;
}
static mp_obj_t python_inference_run(size_t n, const mp_obj_t *args, bool adapted)
{
    const solar_os_inference_model_info_t *info = python_inference_info(args[0]);
    if (!mp_obj_is_type(args[1], &mp_type_dict)) mp_raise_TypeError(MP_ERROR_TEXT("expected named tensor dict"));
    mp_obj_dict_t *dict = MP_OBJ_TO_PTR(args[1]);
    if (dict->map.used != info->input_count) mp_raise_ValueError(MP_ERROR_TEXT("all model inputs are required"));
    /* Interpreter-owned scratch is reclaimed even when parsing raises. */
    solar_os_inference_input_t *inputs = m_new0(solar_os_inference_input_t, info->input_count);
    solar_os_inference_tensor_t *typed = m_new0(solar_os_inference_tensor_t, info->input_count);
    solar_os_inference_value_t *values = adapted ? m_new0(solar_os_inference_value_t, info->input_count) : NULL;
#if SOLAR_OS_PACKAGE_SERVICE_IMAGE
    solar_os_raster_image_pixels_t *images = adapted ? m_new(solar_os_raster_image_pixels_t, info->input_count) : NULL;
#endif
    size_t count = 0;
    for (size_t slot = 0; slot < dict->map.alloc; ++slot) {
        if (!mp_map_slot_is_filled(&dict->map, slot)) continue;
        inputs[count].name = python_inference_name(dict->map.table[slot].key);
        size_t port = 0;
        while (port < info->input_count && strcmp(inputs[count].name, info->inputs[port].name)) ++port;
        if (port == info->input_count) mp_raise_ValueError(MP_ERROR_TEXT("unknown input name"));
        mp_obj_t value = dict->map.table[slot].value;
        if (adapted && info->image_inputs[port]) {
#if SOLAR_OS_PACKAGE_SERVICE_IMAGE
            python_check_esp(solar_os_raster_image_pixels(python_app.images[python_image_slot_from_obj(value)], &images[count]));
            values[count].tensor.name = inputs[count].name; values[count].image = &images[count]; ++count; continue;
#else
            mp_raise_ValueError(MP_ERROR_TEXT("image package is required"));
#endif
        }
        if (mp_obj_is_type(value, &mp_type_dict)) {
            mp_obj_dict_t *td = MP_OBJ_TO_PTR(value);
            for (size_t k = 0; k < td->map.alloc; ++k) {
                if (!mp_map_slot_is_filled(&td->map, k)) continue;
                const char *key = python_inference_name(td->map.table[k].key);
                if (strcmp(key, "data") && strcmp(key, "dtype") && strcmp(key, "shape") &&
                    strcmp(key, "bytes") && strcmp(key, "exponents"))
                    mp_raise_ValueError(MP_ERROR_TEXT("unknown tensor field"));
            }
            typed[count] = info->inputs[port];
            python_check_esp(solar_os_tensor_dtype_parse(python_inference_name(
                python_get_dict_obj(value, "dtype", true)), &typed[count].dtype));
            mp_obj_t *shape; size_t rank;
            mp_obj_get_array(python_get_dict_obj(value, "shape", true), &rank, &shape);
            if (rank > SOLAR_OS_INFERENCE_RANK_MAX) mp_raise_ValueError(MP_ERROR_TEXT("tensor rank too large"));
            typed[count].rank = rank;
            for (size_t k = 0; k < rank; ++k) typed[count].shape[k] = python_u32_from_obj(shape[k]);
            mp_obj_t exponents = python_get_dict_obj(value, "exponents", false);
            if (exponents != MP_OBJ_NULL) {
                mp_obj_t *values; size_t length;
                mp_obj_get_array(exponents, &length, &values);
                if (length != typed[count].exponent_count) mp_raise_ValueError(MP_ERROR_TEXT("quantization mismatch"));
                for (size_t k = 0; k < length; ++k)
                    if (mp_obj_get_int(values[k]) != typed[count].exponents[k])
                        mp_raise_ValueError(MP_ERROR_TEXT("quantization mismatch"));
            }
            inputs[count].tensor = &typed[count];
            mp_obj_t data = python_get_dict_obj(value, "data", true);
            mp_buffer_info_t buffer; mp_get_buffer_raise(data, &buffer, MP_BUFFER_READ);
            inputs[count].data = buffer.buf; inputs[count].bytes = buffer.len;
            typed[count].bytes = buffer.len;
            mp_obj_t length = python_get_dict_obj(value, "bytes", false);
            if (length != MP_OBJ_NULL && python_size_from_obj(length) != buffer.len)
                mp_raise_ValueError(MP_ERROR_TEXT("tensor byte count mismatch"));
        } else {
            mp_buffer_info_t buffer; mp_get_buffer_raise(value, &buffer, MP_BUFFER_READ);
            inputs[count].data = buffer.buf; inputs[count].bytes = buffer.len;
        }
        if (adapted) values[count].tensor = inputs[count];
        ++count;
    }
    solar_os_inference_result_t *result = NULL;
    esp_err_t error = adapted ? solar_os_inference_run_bundle(python_inference_get(), python_u32_from_obj(args[0]),
        values, count, python_inference_timeout(n, args, 2), &result) :
        solar_os_inference_run(python_inference_get(), python_u32_from_obj(args[0]),
            inputs, count, python_inference_timeout(n, args, 2), &result);
    python_check_esp(error);
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t value = python_inference_result(result);
        nlr_pop(); solar_os_inference_result_free(result); return value;
    }
    solar_os_inference_result_free(result); nlr_jump(nlr.ret_val);
}
static mp_obj_t solaros_inference_run(size_t n, const mp_obj_t *args)
{ return python_inference_run(n, args, false); }
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_inference_run_obj, 2, 3, solaros_inference_run);
static mp_obj_t solaros_inference_run_bundle(size_t n, const mp_obj_t *args)
{ return python_inference_run(n, args, true); }
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_inference_run_bundle_obj, 2, 3, solaros_inference_run_bundle);
static mp_obj_t solaros_inference_close(mp_obj_t handle)
{
    python_check_esp(solar_os_inference_close(python_inference_get(), python_u32_from_obj(handle)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_inference_close_obj, solaros_inference_close);
static mp_obj_t solaros_inference_reset(mp_obj_t handle)
{
    python_check_esp(solar_os_inference_reset(python_inference_get(), python_u32_from_obj(handle)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_inference_reset_obj, solaros_inference_reset);
static mp_obj_t solaros_inference_list(void)
{
    uint32_t handles[SOLAR_OS_INFERENCE_MODELS_MAX]; size_t count;
    python_check_esp(solar_os_inference_list(handles, SOLAR_OS_INFERENCE_MODELS_MAX, &count));
    mp_obj_t result = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count; ++i) mp_obj_list_append(result, mp_obj_new_int_from_uint(handles[i]));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_inference_list_obj, solaros_inference_list);
static mp_obj_t solaros_inference_find(mp_obj_t value)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX]; python_resolve_path_obj(value, path, sizeof(path));
    uint32_t handle; esp_err_t error = solar_os_inference_find(path, &handle);
    if (error == ESP_ERR_NOT_FOUND) return mp_const_none;
    python_check_esp(error); return mp_obj_new_int_from_uint(handle);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_inference_find_obj, solaros_inference_find);
#include "solar_os_python_tensor_image.inc"
static mp_obj_t solaros_inference_set_mode(mp_obj_t handle, mp_obj_t value)
{
    solar_os_inference_mode_t mode;
    python_check_esp(solar_os_inference_mode_parse(python_inference_name(value), &mode));
    python_check_esp(solar_os_inference_set_mode(python_inference_get(),
        python_u32_from_obj(handle), mode));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_inference_set_mode_obj, solaros_inference_set_mode);
static mp_obj_t solaros_inference_close_all(void)
{
    python_check_esp(solar_os_inference_close_all(python_inference_get())); return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_inference_close_all_obj, solaros_inference_close_all);
#else
static bool python_inference_active(void) { return false; }
static void python_inference_destroy(void) {}
#endif
