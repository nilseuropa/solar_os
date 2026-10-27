#if SOLAR_OS_PACKAGE_SERVICE_EMOTION
typedef struct { mp_obj_base_t base; unsigned slot; uint32_t generation; } python_emotion_t;
static const mp_obj_type_t solaros_emotion_face_type;
static python_emotion_t *python_emotion_object(mp_obj_t object)
{
    if (!mp_obj_is_type(object, &solaros_emotion_face_type))
        mp_raise_TypeError(MP_ERROR_TEXT("expected an emotion face"));
    return MP_OBJ_TO_PTR(object);
}
static bool python_emotion_live(const python_emotion_t *self)
{
    return python_runtime_state && self->slot < 4 &&
        python_runtime_state->emotion_generation[self->slot] == self->generation &&
        python_runtime_state->emotions[self->slot];
}
static solar_os_emotion_t *python_emotion_face(mp_obj_t object)
{
    python_emotion_t *self = python_emotion_object(object);
    if (!python_emotion_live(self)) mp_raise_ValueError(MP_ERROR_TEXT("emotion face is closed"));
    return python_runtime_state->emotions[self->slot];
}
static void python_emotion_lock(void)
{
    while (__atomic_exchange_n(&python_runtime_state->emotion_busy, true, __ATOMIC_ACQ_REL))
        vTaskDelay(1);
}
static bool python_emotion_active(void)
{ return python_runtime_state && __atomic_load_n(&python_runtime_state->emotion_busy, __ATOMIC_ACQUIRE); }
static bool python_emotion_drawing(void)
{
    return python_runtime_state && __atomic_load_n(&python_runtime_state->emotion_count, __ATOMIC_ACQUIRE) != 0;
}
#define PYTHON_EMOTION_CALL(expression) do { \
    python_emotion_lock(); \
    esp_err_t emotion_error = (expression); \
    __atomic_store_n(&python_runtime_state->emotion_busy, false, __ATOMIC_RELEASE); \
    python_check_esp(emotion_error); \
} while (0)
static esp_err_t python_emotion_mode(bool active)
{
    __atomic_store_n(&python_runtime_state->emotion_ack, false, __ATOMIC_RELEASE);
    python_event_t event = {.type = PYTHON_EVENT_EMOTION_MODE, .attr = active};
    if (!python_send_event(&event)) return ESP_ERR_INVALID_STATE;
    while (!__atomic_load_n(&python_runtime_state->emotion_ack, __ATOMIC_ACQUIRE)) {
        if (python_app.stop_requested) return ESP_ERR_INVALID_STATE;
        vTaskDelay(1);
    }
    return ESP_OK;
}
static void python_emotion_destroy(void)
{
    if (!python_runtime_state) return;
    python_emotion_lock();
    for (unsigned i = 0; i < 4; ++i) {
        while (solar_os_emotion_close(&python_runtime_state->emotions[i]) != ESP_OK)
            vTaskDelay(pdMS_TO_TICKS(20));
    }
    __atomic_store_n(&python_runtime_state->emotion_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&python_runtime_state->emotion_busy, false, __ATOMIC_RELEASE);
}
static void python_emotion_args(size_t count, const mp_obj_t *args, mp_map_t *kwargs,
    const char *const *names, unsigned size, mp_obj_t *values)
{
    if (count > size) mp_raise_TypeError(MP_ERROR_TEXT("too many emotion arguments"));
    if (kwargs) for (size_t i = 0; i < kwargs->alloc; ++i) {
        if (!mp_map_slot_is_filled(kwargs, i)) continue;
        const char *key = mp_obj_str_get_str(kwargs->table[i].key);
        unsigned n = 0;
        for (; n < size && strcmp(key, names[n]); ++n) {}
        if (n == size) mp_raise_TypeError(MP_ERROR_TEXT("unknown emotion argument"));
    }
    for (unsigned i = 0; i < size; ++i) {
        values[i] = python_kw_value(kwargs, names[i]);
        if (i < count) {
            if (values[i] != MP_OBJ_NULL) mp_raise_TypeError(MP_ERROR_TEXT("duplicate emotion argument"));
            values[i] = args[i];
        }
    }
}
static const char *python_emotion_string(mp_obj_t value)
{
    size_t length;
    const char *text = mp_obj_str_get_data(value, &length);
    if (!length || length > 31 || memchr(text, 0, length))
        mp_raise_ValueError(MP_ERROR_TEXT("invalid emotion name"));
    return text;
}
static uint32_t python_emotion_ms(mp_obj_t value, uint32_t fallback)
{
    uint32_t ms = value == MP_OBJ_NULL ? fallback : python_u32_from_obj(value);
    if (ms > 60000) mp_raise_ValueError(MP_ERROR_TEXT("duration must be 0..60000 ms"));
    return ms;
}
static uint32_t python_emotion_color(mp_obj_t value)
{
    const mp_int_t color = mp_obj_get_int(value);
    if (color < 0 || (uint64_t)color > 0xffffffU)
        mp_raise_ValueError(MP_ERROR_TEXT("color must be 0x000000..0xFFFFFF"));
    return (uint32_t)color;
}
static mp_obj_t solaros_emotion_face_close(mp_obj_t object)
{
    python_emotion_t *self = python_emotion_object(object);
    if (python_emotion_live(self)) {
        bool foreground = solar_os_emotion_uses_gfx(python_runtime_state->emotions[self->slot], python_app.session_gfx);
        PYTHON_EMOTION_CALL(solar_os_emotion_close(&python_runtime_state->emotions[self->slot]));
        __atomic_sub_fetch(&python_runtime_state->emotion_count, 1, __ATOMIC_RELEASE);
        if (foreground && !python_app.stop_requested && python_app.events) (void)python_emotion_mode(false);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_emotion_face_close_obj, solaros_emotion_face_close);
static mp_obj_t solaros_emotion_face_set(size_t count, const mp_obj_t *args, mp_map_t *kwargs)
{
    solar_os_emotion_t *face = python_emotion_face(args[0]);
    const char *names[] = {"pose", "transition_ms"}; mp_obj_t v[2];
    python_emotion_args(count - 1, args + 1, kwargs, names, 2, v);
    if (v[0] == MP_OBJ_NULL) mp_raise_TypeError(MP_ERROR_TEXT("pose is required"));
    const char *pose = python_emotion_string(v[0]); uint32_t ms = python_emotion_ms(v[1], 200);
    PYTHON_EMOTION_CALL(solar_os_emotion_set(face, pose, ms)); return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_emotion_face_set_obj, 1, solaros_emotion_face_set);
static mp_obj_t solaros_emotion_face_react(size_t count, const mp_obj_t *args, mp_map_t *kwargs)
{
    solar_os_emotion_t *face = python_emotion_face(args[0]);
    const char *names[] = {"pose", "hold_ms", "transition_ms", "sound"}; mp_obj_t v[4];
    python_emotion_args(count - 1, args + 1, kwargs, names, 4, v);
    if (v[0] == MP_OBJ_NULL) mp_raise_TypeError(MP_ERROR_TEXT("pose is required"));
    const char *pose = python_emotion_string(v[0]);
    uint32_t hold = python_emotion_ms(v[1], 700), ms = python_emotion_ms(v[2], 120);
    const char *cue = v[3] == MP_OBJ_NULL || v[3] == mp_const_none ? NULL : python_emotion_string(v[3]);
    PYTHON_EMOTION_CALL(solar_os_emotion_react(face, pose, hold, ms, cue)); return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_emotion_face_react_obj, 1, solaros_emotion_face_react);
static mp_obj_t solaros_emotion_face_look(size_t count, const mp_obj_t *args, mp_map_t *kwargs)
{
    solar_os_emotion_t *face = python_emotion_face(args[0]);
    const char *names[] = {"x", "y"}; mp_obj_t v[2];
    python_emotion_args(count - 1, args + 1, kwargs, names, 2, v);
    float x = v[0] == MP_OBJ_NULL ? 0 : mp_obj_get_float(v[0]);
    float y = v[1] == MP_OBJ_NULL ? 0 : mp_obj_get_float(v[1]);
    if (!isfinite(x) || !isfinite(y) || x < -1 || x > 1 || y < -1 || y > 1)
        mp_raise_ValueError(MP_ERROR_TEXT("gaze must be -1..1"));
    PYTHON_EMOTION_CALL(solar_os_emotion_look(face, (int32_t)(x * 1000), (int32_t)(y * 1000)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_emotion_face_look_obj, 1, solaros_emotion_face_look);
static mp_obj_t solaros_emotion_face_blink(mp_obj_t object)
{
    solar_os_emotion_t *face = python_emotion_face(object);
    PYTHON_EMOTION_CALL(solar_os_emotion_blink(face)); return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_emotion_face_blink_obj, solaros_emotion_face_blink);
static mp_obj_t solaros_emotion_face_status(mp_obj_t object)
{
    solar_os_emotion_status_t status;
    solar_os_emotion_status(python_emotion_face(object), &status);
    mp_obj_t dict = mp_obj_new_dict(11);
#define FIELD(name) python_dict_store_uint(dict, #name, status.name)
    FIELD(frames); FIELD(dropped); FIELD(audio_skipped); FIELD(fps); FIELD(width); FIELD(height);
#undef FIELD
    python_dict_store_int(dict, "error", status.error);
    python_dict_store_int(dict, "audio_error", status.audio_error);
    python_dict_store_bool(dict, "sound", status.sound);
    python_dict_store_bool(dict, "motion", status.motion);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_emotion_face_status_obj, solaros_emotion_face_status);
static mp_obj_t solaros_emotion_face_enter(mp_obj_t object)
{ (void)python_emotion_face(object); return object; }
MP_DEFINE_CONST_FUN_OBJ_1(solaros_emotion_face_enter_obj, solaros_emotion_face_enter);
static mp_obj_t solaros_emotion_face_exit(size_t count, const mp_obj_t *args)
{ (void)count; return solaros_emotion_face_close(args[0]); }
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_emotion_face_exit_obj, 4, 4, solaros_emotion_face_exit);
static const mp_rom_map_elem_t solaros_emotion_face_locals_table[] = {
    {MP_ROM_QSTR(MP_QSTR_set), MP_ROM_PTR(&solaros_emotion_face_set_obj)},
    {MP_ROM_QSTR(MP_QSTR_react), MP_ROM_PTR(&solaros_emotion_face_react_obj)},
    {MP_ROM_QSTR(MP_QSTR_look), MP_ROM_PTR(&solaros_emotion_face_look_obj)},
    {MP_ROM_QSTR(MP_QSTR_blink), MP_ROM_PTR(&solaros_emotion_face_blink_obj)},
    {MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&solaros_emotion_face_status_obj)},
    {MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&solaros_emotion_face_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&solaros_emotion_face_close_obj)},
    {MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&solaros_emotion_face_enter_obj)},
    {MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&solaros_emotion_face_exit_obj)},
};
static MP_DEFINE_CONST_DICT(solaros_emotion_face_locals, solaros_emotion_face_locals_table);
static MP_DEFINE_CONST_OBJ_TYPE(solaros_emotion_face_type, MP_QSTR_emotion_face,
    MP_TYPE_FLAG_NONE, locals_dict, &solaros_emotion_face_locals);
