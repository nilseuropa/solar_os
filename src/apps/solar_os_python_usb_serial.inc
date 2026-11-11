#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
#include "solar_os_usb_serial.h"

static mp_obj_t solaros_usb_serial_configure(size_t n, const mp_obj_t *args)
{
    solar_os_port_serial_config_t config = {0};
    uint32_t mask = 0;
    if (n > 1) {
        if (!mp_obj_is_type(args[1], &mp_type_dict)) mp_raise_TypeError(MP_ERROR_TEXT("expected dict"));
        mp_obj_dict_t *dict = MP_OBJ_TO_PTR(args[1]);
        for (size_t i = 0; i < dict->map.alloc; i++) {
            if (!mp_map_slot_is_filled(&dict->map, i)) continue;
            const char *key = mp_obj_str_get_str(dict->map.table[i].key);
            mp_obj_t value = dict->map.table[i].value;
            char number[16];
            const char *text;
            if (mp_obj_is_str(value)) text = mp_obj_str_get_str(value);
            else {
                uint32_t integer = value == mp_const_true ? 1 : value == mp_const_false ? 0 : python_u32_from_obj(value);
                snprintf(number, sizeof(number), "%lu", (unsigned long)integer); text = number;
            }
            python_check_esp(solar_os_usb_serial_parse_option(key, text, &config, &mask));
        }
    }
    python_check_esp(solar_os_usb_serial_config(mp_obj_str_get_str(args[0]), &config, mask));
    mp_obj_t result = mp_obj_new_dict(0);
    python_dict_store_int(result, "baud", config.baud_rate);
    python_dict_store_int(result, "bits", config.data_bits);
    static const char *parity[] = {"none", "odd", "even", "mark", "space"};
    static const char *stop[] = {"1", "1.5", "2"};
    python_dict_store_cstr(result, "parity", parity[config.parity]);
    python_dict_store_cstr(result, "stop", stop[config.stop_bits]);
    mp_obj_dict_store(result, mp_obj_new_str("dtr", 3), mp_obj_new_bool(config.dtr));
    mp_obj_dict_store(result, mp_obj_new_str("rts", 3), mp_obj_new_bool(config.rts));
    return result;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_usb_serial_configure_obj, 1, 2, solaros_usb_serial_configure);

static mp_obj_t solaros_usb_serial_send_break(mp_obj_t name, mp_obj_t duration)
{
    const uint32_t value = python_u32_from_obj(duration);
    if (value > UINT16_MAX) mp_raise_ValueError(MP_ERROR_TEXT("break duration out of range"));
    python_check_esp(solar_os_usb_serial_send_break(mp_obj_str_get_str(name), value));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_2(solaros_usb_serial_send_break_obj, solaros_usb_serial_send_break);
#endif
