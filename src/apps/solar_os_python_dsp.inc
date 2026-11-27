#define PYTHON_DSP_MAX_SAMPLES 32768U

typedef enum {
    PYTHON_DSP_FIR,
    PYTHON_DSP_DECIMATOR,
    PYTHON_DSP_FFT,
} python_dsp_kind_t;

typedef struct {
    mp_obj_base_t base;
    python_dsp_kind_t kind;
    union {
        solar_os_native_handle_t fir;
        solar_os_native_handle_t decimator;
        solar_os_native_handle_t fft;
        solar_os_native_handle_t context;
    } processor;
    const solar_os_native_dsp_api_v1_t *api;
} python_dsp_processor_t;

static const mp_obj_type_t solaros_dsp_processor_type;

static const solar_os_native_dsp_api_v1_t *python_dsp_api(void)
{
    const solar_os_native_dsp_api_v1_t *api = solar_os_native_dsp_get(
        SOLAR_OS_SCRIPT_SERVICE_QUERY(dsp),
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_dsp_api_v1_t, fft_execute));
    if (!api) mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("DSP service unavailable"));
    return api;
}
static void python_dsp_check(int result)
{
    if (!result) return;
    if (result == SOLAR_OS_NATIVE_ERROR_NO_MEMORY) mp_raise_type(&mp_type_MemoryError);
    if (result == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT || result == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid DSP arguments"));
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("DSP error %d"), result);
}


static size_t python_dsp_s16_buffer(mp_obj_t object,
                                    mp_buffer_info_t *buffer,
                                    mp_uint_t flags)
{
    mp_get_buffer_raise(object, buffer, flags);
    if ((buffer->len & 1U) != 0U) {
        mp_raise_ValueError(MP_ERROR_TEXT("DSP buffer length must be even"));
    }
    const size_t count = buffer->len / sizeof(int16_t);
    if (count > PYTHON_DSP_MAX_SAMPLES) {
        mp_raise_ValueError(MP_ERROR_TEXT("DSP buffer has too many samples"));
    }
    return count;
}

static int16_t python_dsp_q15(mp_obj_t object)
{
    const mp_float_t value = mp_obj_get_float(object);
    if (!isfinite(value) || value < -1.0 || value > 1.0) {
        mp_raise_ValueError(MP_ERROR_TEXT("gain must be between -1.0 and 1.0"));
    }
    if (value >= 1.0) {
        return INT16_MAX;
    }
    if (value <= -1.0) {
        return INT16_MIN;
    }
    const long converted = lrint((double)value * 32768.0);
    return converted > INT16_MAX ? INT16_MAX : (int16_t)converted;
}

static int16_t python_dsp_i16(mp_obj_t object)
{
    const mp_int_t value = mp_obj_get_int(object);
    if (value < INT16_MIN || value > INT16_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected signed 16-bit integer"));
    }
    return (int16_t)value;
}

static mp_obj_t python_dsp_output(size_t samples, mp_buffer_info_t *buffer)
{
    if (samples > SIZE_MAX / sizeof(int16_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("DSP output is too large"));
    }
    const size_t bytes = samples * sizeof(int16_t);
    mp_obj_t result = mp_obj_new_bytearray_by_ref(bytes, m_new0(uint8_t, bytes));
    mp_get_buffer_raise(result, buffer, MP_BUFFER_WRITE);
    return result;
}

static mp_obj_t solaros_dsp_backend(void)
{
    return mp_obj_new_str_from_cstr(python_dsp_api()->backend());
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_dsp_backend_obj, solaros_dsp_backend);

static mp_obj_t solaros_dsp_capabilities(void)
{
    mp_obj_t result = mp_obj_new_dict(3);
    python_dict_store_cstr(result, "backend", python_dsp_api()->backend());
    python_dict_store_uint(result, "supported", python_dsp_api()->capabilities());
    python_dict_store_uint(result, "accelerated", python_dsp_api()->accelerated_capabilities());
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_dsp_capabilities_obj, solaros_dsp_capabilities);

