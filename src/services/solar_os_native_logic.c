#include "solar_os_native_logic.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_LOGIC
#include "solar_os_logic.h"
#include "solar_os_native_support.h"

static int logic_result(esp_err_t error)
{
    if (error == ESP_ERR_NOT_FINISHED) return SOLAR_OS_NATIVE_ERROR_CANCELLED;
    if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
    return solar_os_native_result(error);
}

static int defaults(solar_os_logic_config_t *config)
{ return logic_result(solar_os_logic_default_config(config)); }

static int validate(const solar_os_logic_config_t *config)
{ return logic_result(solar_os_logic_validate_config(config)); }

static int capture(const solar_os_logic_config_t *config,
    const solar_os_logic_capture_options_v1_t *options, uint32_t *generation)
{ return logic_result(solar_os_logic_capture_with_options(config, options, generation)); }

static int status(solar_os_logic_status_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_logic_status_t){0};
    const esp_err_t error = solar_os_logic_get_status(out);
    if (error == ESP_OK) out->last_error = logic_result(out->last_error);
    return logic_result(error);
}

static int read_samples(uint32_t generation, size_t start, uint8_t *data, size_t size, size_t *count)
{ return logic_result(solar_os_logic_copy_generation(generation, start, data, size, count)); }

static const solar_os_native_logic_api_v1_t logic_api = {
    .abi_version = SOLAR_OS_NATIVE_LOGIC_ABI, .struct_size = sizeof(logic_api),
    .defaults = defaults, .validate = validate, .capture = capture, .status = status, .read = read_samples,
};
#endif

const void *solar_os_native_logic_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_logic_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
