#include "solar_os_native_controllers.h"
#include "solar_os_controllers.h"
#include "esp_timer.h"
#include <string.h>

static int result(esp_err_t e)
{
    switch(e) {
    case ESP_OK: return SOLAR_OS_NATIVE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    default: return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
}
static int open_controller(const char *name, const char *owner,
    const solar_os_native_controller_config_v1_t *c, uint32_t *id)
{ return result(solar_os_controller_open_owned(name,owner,c,id)); }
static int evaluate(uint32_t id, const solar_os_native_controller_sample_v1_t *s,
    solar_os_native_controller_output_v1_t *out)
{ return result(solar_os_controller_evaluate_owned(id,s,(uint64_t)esp_timer_get_time(),out)); }
static int pause_controller(uint32_t id)
{ return result(solar_os_controller_pause_owned(id)); }
static int fault_controller(uint32_t id, int error)
{
    esp_err_t e;
    switch(error) {
    case SOLAR_OS_NATIVE_OK: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: e=ESP_ERR_TIMEOUT; break;
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: e=ESP_ERR_INVALID_ARG; break;
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: e=ESP_ERR_NOT_FOUND; break;
    case SOLAR_OS_NATIVE_ERROR_INVALID_STATE: e=ESP_ERR_INVALID_STATE; break;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: e=ESP_ERR_NO_MEM; break;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: e=ESP_ERR_NOT_SUPPORTED; break;
    default: e=ESP_FAIL; break;
    }
    return result(solar_os_controller_fault_owned(id,e));
}
static int close_controller(uint32_t id)
{ return result(solar_os_controller_close_owned(id)); }
static const solar_os_native_controllers_api_v1_t api={
    .abi_version=SOLAR_OS_NATIVE_CONTROLLERS_ABI,.struct_size=sizeof(api),
    .open=open_controller,.evaluate=evaluate,.pause=pause_controller,.fault=fault_controller,.close=close_controller};
const void *solar_os_native_controllers_get_service(const char *name, uint32_t abi, uint32_t size)
{ return name && !strcmp(name,SOLAR_OS_NATIVE_CONTROLLERS_SERVICE) && abi==SOLAR_OS_NATIVE_CONTROLLERS_ABI && size<=sizeof(api)?&api:NULL; }
