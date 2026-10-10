#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "solar_os_inference_types_v1.h"

const char *solar_os_tensor_dtype_name(solar_os_tensor_dtype_t dtype);
esp_err_t solar_os_tensor_dtype_parse(const char *name, solar_os_tensor_dtype_t *dtype);
size_t solar_os_tensor_dtype_bytes(solar_os_tensor_dtype_t dtype);
const char *solar_os_inference_mode_name(solar_os_inference_mode_t mode);
esp_err_t solar_os_inference_mode_parse(const char *name, solar_os_inference_mode_t *mode);

/* The OS owns up to four resident models globally. Clients own only cancellation
 * and metadata snapshots; destroying a client never unloads models. Handles never
 * recycle during a boot. One native operation globally at a time; competing work
 * is busy. Calls on a client must be serialized by its owner. Cancellation/deadlines
 * join the worker before returning; they are cooperative, not hard preemption.
 * Loading takes a resolved SolarOS storage path, with no zoo/catalog required. */
esp_err_t solar_os_inference_create(solar_os_inference_cancel_fn cancel, void *user,
                                   solar_os_inference_t **client);
void solar_os_inference_destroy(solar_os_inference_t *client);
esp_err_t solar_os_inference_load(solar_os_inference_t *client, const char *path,
                                 uint32_t timeout_ms, uint32_t *handle);
/* Client-owned immutable snapshot, valid until its next successful info call or destruction.
 * It survives another client's unloading/changing the actual model. */
esp_err_t solar_os_inference_info(solar_os_inference_t *client, uint32_t handle,
                                 const solar_os_inference_model_info_t **info);
esp_err_t solar_os_inference_run(solar_os_inference_t *client, uint32_t handle,
    const solar_os_inference_input_t *inputs, size_t count, uint32_t timeout_ms,
    solar_os_inference_result_t **result);
esp_err_t solar_os_inference_reset(solar_os_inference_t *client, uint32_t handle);
/* Select execution mode without reloading the model. Default is single. */
esp_err_t solar_os_inference_set_mode(solar_os_inference_t *client, uint32_t handle,
                                    solar_os_inference_mode_t mode);
esp_err_t solar_os_inference_close(solar_os_inference_t *client, uint32_t handle);
/* Explicit global unload. All-or-nothing: referenced models make close_all busy. */
esp_err_t solar_os_inference_close_all(solar_os_inference_t *client);
esp_err_t solar_os_inference_list(uint32_t *handles, size_t capacity, size_t *count);
esp_err_t solar_os_inference_find(const char *resolved_path, uint32_t *handle);
/* Jobs retain a model while using it, release on stop, then it can be unloaded.
 * References are independent of client lifetimes. */
esp_err_t solar_os_inference_retain(uint32_t handle);
esp_err_t solar_os_inference_release(uint32_t handle);
void solar_os_inference_result_free(solar_os_inference_result_t *result);

#ifdef __cplusplus
}
#endif