static mp_obj_t solaros_dsp_dot(mp_obj_t a_object, mp_obj_t b_object)
{
    mp_buffer_info_t a;
    mp_buffer_info_t b;
    const size_t count = python_dsp_s16_buffer(a_object, &a, MP_BUFFER_READ);
    if (python_dsp_s16_buffer(b_object, &b, MP_BUFFER_READ) != count) {
        mp_raise_ValueError(MP_ERROR_TEXT("DSP buffers must have equal length"));
    }
    int64_t value = 0;
    python_dsp_check(python_dsp_api()->dot_s16(a.buf, b.buf, count, &value));
    return python_i64_to_obj(value);
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_dsp_dot_obj, solaros_dsp_dot);

static mp_obj_t solaros_dsp_gain(mp_obj_t input_object, mp_obj_t gain_object)
{
    mp_buffer_info_t input;
    mp_buffer_info_t output;
    const size_t count = python_dsp_s16_buffer(input_object, &input, MP_BUFFER_READ);
    mp_obj_t result = python_dsp_output(count, &output);
    python_dsp_check(python_dsp_api()->gain_q15(output.buf, input.buf, count,
                                           python_dsp_q15(gain_object)));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_dsp_gain_obj, solaros_dsp_gain);

static mp_obj_t solaros_dsp_mix(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    mp_buffer_info_t a;
    mp_buffer_info_t b;
    mp_buffer_info_t output;
    const size_t count = python_dsp_s16_buffer(args[0], &a, MP_BUFFER_READ);
    if (python_dsp_s16_buffer(args[1], &b, MP_BUFFER_READ) != count) {
        mp_raise_ValueError(MP_ERROR_TEXT("DSP buffers must have equal length"));
    }
    mp_obj_t result = python_dsp_output(count, &output);
    python_dsp_check(python_dsp_api()->mix_q15(output.buf, a.buf, b.buf, count,
                                          python_dsp_q15(args[2]),
                                          python_dsp_q15(args[3])));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_dsp_mix_obj, 4, 4, solaros_dsp_mix);

static mp_obj_t solaros_dsp_clip(size_t n_args, const mp_obj_t *args)
{
    (void)n_args;
    mp_buffer_info_t input;
    mp_buffer_info_t output;
    const size_t count = python_dsp_s16_buffer(args[0], &input, MP_BUFFER_READ);
    mp_obj_t result = python_dsp_output(count, &output);
    python_dsp_check(python_dsp_api()->clip_s16(output.buf, input.buf, count,
                                           python_dsp_i16(args[1]),
                                           python_dsp_i16(args[2])));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_dsp_clip_obj, 3, 3, solaros_dsp_clip);

static mp_obj_t solaros_dsp_level(mp_obj_t input_object)
{
    mp_buffer_info_t input;
    const size_t count = python_dsp_s16_buffer(input_object, &input, MP_BUFFER_READ);
    solar_os_dsp_level_t level;
    python_dsp_check(python_dsp_api()->level_s16(input.buf, count, &level));
    mp_obj_t values[] = {
        mp_obj_new_int_from_uint(level.peak),
        mp_obj_new_int_from_uint(level.rms),
    };
    return mp_obj_new_tuple(MP_ARRAY_SIZE(values), values);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_dsp_level_obj, solaros_dsp_level);

static mp_obj_t solaros_dsp_window(mp_obj_t input_object, mp_obj_t window_object)
{
    mp_buffer_info_t input;
    mp_buffer_info_t window;
    mp_buffer_info_t output;
    const size_t count = python_dsp_s16_buffer(input_object, &input, MP_BUFFER_READ);
    if (python_dsp_s16_buffer(window_object, &window, MP_BUFFER_READ) != count) {
        mp_raise_ValueError(MP_ERROR_TEXT("DSP buffers must have equal length"));
    }
    mp_obj_t result = python_dsp_output(count, &output);
    python_dsp_check(python_dsp_api()->window_q15(output.buf, input.buf, window.buf, count));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_dsp_window_obj, solaros_dsp_window);

