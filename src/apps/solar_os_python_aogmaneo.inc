#if SOLAR_OS_PACKAGE_SERVICE_AOGMANEO
#define PY_AON_CONTEXT (&python_runtime_state->aogmaneo)
static bool python_aogmaneo_active(void)
{ return python_runtime_state && solar_os_aogmaneo_active(PY_AON_CONTEXT); }
static void python_aogmaneo_cleanup(void)
{
    if (python_runtime_state)
        while (solar_os_aogmaneo_close_all(PY_AON_CONTEXT)) vTaskDelay(pdMS_TO_TICKS(20));
}
static void python_aogmaneo_check(int result)
{
    if (!result) return;
    if (result == SOLAR_OS_NATIVE_ERROR_NO_MEMORY) mp_raise_type(&mp_type_MemoryError);
    if (result == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT || result == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid AOgmaNeo arguments"));
    if (result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND)
        mp_raise_ValueError(MP_ERROR_TEXT("AOgmaNeo library unavailable; run pkg install aogmaneo"));
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("AOgmaNeo error %d"), result);
}
#define PY_AON_CALL(expression) do { \
    if (python_should_cancel(NULL)) mp_raise_type(&mp_type_KeyboardInterrupt); \
    int aon_result = (expression); \
    if (python_should_cancel(NULL)) mp_raise_type(&mp_type_KeyboardInterrupt); \
    python_aogmaneo_check(aon_result); \
} while (0)
static void python_aogmaneo_args(size_t count, const mp_obj_t *args, mp_map_t *kwargs,
    const char *const *names, size_t total, mp_obj_t *values)
{
    if (count > total) mp_raise_TypeError(MP_ERROR_TEXT("too many AOgmaNeo arguments"));
    if (kwargs) for (size_t i = 0; i < kwargs->alloc; ++i) if (mp_map_slot_is_filled(kwargs, i)) {
        size_t bytes;
        const char *key = mp_obj_str_get_data(kwargs->table[i].key, &bytes);
        size_t n = 0;
        for (; n < total && (strlen(names[n]) != bytes || memcmp(key, names[n], bytes)); ++n) {}
        if (n == total) mp_raise_TypeError(MP_ERROR_TEXT("unknown AOgmaNeo argument"));
    }
    for (size_t i = 0; i < total; ++i) {
        values[i] = python_kw_value(kwargs, names[i]);
        if (i < count) {
            if (values[i] != MP_OBJ_NULL) mp_raise_TypeError(MP_ERROR_TEXT("duplicate AOgmaNeo argument"));
            values[i] = args[i];
        }
    }
}
static int32_t python_aogmaneo_int(mp_obj_t value, int32_t low, int32_t high, int32_t fallback)
{
    mp_int_t n = value == MP_OBJ_NULL ? fallback : mp_obj_get_int(value);
    if (n < low || n > high) mp_raise_ValueError(MP_ERROR_TEXT("AOgmaNeo integer out of range"));
    return (int32_t)n;
}
static mp_obj_t *python_aogmaneo_array(mp_obj_t value, size_t expected, size_t maximum, size_t *count)
{
    mp_obj_t *items;
    if (value == MP_OBJ_NULL) mp_raise_TypeError(MP_ERROR_TEXT("missing AOgmaNeo argument"));
    mp_obj_get_array(value, count, &items);
    if (!*count || *count > maximum || (expected && *count != expected))
        mp_raise_ValueError(MP_ERROR_TEXT("invalid AOgmaNeo array size"));
    return items;
}
static solar_os_aogmaneo_size_v1_t python_aogmaneo_size(mp_obj_t object)
{
    size_t count;
    mp_obj_t *v = python_aogmaneo_array(object, 3, 3, &count);
    return (solar_os_aogmaneo_size_v1_t){python_aogmaneo_int(v[0], 1, 64, 0),
        python_aogmaneo_int(v[1], 1, 64, 0), python_aogmaneo_int(v[2], 1, 256, 0)};
}
static uint32_t python_aogmaneo_handle(mp_obj_t object, bool image, solar_os_aogmaneo_info_t *info)
{
    uint32_t handle = python_aogmaneo_int(object, 1, 0x3fffffff, 0);
    const solar_os_aogmaneo_info_t *found = solar_os_aogmaneo_info(PY_AON_CONTEXT, handle);
    if (!found || found->image != image) mp_raise_ValueError(MP_ERROR_TEXT("invalid AOgmaNeo model"));
    if (info) *info = *found;
    return handle;
}
static size_t python_aogmaneo_columns(solar_os_aogmaneo_size_v1_t size)
{ return (size_t)size.x * size.y; }
static int32_t *python_aogmaneo_codes(mp_obj_t value, size_t expected, int depth)
{
    size_t count;
    mp_obj_t *items = python_aogmaneo_array(value, expected, 4096, &count);
    int32_t *codes = m_new(int32_t, count);
    for (size_t i = 0; i < count; ++i) codes[i] = python_aogmaneo_int(items[i], 0, depth - 1, 0);
    return codes;
}
static mp_obj_t python_aogmaneo_code_list(const int32_t *codes, size_t count)
{
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (size_t i = 0; i < count; ++i) mp_obj_list_append(list, mp_obj_new_int(codes[i]));
    return list;
}
static mp_obj_t solaros_aogmaneo_create(size_t count, const mp_obj_t *args, mp_map_t *kwargs)
{
    const char *names[] = {"inputs", "layers", "seed"};
    mp_obj_t v[3];
    python_aogmaneo_args(count, args, kwargs, names, 3, v);
    size_t ni, nl;
    mp_obj_t *inputs = python_aogmaneo_array(v[0], 0, 8, &ni);
    mp_obj_t *layers = python_aogmaneo_array(v[1], 0, 8, &nl);
    solar_os_aogmaneo_io_v1_t ios[8];
    solar_os_aogmaneo_layer_v1_t hidden[8];
    const char *io_names[] = {"size", "type", "dendrites", "up_radius", "down_radius",
        "value_size", "value_dendrites", "history_capacity"};
    for (size_t i = 0; i < ni; ++i) {
        mp_obj_t p[8];
        python_aogmaneo_args(0, NULL, mp_obj_dict_get_map(inputs[i]), io_names, 8, p);
        ios[i] = (solar_os_aogmaneo_io_v1_t){python_aogmaneo_size(p[0]),
            python_aogmaneo_int(p[1], 0, 2, 1), python_aogmaneo_int(p[2], 2, 16, 4),
            python_aogmaneo_int(p[3], 0, 8, 2), python_aogmaneo_int(p[4], 0, 8, 2),
            python_aogmaneo_int(p[5], 2, 256, 128), python_aogmaneo_int(p[6], 2, 16, 2),
            python_aogmaneo_int(p[7], 2, 512, 512)};
    }
    const char *layer_names[] = {"size", "dendrites", "up_radius", "recurrent_radius", "down_radius"};
    for (size_t i = 0; i < nl; ++i) {
        mp_obj_t p[5];
        python_aogmaneo_args(0, NULL, mp_obj_dict_get_map(layers[i]), layer_names, 5, p);
        hidden[i] = (solar_os_aogmaneo_layer_v1_t){python_aogmaneo_size(p[0]),
            python_aogmaneo_int(p[1], 2, 16, 4), python_aogmaneo_int(p[2], 0, 8, 2),
            python_aogmaneo_int(p[3], -1, 8, 0), python_aogmaneo_int(p[4], 0, 8, 2)};
    }
    uint32_t seed = v[2] == MP_OBJ_NULL ? 12345 : python_u32_from_obj(v[2]), token = 0;
    PY_AON_CALL(solar_os_aogmaneo_create(PY_AON_CONTEXT, ios, ni, hidden, nl, seed, &token));
    return mp_obj_new_int(token);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_aogmaneo_create_obj, 0, solaros_aogmaneo_create);
