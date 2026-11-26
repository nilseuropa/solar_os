#include "solar_os_native_expansion.h"

#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_expansion_abi.h"

#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
#include "solar_os_expansion.h"
#include "solar_os_native_support.h"

static int expansion_attach(const char *driver, const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    return solar_os_native_result(solar_os_expansion_attach(driver, name, bindings, count));
}

static int expansion_attach_board(const char *name)
{
    return solar_os_native_result(solar_os_expansion_attach_board_device(name));
}

static int expansion_detach(const char *name)
{
    return solar_os_native_result(solar_os_expansion_detach(name));
}

static int expansion_set_ready(const char *name, bool ready)
{
    return solar_os_native_result(solar_os_expansion_device_set_ready(name, ready));
}

static int expansion_validate(const char *driver, const solar_os_expansion_binding_t *bindings,
    size_t count, solar_os_expansion_binding_validation_t *validation)
{
    return solar_os_native_result(solar_os_expansion_validate_bindings(driver, bindings, count, validation));
}

static bool stream_binding_valid(solar_os_expansion_binding_kind_t kind, const char *id)
{
    solar_os_stream_info_t info;
    return id && solar_os_stream_get_info(id, &info) == ESP_OK && solar_os_expansion_stream_binding_valid(kind, &info);
}

static const char *expansion_label(uint32_t field, uint32_t value)
{
    switch (field) {
    case SOLAR_OS_NATIVE_EXPANSION_LABEL_BINDING:
        return value <= SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET ? solar_os_expansion_binding_kind_name(value) : NULL;
    case SOLAR_OS_NATIVE_EXPANSION_LABEL_CATEGORY:
        return value < SOLAR_OS_EXPANSION_CATEGORY_COUNT ? solar_os_expansion_category_name(value) : NULL;
    case SOLAR_OS_NATIVE_EXPANSION_LABEL_ORIGIN:
        return value <= SOLAR_OS_EXPANSION_ORIGIN_RUNTIME ? solar_os_expansion_origin_name(value) : NULL;
    default:
        return NULL;
    }
}

static int driver_info(size_t index, solar_os_expansion_driver_info_t *info)
{
    return solar_os_native_result(solar_os_expansion_get_driver_info(index, info));
}

static int driver_find(const char *name, solar_os_expansion_driver_info_t *info)
{
    return solar_os_native_result(solar_os_expansion_find_driver_info(name, info));
}

static int driver_binding(const char *name, size_t index, solar_os_expansion_binding_spec_info_t *spec,
    int *values, size_t capacity)
{
    return solar_os_native_result(solar_os_expansion_get_binding_spec(name, index, spec, values, capacity));
}

static const solar_os_native_expansion_api_v1_t expansion_api = {
    .abi_version = SOLAR_OS_NATIVE_EXPANSION_ABI, .struct_size = sizeof(expansion_api),
    .available = solar_os_expansion_available, .device_count = solar_os_expansion_device_count,
    .device_info = solar_os_expansion_get_device, .attach = expansion_attach,
    .attach_board_device = expansion_attach_board, .detach = expansion_detach, .set_ready = expansion_set_ready,
    .driver_count = solar_os_expansion_driver_count, .driver_supported = solar_os_expansion_driver_supported,
    .validate = expansion_validate, .binding_pin_supported = solar_os_expansion_binding_pin_supported,
    .stream_binding_valid = stream_binding_valid, .label = expansion_label,
    .driver_info = driver_info, .driver_find = driver_find, .driver_binding = driver_binding,
};
#endif

const void *solar_os_native_expansion_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_expansion_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
