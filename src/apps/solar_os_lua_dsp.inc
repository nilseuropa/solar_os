#define SOLUA_DSP_MAX_SAMPLES 32768U
#define SOLUA_DSP_PROCESSOR_METATABLE "solaros.dsp.processor"

typedef enum {
    SOLUA_DSP_FIR,
    SOLUA_DSP_DECIMATOR,
    SOLUA_DSP_FFT,
} solua_dsp_kind_t;

typedef struct {
    solua_dsp_kind_t kind;
    union {
        solar_os_native_handle_t fir;
        solar_os_native_handle_t decimator;
        solar_os_native_handle_t fft;
        solar_os_native_handle_t context;
    } processor;
    const solar_os_native_dsp_api_v1_t *api;
} solua_dsp_processor_t;

static const solar_os_native_dsp_api_v1_t *solua_dsp_api(lua_State *L)
{
    const solar_os_native_dsp_api_v1_t *api = solar_os_native_dsp_get(
        SOLAR_OS_SCRIPT_SERVICE_QUERY(dsp),
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_dsp_api_v1_t, fft_execute));
    if (!api) luaL_error(L, "DSP service unavailable");
    return api;
}
static int solua_dsp_check(lua_State *L, int result)
{
    return result ? luaL_error(L, "DSP error %d", result) : 0;
}

static const int16_t *solua_dsp_s16_string(lua_State *L, int index,
                                           size_t *sample_count)
{
    size_t bytes = 0;
    const char *data = luaL_checklstring(L, index, &bytes);
    if ((bytes & 1U) != 0U) {
        luaL_error(L, "DSP string length must be even");
    }
    const size_t count = bytes / sizeof(int16_t);
    if (count > SOLUA_DSP_MAX_SAMPLES) {
        luaL_error(L, "DSP string has too many samples");
    }
    *sample_count = count;
    return (const int16_t *)data;
}

static int16_t solua_dsp_q15(lua_State *L, int index)
{
    const lua_Number value = luaL_checknumber(L, index);
    if (!isfinite(value) || value < -1.0 || value > 1.0) {
        luaL_error(L, "gain must be between -1.0 and 1.0");
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

static int16_t solua_dsp_i16(lua_State *L, int index)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < INT16_MIN || value > INT16_MAX) {
        luaL_error(L, "expected signed 16-bit integer");
    }
    return (int16_t)value;
}

static int16_t *solua_dsp_output(lua_State *L, size_t samples,
                                 luaL_Buffer *buffer)
{
    if (samples > SIZE_MAX / sizeof(int16_t)) {
        luaL_error(L, "DSP output is too large");
    }
    return (int16_t *)luaL_buffinitsize(L, buffer,
                                        samples * sizeof(int16_t));
}

static int solua_dsp_backend(lua_State *L)
{
    lua_pushstring(L, solua_dsp_api(L)->backend());
    return 1;
}

static int solua_dsp_capabilities(lua_State *L)
{
    lua_createtable(L, 0, 3);
    const int result = lua_gettop(L);
    solua_set_str(L, result, "backend", solua_dsp_api(L)->backend());
    solua_set_int(L, result, "supported", solua_dsp_api(L)->capabilities());
    solua_set_int(L, result, "accelerated",
                  solua_dsp_api(L)->accelerated_capabilities());
    return 1;
}

static int solua_dsp_dot(lua_State *L)
{
    size_t count = 0;
    size_t b_count = 0;
    const int16_t *a = solua_dsp_s16_string(L, 1, &count);
    const int16_t *b = solua_dsp_s16_string(L, 2, &b_count);
    if (b_count != count) {
        return luaL_error(L, "DSP strings must have equal length");
    }
    int64_t value = 0;
    (void)solua_dsp_check(L, solua_dsp_api(L)->dot_s16(a, b, count, &value));
    lua_pushinteger(L, (lua_Integer)value);
    return 1;
}

static int solua_dsp_gain(lua_State *L)
{
    size_t count = 0;
    const int16_t *input = solua_dsp_s16_string(L, 1, &count);
    luaL_Buffer buffer;
    int16_t *output = solua_dsp_output(L, count, &buffer);
    (void)solua_dsp_check(L, solua_dsp_api(L)->gain_q15(
        output, input, count, solua_dsp_q15(L, 2)));
    luaL_pushresultsize(&buffer, count * sizeof(int16_t));
    return 1;
}