static mp_obj_t solaros_aogmaneo_destroy(mp_obj_t object)
{
    uint32_t token = python_aogmaneo_int(object, 1, 0x3fffffff, 0);
    PY_AON_CALL(solar_os_aogmaneo_destroy(PY_AON_CONTEXT, token));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_aogmaneo_destroy_obj, solaros_aogmaneo_destroy);
static mp_obj_t solaros_aogmaneo_step(size_t count, const mp_obj_t *args, mp_map_t *kwargs)
{
    const char *names[] = {"model", "inputs", "learn", "reward", "mimic"};
    mp_obj_t v[5];
    python_aogmaneo_args(count, args, kwargs, names, 5, v);
    solar_os_aogmaneo_info_t info;
    uint32_t token = python_aogmaneo_handle(v[0], false, &info);
    size_t n;
    mp_obj_t *items = python_aogmaneo_array(v[1], info.input_count, 8, &n);
    solar_os_aogmaneo_codes_v1_t inputs[8];
    for (size_t i = 0; i < n; ++i) {
        inputs[i].count = python_aogmaneo_columns(info.inputs[i]);
        inputs[i].data = python_aogmaneo_codes(items[i], inputs[i].count, info.inputs[i].z);
    }
    bool learn = v[2] == MP_OBJ_NULL || mp_obj_is_true(v[2]);
    float reward = v[3] == MP_OBJ_NULL ? 0 : mp_obj_get_float(v[3]);
    float mimic = v[4] == MP_OBJ_NULL ? 0 : mp_obj_get_float(v[4]);
    PY_AON_CALL(solar_os_aogmaneo_step(PY_AON_CONTEXT, token, inputs, n, learn, reward, mimic));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_aogmaneo_step_obj, 0, solaros_aogmaneo_step);