static python_dsp_processor_t *python_dsp_self(mp_obj_t self_object)
{
    if (!mp_obj_is_type(self_object, &solaros_dsp_processor_type)) {
        mp_raise_TypeError(MP_ERROR_TEXT("expected DSP processor"));
    }
    python_dsp_processor_t *self = MP_OBJ_TO_PTR(self_object);
    if (self->processor.context == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("DSP processor is closed"));
    }
    return self;
}

static int python_dsp_destroy(python_dsp_processor_t *self)
{
    if (!self || !self->processor.context) return SOLAR_OS_NATIVE_OK;
    int result;
    if (self->kind == PYTHON_DSP_FIR) result = self->api->fir_close(self->processor.fir);
    else if (self->kind == PYTHON_DSP_DECIMATOR) result = self->api->decimator_close(self->processor.decimator);
    else result = self->api->fft_close(self->processor.fft);
    if (result == SOLAR_OS_NATIVE_OK || result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) {
        self->processor.context = 0;
        return SOLAR_OS_NATIVE_OK;
    }
    return result;
}

static mp_obj_t solaros_dsp_processor_close(mp_obj_t self_object)
{
    if (mp_obj_is_type(self_object, &solaros_dsp_processor_type)) {
        python_dsp_check(python_dsp_destroy(MP_OBJ_TO_PTR(self_object)));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_dsp_processor_close_obj,
                          solaros_dsp_processor_close);

static mp_obj_t solaros_dsp_processor_reset(mp_obj_t self_object)
{
    python_dsp_processor_t *self = python_dsp_self(self_object);
    if (self->kind == PYTHON_DSP_FIR) {
        python_dsp_check(self->api->fir_reset(self->processor.fir));
    } else if (self->kind == PYTHON_DSP_DECIMATOR) {
        python_dsp_check(self->api->decimator_reset(self->processor.decimator));
    } else {
        mp_raise_TypeError(MP_ERROR_TEXT("FFT processor has no streaming state"));
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_dsp_processor_reset_obj,
                          solaros_dsp_processor_reset);

static mp_obj_t solaros_dsp_processor_process(mp_obj_t self_object,
                                              mp_obj_t input_object)
{
    python_dsp_processor_t *self = python_dsp_self(self_object);
    if (self->kind == PYTHON_DSP_FFT) {
        mp_raise_TypeError(MP_ERROR_TEXT("use execute() for FFT processors"));
    }
    mp_buffer_info_t input;
    mp_buffer_info_t output;
    const size_t count = python_dsp_s16_buffer(input_object, &input, MP_BUFFER_READ);
    size_t output_count = count;
    if (self->kind == PYTHON_DSP_DECIMATOR) {
        output_count = self->api->decimator_output_count(self->processor.decimator,
                                                            count);
    }
    mp_obj_t result = python_dsp_output(output_count, &output);
    if (self->kind == PYTHON_DSP_FIR) {
        python_dsp_check(self->api->fir_process(self->processor.fir,
                                                  output.buf, input.buf, count));
    } else {
        size_t produced = 0;
        python_dsp_check(self->api->decimator_process(self->processor.decimator,
                                                        output.buf, output_count,
                                                        input.buf, count, &produced));
        if (produced != output_count) {
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("DSP output count mismatch"));
        }
    }
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_dsp_processor_process_obj,
                          solaros_dsp_processor_process);

static mp_obj_t solaros_dsp_processor_execute(mp_obj_t self_object,
                                              mp_obj_t input_object)
{
    python_dsp_processor_t *self = python_dsp_self(self_object);
    if (self->kind != PYTHON_DSP_FFT) {
        mp_raise_TypeError(MP_ERROR_TEXT("execute() requires an FFT processor"));
    }
    mp_buffer_info_t input;
    mp_buffer_info_t output;
    const size_t count = python_dsp_s16_buffer(input_object, &input, MP_BUFFER_READ);
    const size_t fft_size = self->api->fft_size(self->processor.fft);
    if (count != fft_size) {
        mp_raise_ValueError(MP_ERROR_TEXT("input length does not match FFT size"));
    }
    mp_obj_t result = python_dsp_output(fft_size * 2U, &output);
    uint8_t exponent = 0;
    python_dsp_check(self->api->fft_execute(self->processor.fft, output.buf,
                                              input.buf, &exponent));
    mp_obj_t values[] = {result, mp_obj_new_int_from_uint(exponent)};
    return mp_obj_new_tuple(MP_ARRAY_SIZE(values), values);
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_dsp_processor_execute_obj,
                          solaros_dsp_processor_execute);

static const mp_rom_map_elem_t solaros_dsp_processor_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_process), MP_ROM_PTR(&solaros_dsp_processor_process_obj)},
    {MP_ROM_QSTR(MP_QSTR_execute), MP_ROM_PTR(&solaros_dsp_processor_execute_obj)},
    {MP_ROM_QSTR(MP_QSTR_reset), MP_ROM_PTR(&solaros_dsp_processor_reset_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&solaros_dsp_processor_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&solaros_dsp_processor_close_obj)},
};
static MP_DEFINE_CONST_DICT(solaros_dsp_processor_locals,
                            solaros_dsp_processor_locals_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    solaros_dsp_processor_type,
    MP_QSTR_dsp_processor,
    MP_TYPE_FLAG_NONE,
    locals_dict, &solaros_dsp_processor_locals);

