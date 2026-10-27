#include "solar_os_espdl_backend.h"
#include "solar_os_inference_backend_abi.h"
extern "C" {
#include "solar_os_memory.h"
}
#ifdef SOLAR_OS_INFERENCE_EXTERNAL
extern "C" void solar_os_espdl_initialize(void);
extern "C" void solar_os_espdl_port_leave_task(void);
#else
static void solar_os_espdl_initialize() {}
static void solar_os_espdl_port_leave_task() {}
#endif
static bool never_cancel(void *) { return false; }
static int result(esp_err_t error)
{
    switch (error) {
    case ESP_OK: return SOLAR_OS_NATIVE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    case ESP_ERR_INVALID_SIZE: return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    default: return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
}
static int load(const uint8_t *data, size_t bytes, solar_os_inference_cancel_fn cancel,
    void *user, void **out, solar_os_inference_model_info_t *info)
{
    if (!out || !info) return -2;
    *out = nullptr;
    solar_os_espdl_initialize();
    solar_os_inference_backend_t *backend = nullptr;
    int error = result(solar_os_espdl_backend_load_buffer(data, bytes,
        cancel ? cancel : never_cancel, user, &backend, info));
    *out = backend;
    solar_os_espdl_port_leave_task();
    return error;
}
static int run(void *backend, const solar_os_inference_input_t *inputs, size_t count,
    solar_os_inference_cancel_fn cancel, void *user, solar_os_inference_result_t *out)
{
    if (!backend || !inputs || !count || !out) return -2;
    int error = result(solar_os_espdl_backend_run(static_cast<solar_os_inference_backend_t *>(backend),
        inputs, count, cancel ? cancel : never_cancel, user, out));
    solar_os_espdl_port_leave_task();
    return error;
}
static int load_owned(void *allocation, uint8_t *data, size_t bytes,
    solar_os_inference_cancel_fn cancel, void *user, void **out,
    solar_os_inference_model_info_t *info)
{
    if (!out || !info) { solar_os_memory_free(allocation); return -2; }
    *out = nullptr;
    solar_os_espdl_initialize();
    solar_os_inference_backend_t *backend = nullptr;
    int error = result(solar_os_espdl_backend_load_owned_buffer(allocation, data, bytes,
        cancel ? cancel : never_cancel, user, &backend, info));
    *out = backend; solar_os_espdl_port_leave_task(); return error;
}
static int reset(void *backend)
{
    if (!backend) return -2;
    solar_os_espdl_backend_reset(static_cast<solar_os_inference_backend_t *>(backend));
    solar_os_espdl_port_leave_task(); return 0;
}
static int set_mode(void *backend, solar_os_inference_mode_t mode)
{
    if (!backend || (unsigned)mode > SOLAR_OS_INFERENCE_DUAL) return -2;
    solar_os_espdl_backend_set_mode(static_cast<solar_os_inference_backend_t *>(backend), mode);
    solar_os_espdl_port_leave_task(); return 0;
}
static int close(void *backend)
{
    if (!backend) return -2;
    solar_os_espdl_backend_close(static_cast<solar_os_inference_backend_t *>(backend));
    solar_os_espdl_port_leave_task(); return 0;
}
extern "C" const solar_os_inference_backend_api_v1_t *solar_os_inference_espdl_api(void)
{
    static const solar_os_inference_backend_api_v1_t api = {1, sizeof(api),
        SOLAR_OS_INFERENCE_BACKEND_VERSION, load, run, reset, set_mode, close, load_owned};
    return &api;
}
