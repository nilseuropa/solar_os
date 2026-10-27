#if SOLAR_OS_PACKAGE_SERVICE_EMOTION
typedef struct { unsigned slot; uint32_t generation; } solua_emotion_t;
static bool solua_emotion_live(const solua_emotion_t *self)
{
    return solua_runtime_state && self->slot < 4 &&
        solua_runtime_state->emotion_generation[self->slot] == self->generation &&
        solua_runtime_state->emotions[self->slot];
}
static solar_os_emotion_t *solua_emotion_face(lua_State *L)
{
    solua_emotion_t *self = luaL_checkudata(L, 1, "solaros.emotion.face");
    if (!solua_emotion_live(self)) luaL_error(L, "emotion face is closed");
    return solua_runtime_state->emotions[self->slot];
}
static void solua_emotion_lock(void)
{
    while (__atomic_exchange_n(&solua_runtime_state->emotion_busy, true, __ATOMIC_ACQ_REL))
        vTaskDelay(1);
}
static bool solua_emotion_active(void)
{ return solua_runtime_state && __atomic_load_n(&solua_runtime_state->emotion_busy, __ATOMIC_ACQUIRE); }
static bool solua_emotion_drawing(void)
{
    return solua_runtime_state && __atomic_load_n(&solua_runtime_state->emotion_count, __ATOMIC_ACQUIRE) != 0;
}
#define SOLUA_EMOTION_CALL(expression) do { \
    solua_emotion_lock(); \
    esp_err_t emotion_error = (expression); \
    __atomic_store_n(&solua_runtime_state->emotion_busy, false, __ATOMIC_RELEASE); \
    solua_check_esp(L, emotion_error); \
} while (0)
static esp_err_t solua_emotion_mode(bool active)
{
    __atomic_store_n(&solua_runtime_state->emotion_ack, false, __ATOMIC_RELEASE);
    solua_event_t event = {.type = SOLUA_EVENT_EMOTION_MODE, .attr = active};
    if (!solua_send_event(&event)) return ESP_ERR_INVALID_STATE;
    while (!__atomic_load_n(&solua_runtime_state->emotion_ack, __ATOMIC_ACQUIRE)) {
        if (solua.stop_requested) return ESP_ERR_INVALID_STATE;
        vTaskDelay(1);
    }
    return ESP_OK;
}
static void solua_emotion_destroy(void)
{
    if (!solua_runtime_state) return;
    solua_emotion_lock();
    for (unsigned i = 0; i < 4; ++i)
        while (solar_os_emotion_close(&solua_runtime_state->emotions[i]) != ESP_OK)
            vTaskDelay(pdMS_TO_TICKS(20));
    __atomic_store_n(&solua_runtime_state->emotion_count, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&solua_runtime_state->emotion_busy, false, __ATOMIC_RELEASE);
}
static const char *solua_emotion_string(lua_State *L, int index)
{
    size_t length; const char *text = luaL_checklstring(L, index, &length);
    if (!length || length > 31 || memchr(text, 0, length)) luaL_error(L, "invalid emotion name");
    return text;
}
static uint32_t solua_emotion_ms(lua_State *L, int index, uint32_t fallback)
{
    lua_Integer ms = luaL_optinteger(L, index, fallback);
    if (ms < 0 || ms > 60000) luaL_error(L, "duration must be 0..60000 ms");
    return (uint32_t)ms;
}
static int solua_emotion_face_close(lua_State *L)
{
    solua_emotion_t *self = luaL_checkudata(L, 1, "solaros.emotion.face");
    if (solua_emotion_live(self)) {
        bool foreground = solar_os_emotion_uses_gfx(solua_runtime_state->emotions[self->slot], solua.session_gfx);
        SOLUA_EMOTION_CALL(solar_os_emotion_close(&solua_runtime_state->emotions[self->slot]));
        __atomic_sub_fetch(&solua_runtime_state->emotion_count, 1, __ATOMIC_RELEASE);
        if (foreground && !solua.stop_requested && solua.events) (void)solua_emotion_mode(false);
    }
    return 0;
}
static int solua_emotion_face_set(lua_State *L)
{
    solar_os_emotion_t *face = solua_emotion_face(L);
    const char *pose = solua_emotion_string(L, 2); uint32_t ms = solua_emotion_ms(L, 3, 200);
    SOLUA_EMOTION_CALL(solar_os_emotion_set(face, pose, ms)); return 0;
}
static int solua_emotion_face_react(lua_State *L)
{
    solar_os_emotion_t *face = solua_emotion_face(L);
    const char *pose = solua_emotion_string(L, 2);
    uint32_t hold = solua_emotion_ms(L, 3, 700), ms = solua_emotion_ms(L, 4, 120);
    const char *cue = lua_isnoneornil(L, 5) ? NULL : solua_emotion_string(L, 5);
    SOLUA_EMOTION_CALL(solar_os_emotion_react(face, pose, hold, ms, cue)); return 0;
}
static int solua_emotion_face_look(lua_State *L)
{
    solar_os_emotion_t *face = solua_emotion_face(L);
    double x = luaL_optnumber(L, 2, 0), y = luaL_optnumber(L, 3, 0);
    if (!isfinite(x) || !isfinite(y) || x < -1 || x > 1 || y < -1 || y > 1)
        return luaL_error(L, "gaze must be -1..1");
    SOLUA_EMOTION_CALL(solar_os_emotion_look(face, (int32_t)(x * 1000), (int32_t)(y * 1000))); return 0;
}
static int solua_emotion_face_blink(lua_State *L)
{
    solar_os_emotion_t *face = solua_emotion_face(L);
    SOLUA_EMOTION_CALL(solar_os_emotion_blink(face)); return 0;
}
static int solua_emotion_face_status(lua_State *L)
{
    solar_os_emotion_status_t status; solar_os_emotion_status(solua_emotion_face(L), &status);
    lua_newtable(L);
#define FIELD(name) solua_set_int(L, -1, #name, status.name)
    FIELD(frames); FIELD(dropped); FIELD(audio_skipped); FIELD(error); FIELD(audio_error);
    FIELD(fps); FIELD(width); FIELD(height);
#undef FIELD
    lua_pushboolean(L, status.sound); lua_setfield(L, -2, "sound");
    lua_pushboolean(L, status.motion); lua_setfield(L, -2, "motion");
    return 1;
}
static int solua_emotion_open(lua_State *L)
{
    if (lua_gettop(L) > 6) return luaL_error(L, "too many emotion arguments");
    solar_os_emotion_options_t options = {.motion = -1};
    if (!lua_isnoneornil(L, 1)) options.display = solua_emotion_string(L, 1);
    options.sound = lua_toboolean(L, 2);
    for (int i = 3; i <= 4; ++i) {
        if (lua_isnoneornil(L, i)) continue;
        lua_Integer color = luaL_checkinteger(L, i);
        if (color < 0 || color > 0xffffff) return luaL_error(L, "color must be 0x000000..0xFFFFFF");
        if (i == 3) { options.foreground_rgb888 = (uint32_t)color; options.foreground_set = true; }
        else { options.background_rgb888 = (uint32_t)color; options.background_set = true; }
    }
    lua_Integer fps = luaL_optinteger(L, 5, 0);
    if (fps < 0 || fps > 30) return luaL_error(L, "fps must be 0..30");
    options.fps = fps;
    if (!lua_isnoneornil(L, 6)) options.motion = lua_toboolean(L, 6);
    char current[SOLAR_OS_DISPLAY_TARGET_NAME_MAX] = {0};
    if (solua.session_gfx) solar_os_gfx_display_target_name(solua.session_gfx, current, sizeof(current));
    bool foreground = *current && (!options.display || !strcmp(options.display, current));
#if SOLAR_OS_PACKAGE_SERVICE_JET
    if (solua_jet_drawing()) return luaL_error(L, "close Jet scene before opening a face");
#endif
    if (solua.claimed_gfx || solua.tui_active) return luaL_error(L, "end graphics/TUI before opening a face");
    for (unsigned i = 0; i < 4; ++i)
        if (foreground && solar_os_emotion_uses_gfx(solua_runtime_state->emotions[i], solua.session_gfx))
            return luaL_error(L, "foreground already has a face");
    unsigned slot;
    for (slot = 0; slot < 4 && solua_runtime_state->emotions[slot]; ++slot) {}
    if (slot == 4) return luaL_error(L, "too many emotion instances");
    solua_emotion_t *self = lua_newuserdatauv(L, sizeof(*self), 0);
    self->slot = 4; self->generation = 0;
    if (luaL_newmetatable(L, "solaros.emotion.face")) {
        solua_set_func(L, -1, "set", solua_emotion_face_set);
        solua_set_func(L, -1, "react", solua_emotion_face_react);
        solua_set_func(L, -1, "look", solua_emotion_face_look);
        solua_set_func(L, -1, "blink", solua_emotion_face_blink);
        solua_set_func(L, -1, "status", solua_emotion_face_status);
        solua_set_func(L, -1, "close", solua_emotion_face_close);
        solua_set_func(L, -1, "__gc", solua_emotion_face_close);
        solua_set_func(L, -1, "__close", solua_emotion_face_close);
        lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
    }
    lua_setmetatable(L, -2);
    char detail[96] = {0};
    solua_emotion_lock();
    bool mode_started = foreground && !solua_runtime_state->emotion_suspended;
    esp_err_t err = foreground && !mode_started ? ESP_ERR_INVALID_STATE :
        (foreground ? solua_emotion_mode(true) : ESP_OK);
    if (err == ESP_OK) err = solar_os_emotion_open(&options, solua.session_gfx,
        &solua_runtime_state->emotions[slot], detail, sizeof(detail));
    if (solua_runtime_state->emotions[slot])
        __atomic_add_fetch(&solua_runtime_state->emotion_count, 1, __ATOMIC_RELEASE);
    if (err != ESP_OK && mode_started && !solua_runtime_state->emotions[slot]) (void)solua_emotion_mode(false);
    if (!++solua_runtime_state->emotion_generation[slot]) ++solua_runtime_state->emotion_generation[slot];
    self->slot = slot; self->generation = solua_runtime_state->emotion_generation[slot];
    __atomic_store_n(&solua_runtime_state->emotion_busy, false, __ATOMIC_RELEASE);
    if (err != ESP_OK) return luaL_error(L, "emotion: %s (%s)", detail, esp_err_to_name(err));
    return 1;
}
#undef SOLUA_EMOTION_CALL
#else
static bool solua_emotion_active(void) { return false; }
static bool solua_emotion_drawing(void) { return false; }
static void solua_emotion_destroy(void) {}
#endif