static mp_obj_t solaros_emotion_open(size_t count, const mp_obj_t *args, mp_map_t *kwargs)
{
    const char *names[] = {"display", "sound", "fg", "bg", "fps", "motion"}; mp_obj_t v[6];
    python_emotion_args(count, args, kwargs, names, 6, v);
    solar_os_emotion_options_t options = {.motion = -1};
    if (v[0] != MP_OBJ_NULL && v[0] != mp_const_none) options.display = python_emotion_string(v[0]);
    if (v[1] != MP_OBJ_NULL) options.sound = mp_obj_is_true(v[1]);
    if (v[2] != MP_OBJ_NULL && v[2] != mp_const_none) {
        options.foreground_rgb888 = python_emotion_color(v[2]); options.foreground_set = true;
    }
    if (v[3] != MP_OBJ_NULL && v[3] != mp_const_none) {
        options.background_rgb888 = python_emotion_color(v[3]); options.background_set = true;
    }
    if (v[4] != MP_OBJ_NULL) {
        uint32_t fps = python_u32_from_obj(v[4]);
        if (fps > 30) mp_raise_ValueError(MP_ERROR_TEXT("fps must be 0..30"));
        options.fps = fps;
    }
    if (v[5] != MP_OBJ_NULL && v[5] != mp_const_none) options.motion = mp_obj_is_true(v[5]);
    char current[SOLAR_OS_DISPLAY_TARGET_NAME_MAX] = {0};
    if (python_app.session_gfx) solar_os_gfx_display_target_name(python_app.session_gfx, current, sizeof(current));
    bool foreground = *current && (!options.display || !strcmp(options.display, current));
#if SOLAR_OS_PACKAGE_SERVICE_JET
    if (python_jet_drawing()) mp_raise_ValueError(MP_ERROR_TEXT("close Jet scene before opening a face"));
#endif
    if (python_app.claimed_gfx || python_app.tui_active)
        mp_raise_ValueError(MP_ERROR_TEXT("end graphics/TUI before opening a face"));
    for (unsigned i = 0; i < 4; ++i)
        if (foreground && solar_os_emotion_uses_gfx(python_runtime_state->emotions[i], python_app.session_gfx))
            mp_raise_ValueError(MP_ERROR_TEXT("foreground already has a face"));
    unsigned slot;
    for (slot = 0; slot < 4 && python_runtime_state->emotions[slot]; ++slot) {}
    if (slot == 4) mp_raise_ValueError(MP_ERROR_TEXT("too many emotion instances"));
    python_emotion_t *self = mp_obj_malloc_with_finaliser(python_emotion_t, &solaros_emotion_face_type);
    self->slot = 4; self->generation = 0;
    char detail[96] = {0};
    python_emotion_lock();
    bool mode_started = foreground && !python_runtime_state->emotion_suspended;
    esp_err_t err = foreground && !mode_started ? ESP_ERR_INVALID_STATE :
        (foreground ? python_emotion_mode(true) : ESP_OK);
    if (err == ESP_OK) err = solar_os_emotion_open(&options, python_app.session_gfx,
        &python_runtime_state->emotions[slot], detail, sizeof(detail));
    if (python_runtime_state->emotions[slot])
        __atomic_add_fetch(&python_runtime_state->emotion_count, 1, __ATOMIC_RELEASE);
    if (err != ESP_OK && mode_started && !python_runtime_state->emotions[slot]) (void)python_emotion_mode(false);
    if (!++python_runtime_state->emotion_generation[slot]) ++python_runtime_state->emotion_generation[slot];
    self->slot = slot; self->generation = python_runtime_state->emotion_generation[slot];
    __atomic_store_n(&python_runtime_state->emotion_busy, false, __ATOMIC_RELEASE);
    if (err != ESP_OK) mp_raise_msg_varg(&mp_type_RuntimeError,
        MP_ERROR_TEXT("emotion: %s (%s)"), detail, esp_err_to_name(err));
    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(solaros_emotion_open_obj, 0, solaros_emotion_open);
#undef PYTHON_EMOTION_CALL
#else
static bool python_emotion_active(void) { return false; }
static bool python_emotion_drawing(void) { return false; }
static void python_emotion_destroy(void) {}
#endif