static int solua_dsp_mix(lua_State *L)
{
    size_t count = 0;
    size_t b_count = 0;
    const int16_t *a = solua_dsp_s16_string(L, 1, &count);
    const int16_t *b = solua_dsp_s16_string(L, 2, &b_count);
    if (b_count != count) {
        return luaL_error(L, "DSP strings must have equal length");
    }
    luaL_Buffer buffer;
    int16_t *output = solua_dsp_output(L, count, &buffer);
    (void)solua_dsp_check(L, solua_dsp_api(L)->mix_q15(
        output, a, b, count, solua_dsp_q15(L, 3), solua_dsp_q15(L, 4)));
    luaL_pushresultsize(&buffer, count * sizeof(int16_t));
    return 1;
}

static int solua_dsp_clip(lua_State *L)
{
    size_t count = 0;
    const int16_t *input = solua_dsp_s16_string(L, 1, &count);
    luaL_Buffer buffer;
    int16_t *output = solua_dsp_output(L, count, &buffer);
    (void)solua_dsp_check(L, solua_dsp_api(L)->clip_s16(
        output, input, count, solua_dsp_i16(L, 2), solua_dsp_i16(L, 3)));
    luaL_pushresultsize(&buffer, count * sizeof(int16_t));
    return 1;
}

static int solua_dsp_level(lua_State *L)
{
    size_t count = 0;
    const int16_t *input = solua_dsp_s16_string(L, 1, &count);
    solar_os_dsp_level_t level;
    (void)solua_dsp_check(L, solua_dsp_api(L)->level_s16(input, count, &level));
    lua_pushinteger(L, level.peak);
    lua_pushinteger(L, level.rms);
    return 2;
}

static int solua_dsp_window(lua_State *L)
{
    size_t count = 0;
    size_t window_count = 0;
    const int16_t *input = solua_dsp_s16_string(L, 1, &count);
    const int16_t *window = solua_dsp_s16_string(L, 2, &window_count);
    if (window_count != count) {
        return luaL_error(L, "DSP strings must have equal length");
    }
    luaL_Buffer buffer;
    int16_t *output = solua_dsp_output(L, count, &buffer);
    (void)solua_dsp_check(L,
        solua_dsp_api(L)->window_q15(output, input, window, count));
    luaL_pushresultsize(&buffer, count * sizeof(int16_t));
    return 1;
}

static solua_dsp_processor_t *solua_dsp_self(lua_State *L)
{
    solua_dsp_processor_t *self =
        luaL_checkudata(L, 1, SOLUA_DSP_PROCESSOR_METATABLE);
    if (self->processor.context == 0) {
        luaL_error(L, "DSP processor is closed");
    }
    return self;
}

static int solua_dsp_destroy(solua_dsp_processor_t *self)
{
    if (!self || !self->processor.context) return SOLAR_OS_NATIVE_OK;
    int result;
    if (self->kind == SOLUA_DSP_FIR) result = self->api->fir_close(self->processor.fir);
    else if (self->kind == SOLUA_DSP_DECIMATOR) result = self->api->decimator_close(self->processor.decimator);
    else result = self->api->fft_close(self->processor.fft);
    if (result == SOLAR_OS_NATIVE_OK || result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) {
        self->processor.context = 0;
        return SOLAR_OS_NATIVE_OK;
    }
    return result;
}

static int solua_dsp_processor_close(lua_State *L)
{
    solua_dsp_processor_t *self =
        luaL_checkudata(L, 1, SOLUA_DSP_PROCESSOR_METATABLE);
    (void)solua_dsp_check(L, solua_dsp_destroy(self));
    return 0;
}

static int solua_dsp_processor_reset(lua_State *L)
{
    solua_dsp_processor_t *self = solua_dsp_self(L);
    if (self->kind == SOLUA_DSP_FIR) {
        (void)solua_dsp_check(L, self->api->fir_reset(self->processor.fir));
    } else if (self->kind == SOLUA_DSP_DECIMATOR) {
        (void)solua_dsp_check(L, self->api->decimator_reset(self->processor.decimator));
    } else {
        return luaL_error(L, "FFT processor has no streaming state");
    }
    return 0;
}

