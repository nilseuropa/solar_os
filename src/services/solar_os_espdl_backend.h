#pragma once
#include "solar_os_inference_backend.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t solar_os_espdl_backend_load_buffer(const uint8_t *data, size_t bytes,
    solar_os_inference_cancel_fn cancel, void *user,
    solar_os_inference_backend_t **backend, solar_os_inference_model_info_t *info);
esp_err_t solar_os_espdl_backend_load_owned_buffer(void *allocation, uint8_t *data, size_t bytes,
    solar_os_inference_cancel_fn cancel, void *user,
    solar_os_inference_backend_t **backend, solar_os_inference_model_info_t *info);
esp_err_t solar_os_espdl_backend_run(solar_os_inference_backend_t *backend,
    const solar_os_inference_input_t *inputs, size_t count,
    solar_os_inference_cancel_fn cancel, void *user, solar_os_inference_result_t *result);
void solar_os_espdl_backend_reset(solar_os_inference_backend_t *backend);
void solar_os_espdl_backend_set_mode(solar_os_inference_backend_t *backend, solar_os_inference_mode_t mode);
void solar_os_espdl_backend_close(solar_os_inference_backend_t *backend);
bool solar_os_espdl_backend_idle(void);
#ifdef __cplusplus
}
#endif