static mp_obj_t python_aogmaneo_read(size_t count, const mp_obj_t *args, unsigned kind)
{
    solar_os_aogmaneo_info_t info;
    uint32_t token = python_aogmaneo_handle(args[0], false, &info);
    unsigned limit = kind == SOLAR_OS_AOGMANEO_READ_HIDDEN ? info.layer_count : info.input_count;
    unsigned index = count == 1 ? 0 : python_aogmaneo_int(args[1], 0, limit - 1, 0);
    solar_os_aogmaneo_size_v1_t shape = kind == SOLAR_OS_AOGMANEO_READ_HIDDEN ? info.layers[index] : info.inputs[index];
    size_t n = python_aogmaneo_columns(shape);
    if (kind == SOLAR_OS_AOGMANEO_READ_ACTIVATIONS) n *= shape.z;
    bool floats = kind == SOLAR_OS_AOGMANEO_READ_ACTIVATIONS || kind == SOLAR_OS_AOGMANEO_READ_VALUES;
    if (floats) {
        float *data = m_new(float, n);
        PY_AON_CALL(solar_os_aogmaneo_read(PY_AON_CONTEXT, token, kind, index, data, n));
        mp_obj_t list = mp_obj_new_list(0, NULL);
        for (size_t i = 0; i < n; ++i) mp_obj_list_append(list, mp_obj_new_float(data[i]));
        return list;
    }
    int32_t *data = m_new(int32_t, n);
    PY_AON_CALL(solar_os_aogmaneo_read(PY_AON_CONTEXT, token, kind, index, data, n));
    return python_aogmaneo_code_list(data, n);
}
static mp_obj_t solaros_aogmaneo_prediction(size_t n, const mp_obj_t *v)
{ return python_aogmaneo_read(n, v, SOLAR_OS_AOGMANEO_READ_PREDICTION); }
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_aogmaneo_prediction_obj, 1, 2, solaros_aogmaneo_prediction);
static mp_obj_t solaros_aogmaneo_activations(size_t n, const mp_obj_t *v)
{ return python_aogmaneo_read(n, v, SOLAR_OS_AOGMANEO_READ_ACTIVATIONS); }
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_aogmaneo_activations_obj, 1, 2, solaros_aogmaneo_activations);
static mp_obj_t solaros_aogmaneo_values(size_t n, const mp_obj_t *v)
{ return python_aogmaneo_read(n, v, SOLAR_OS_AOGMANEO_READ_VALUES); }
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_aogmaneo_values_obj, 1, 2, solaros_aogmaneo_values);
static mp_obj_t solaros_aogmaneo_hidden(size_t n, const mp_obj_t *v)
{ return python_aogmaneo_read(n, v, SOLAR_OS_AOGMANEO_READ_HIDDEN); }
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_aogmaneo_hidden_obj, 1, 2, solaros_aogmaneo_hidden);
static mp_obj_t solaros_aogmaneo_clear(mp_obj_t object)
{
    uint32_t token = python_aogmaneo_handle(object, false, NULL);
    PY_AON_CALL(solar_os_aogmaneo_clear(PY_AON_CONTEXT, token));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_aogmaneo_clear_obj, solaros_aogmaneo_clear);
