#pragma once
#include "solar_os_inference.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct solar_os_inference_backend solar_os_inference_backend_t;
esp_err_t solar_os_inference_backend_load(const char *path,
    solar_os_inference_cancel_fn cancel, void *user,
    solar_os_inference_backend_t **backend, solar_os_inference_model_info_t *info);
esp_err_t solar_os_inference_backend_run(solar_os_inference_backend_t *backend,
    const solar_os_inference_input_t *inputs, size_t count,
    solar_os_inference_cancel_fn cancel, void *user,
    solar_os_inference_result_t *result);
void solar_os_inference_backend_reset(solar_os_inference_backend_t *backend);
void solar_os_inference_backend_set_mode(solar_os_inference_backend_t *backend,
                                       solar_os_inference_mode_t mode);
void solar_os_inference_backend_close(solar_os_inference_backend_t *backend);
#ifdef __cplusplus
}
#endif
