/* Package-owned bindings to the OS HID output device. */
static const solar_os_native_hid_device_api_v1_t *python_hid_cached_api;
static const solar_os_native_hid_device_api_v1_t *python_hid_api(void)
{
    if (!python_hid_cached_api) {
        python_hid_cached_api = solar_os_native_hid_device_get(
            solar_os_native_host_v1()->get_service,
            SOLAR_OS_NATIVE_THROUGH(solar_os_native_hid_device_api_v1_t, release_all));
        if (!python_hid_cached_api) mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("HID service unavailable"));
    }
    return python_hid_cached_api;
}
static void python_hid_check(int result)
{ if (result) mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("HID error %d"), result); }
static void python_hid_cleanup(void)
{
    /* Cleanup cannot raise VM errors, including with an unavailable service.
     * Preserve the core's queued typing and neutralize every report type. */
    const solar_os_native_hid_device_api_v1_t *api = python_hid_cached_api;
    if (!api) api = solar_os_native_hid_device_get(solar_os_native_host_v1()->get_service,
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_hid_device_api_v1_t, release_all));
    if (api) api->release_all();
    python_hid_cached_api = NULL;
}

static mp_obj_t solaros_hid_status(void)
{
    solar_os_hid_status_t status;
    python_hid_api()->get_status(&status);
    mp_obj_t result = mp_obj_new_dict(2);
    python_dict_store_bool(result, "initialized", status.initialized);
    python_dict_store_bool(result, "connected", status.connected);
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_hid_status_obj, solaros_hid_status);

static size_t python_hid_keys(size_t n_args,
                              const mp_obj_t *args,
                              uint16_t *keys,
                              size_t capacity)
{
    if (n_args == 0 || n_args > capacity) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected 1..8 keys"));
    }
    for (size_t index = 0; index < n_args; index++) {
        const mp_int_t value = mp_obj_get_int(args[index]);
        if (value < 0 || value > UINT16_MAX) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid HID key"));
        }
        keys[index] = (uint16_t)value;
    }
    return n_args;
}

static mp_obj_t solaros_hid_keyboard_press(size_t n_args, const mp_obj_t *args)
{
    uint16_t keys[8];
    python_hid_check(python_hid_api()->keyboard_press(
        keys, python_hid_keys(n_args, args, keys, 8)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_hid_keyboard_press_obj,
                                    1,
                                    8,
                                    solaros_hid_keyboard_press);

static mp_obj_t solaros_hid_keyboard_release(size_t n_args, const mp_obj_t *args)
{
    uint16_t keys[8];
    python_hid_check(python_hid_api()->keyboard_release(
        keys, python_hid_keys(n_args, args, keys, 8)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_hid_keyboard_release_obj,
                                    1,
                                    8,
                                    solaros_hid_keyboard_release);

static mp_obj_t solaros_hid_keyboard_release_all(void)
{
    python_hid_check(python_hid_api()->keyboard_release_all());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_hid_keyboard_release_all_obj,
                          solaros_hid_keyboard_release_all);

static mp_obj_t solaros_hid_mouse_move(mp_obj_t x_obj, mp_obj_t y_obj)
{
    python_hid_check(python_hid_api()->mouse_move(python_i32_from_obj(x_obj),
                                              python_i32_from_obj(y_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_hid_mouse_move_obj, solaros_hid_mouse_move);

static mp_obj_t solaros_hid_mouse_button(mp_obj_t button_obj, mp_obj_t pressed_obj)
{
    python_hid_check(python_hid_api()->mouse_button(python_u8_from_obj(button_obj),
                                                mp_obj_is_true(pressed_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_hid_mouse_button_obj, solaros_hid_mouse_button);

static mp_obj_t solaros_hid_gamepad_axis(mp_obj_t axis_obj, mp_obj_t value_obj)
{
    const mp_int_t axis = mp_obj_get_int(axis_obj);
    if (axis < SOLAR_OS_HID_AXIS_X || axis >= SOLAR_OS_HID_AXIS_COUNT)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid gamepad axis"));
    const mp_int_t value = mp_obj_get_int(value_obj);
    if (value < INT16_MIN || value > INT16_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("expected axis value -32768..32767"));
    }
    python_hid_check(python_hid_api()->gamepad_axis((solar_os_hid_axis_t)axis,
                                                (int16_t)value));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_hid_gamepad_axis_obj, solaros_hid_gamepad_axis);

static mp_obj_t solaros_hid_gamepad_button(mp_obj_t button_obj, mp_obj_t pressed_obj)
{
    python_hid_check(python_hid_api()->gamepad_button(python_u8_from_obj(button_obj),
                                                  mp_obj_is_true(pressed_obj)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_hid_gamepad_button_obj, solaros_hid_gamepad_button);

static mp_obj_t solaros_hid_gamepad_hat(mp_obj_t hat_obj)
{
    const mp_int_t hat = mp_obj_get_int(hat_obj);
    if (hat < SOLAR_OS_HID_HAT_CENTERED || hat > SOLAR_OS_HID_HAT_UP_LEFT)
        mp_raise_ValueError(MP_ERROR_TEXT("invalid gamepad hat"));
    python_hid_check(python_hid_api()->gamepad_hat((solar_os_hid_hat_t)hat));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_hid_gamepad_hat_obj, solaros_hid_gamepad_hat);

static mp_obj_t solaros_hid_gamepad_send(void)
{
    python_hid_check(python_hid_api()->gamepad_send());
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_hid_gamepad_send_obj, solaros_hid_gamepad_send);