static mp_obj_t solaros_aogmaneo_image_create(size_t count, const mp_obj_t *args, mp_map_t *kwargs)
{
    const char *names[] = {"hidden", "visible", "seed"};
    mp_obj_t v[3];
    python_aogmaneo_args(count, args, kwargs, names, 3, v);
    solar_os_aogmaneo_size_v1_t hidden = python_aogmaneo_size(v[0]);
    size_t n;
    mp_obj_t *items = python_aogmaneo_array(v[1], 0, 8, &n);
    solar_os_aogmaneo_visible_v1_t visible[8];
    const char *fields[] = {"size", "radius"};
    for (size_t i = 0; i < n; ++i) {
        mp_obj_t p[2];
        python_aogmaneo_args(0, NULL, mp_obj_dict_get_map(items[i]), fields, 2, p);
        visible[i] = (solar_os_aogmaneo_visible_v1_t){python_aogmaneo_size(p[0]), python_aogmaneo_int(p[1], 0, 8, 2)};
    }
    uint32_t seed = v[2] == MP_OBJ_NULL ? 12345 : python_u32_from_obj(v[2]), token = 0;
    PY_AON_CALL(solar_os_aogmaneo_image_create(PY_AON_CONTEXT, hidden, visible, n, seed, &token));
    return mp_obj_new_int(token);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_aogmaneo_image_create_obj, 0, solaros_aogmaneo_image_create);
static mp_obj_t solaros_aogmaneo_image_step(size_t count, const mp_obj_t *args, mp_map_t *kwargs)
{
    const char *names[] = {"model", "inputs", "learn", "learn_reconstruction"};
    mp_obj_t v[4];
    python_aogmaneo_args(count, args, kwargs, names, 4, v);
    solar_os_aogmaneo_info_t info;
    uint32_t token = python_aogmaneo_handle(v[0], true, &info);
    size_t n;
    mp_obj_t *items = python_aogmaneo_array(v[1], info.input_count, 8, &n);
    solar_os_aogmaneo_bytes_v1_t inputs[8];
    for (size_t i = 0; i < n; ++i) {
        mp_buffer_info_t buffer;
        mp_get_buffer_raise(items[i], &buffer, MP_BUFFER_READ);
        size_t expected = python_aogmaneo_columns(info.inputs[i]) * info.inputs[i].z;
        if (buffer.len != expected) mp_raise_ValueError(MP_ERROR_TEXT("invalid AOgmaNeo image size"));
        inputs[i] = (solar_os_aogmaneo_bytes_v1_t){buffer.buf, buffer.len};
    }
    size_t codes = python_aogmaneo_columns(info.layers[0]);
    int32_t *output = m_new(int32_t, codes);
    bool learn = v[2] == MP_OBJ_NULL || mp_obj_is_true(v[2]);
    bool recon = v[3] != MP_OBJ_NULL && mp_obj_is_true(v[3]);
    PY_AON_CALL(solar_os_aogmaneo_image_step(PY_AON_CONTEXT, token, inputs, n, learn, recon, output, codes));
    return python_aogmaneo_code_list(output, codes);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_aogmaneo_image_step_obj, 0, solaros_aogmaneo_image_step);
static mp_obj_t solaros_aogmaneo_image_reconstruct(size_t count, const mp_obj_t *args)
{
    solar_os_aogmaneo_info_t info;
    uint32_t token = python_aogmaneo_handle(args[0], true, &info);
    unsigned visible = count < 3 ? 0 : python_aogmaneo_int(args[2], 0, info.input_count - 1, 0);
    size_t n = python_aogmaneo_columns(info.layers[0]);
    int32_t *codes = python_aogmaneo_codes(args[1], n, info.layers[0].z);
    size_t bytes = python_aogmaneo_columns(info.inputs[visible]) * info.inputs[visible].z;
    uint8_t *output = m_new(uint8_t, bytes);
    PY_AON_CALL(solar_os_aogmaneo_image_reconstruct(PY_AON_CONTEXT, token, codes, n, visible, output, bytes));
    return mp_obj_new_bytes(output, bytes);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_aogmaneo_image_reconstruct_obj, 2, 3, solaros_aogmaneo_image_reconstruct);
#undef PY_AON_CALL
#undef PY_AON_CONTEXT
#else
static bool python_aogmaneo_active(void) { return false; }
static void python_aogmaneo_cleanup(void) {}
#endif