static mp_obj_t solaros_dsp_fir(size_t n_args,
                                const mp_obj_t *args,
                                mp_map_t *kw_args)
{
    mp_arg_check_num(n_args, kw_args != NULL ? kw_args->used : 0, 1, 2, true);
    python_check_known_kwargs(kw_args, "decimation", NULL, NULL, NULL);
    size_t decimation = n_args >= 2 ? python_size_from_obj(args[1]) : 1U;
    const mp_obj_t keyword = python_kw_value(kw_args, "decimation");
    if (keyword != MP_OBJ_NULL) {
        decimation = python_size_from_obj(keyword);
    }
    if (decimation == 0U || decimation > UINT16_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("decimation must be 1..65535"));
    }
    mp_buffer_info_t coefficients;
    const size_t taps = python_dsp_s16_buffer(args[0], &coefficients, MP_BUFFER_READ);
    if (taps == 0U || taps > SOLAR_OS_DSP_MAX_TAPS) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid FIR coefficient count"));
    }
    python_dsp_processor_t *self =
        mp_obj_malloc_with_finaliser(python_dsp_processor_t,
                                     &solaros_dsp_processor_type);
    self->processor.context = 0;
    self->api = python_dsp_api();
    int error;
    if (decimation == 1U) {
        self->kind = PYTHON_DSP_FIR;
        error = self->api->fir_create(coefficients.buf, taps,
                                         &self->processor.fir);
    } else {
        self->kind = PYTHON_DSP_DECIMATOR;
        error = self->api->decimator_create(coefficients.buf, taps, decimation,
                                               &self->processor.decimator);
    }
    python_dsp_check(error);
    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_dsp_fir_obj, 1, solaros_dsp_fir);

static mp_obj_t solaros_dsp_fft(mp_obj_t size_object)
{
    python_dsp_processor_t *self =
        mp_obj_malloc_with_finaliser(python_dsp_processor_t,
                                     &solaros_dsp_processor_type);
    self->processor.context = 0;
    self->api = python_dsp_api();
    self->kind = PYTHON_DSP_FFT;
    python_dsp_check(self->api->fft_create(python_size_from_obj(size_object),
                                             &self->processor.fft));
    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_dsp_fft_obj, solaros_dsp_fft);