static int solua_dsp_processor_process(lua_State *L)
{
    solua_dsp_processor_t *self = solua_dsp_self(L);
    if (self->kind == SOLUA_DSP_FFT) {
        return luaL_error(L, "use execute() for FFT processors");
    }
    size_t count = 0;
    const int16_t *input = solua_dsp_s16_string(L, 2, &count);
    size_t output_count = count;
    if (self->kind == SOLUA_DSP_DECIMATOR) {
        output_count = self->api->decimator_output_count(
            self->processor.decimator, count);
    }
    luaL_Buffer buffer;
    int16_t *output = solua_dsp_output(L, output_count, &buffer);
    if (self->kind == SOLUA_DSP_FIR) {
        (void)solua_dsp_check(L, self->api->fir_process(
            self->processor.fir, output, input, count));
    } else {
        size_t produced = 0;
        (void)solua_dsp_check(L, self->api->decimator_process(
            self->processor.decimator, output, output_count, input, count,
            &produced));
        if (produced != output_count) {
            return luaL_error(L, "DSP decimator output count mismatch");
        }
    }
    luaL_pushresultsize(&buffer, output_count * sizeof(int16_t));
    return 1;
}

static int solua_dsp_processor_execute(lua_State *L)
{
    solua_dsp_processor_t *self = solua_dsp_self(L);
    if (self->kind != SOLUA_DSP_FFT) {
        return luaL_error(L, "execute() requires an FFT processor");
    }
    size_t count = 0;
    const int16_t *input = solua_dsp_s16_string(L, 2, &count);
    const size_t fft_size = self->api->fft_size(self->processor.fft);
    if (count != fft_size) {
        return luaL_error(L, "input length does not match FFT size");
    }
    luaL_Buffer buffer;
    int16_t *output = solua_dsp_output(L, fft_size * 2U, &buffer);
    uint8_t exponent = 0;
    (void)solua_dsp_check(L, self->api->fft_execute(
        self->processor.fft, (solar_os_dsp_complex_s16_t *)output, input,
        &exponent));
    luaL_pushresultsize(&buffer,
                        fft_size * sizeof(solar_os_dsp_complex_s16_t));
    lua_pushinteger(L, exponent);
    return 2;
}

static int solua_dsp_fir(lua_State *L)
{
    size_t taps = 0;
    const int16_t *coefficients = solua_dsp_s16_string(L, 1, &taps);
    const size_t decimation =
        lua_isnoneornil(L, 2) ? 1U : solua_check_size(L, 2);
    if (taps == 0U || taps > SOLAR_OS_DSP_MAX_TAPS) {
        return luaL_error(L, "invalid FIR coefficient count");
    }
    if (decimation == 0U || decimation > UINT16_MAX) {
        return luaL_error(L, "decimation must be 1..65535");
    }
    solua_dsp_processor_t *self =
        lua_newuserdatauv(L, sizeof(*self), 0);
    memset(self, 0, sizeof(*self));
    self->api = solua_dsp_api(L);
    int error;
    if (decimation == 1U) {
        self->kind = SOLUA_DSP_FIR;
        error = self->api->fir_create(coefficients, taps,
                                         &self->processor.fir);
    } else {
        self->kind = SOLUA_DSP_DECIMATOR;
        error = self->api->decimator_create(coefficients, taps, decimation,
                                               &self->processor.decimator);
    }
    (void)solua_dsp_check(L, error);
    luaL_setmetatable(L, SOLUA_DSP_PROCESSOR_METATABLE);
    return 1;
}

static int solua_dsp_fft(lua_State *L)
{
    solua_dsp_processor_t *self =
        lua_newuserdatauv(L, sizeof(*self), 0);
    memset(self, 0, sizeof(*self));
    self->api = solua_dsp_api(L);
    self->kind = SOLUA_DSP_FFT;
    (void)solua_dsp_check(L, self->api->fft_create(
        solua_check_size(L, 1), &self->processor.fft));
    luaL_setmetatable(L, SOLUA_DSP_PROCESSOR_METATABLE);
    return 1;
}

static void solua_dsp_register_type(lua_State *L)
{
    if (luaL_newmetatable(L, SOLUA_DSP_PROCESSOR_METATABLE)) {
        lua_pushcfunction(L, solua_dsp_processor_close);
        lua_setfield(L, -2, "__gc");
        lua_newtable(L);
        const int methods = lua_gettop(L);
        solua_set_func(L, methods, "process", solua_dsp_processor_process);
        solua_set_func(L, methods, "execute", solua_dsp_processor_execute);
        solua_set_func(L, methods, "reset", solua_dsp_processor_reset);
        solua_set_func(L, methods, "close", solua_dsp_processor_close);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}
