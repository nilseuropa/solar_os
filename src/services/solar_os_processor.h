#pragma once
#include <stdbool.h>
#include "solar_os_raster_image.h"
#include "solar_os_media_sample.h"
#include "solar_os_native_processor_abi.h"
#include "solar_os_processor_provider_abi.h"

typedef struct solar_os_processor solar_os_processor_t;
typedef bool (*solar_os_processor_cancel_fn)(void *user);
bool solar_os_processor_kind_valid(const char *kind);
/* Owned capability/model metadata from a built-in or installed provider. */
esp_err_t solar_os_processor_query(const char *kind, const char *source,
    const char *query, char **json);
esp_err_t solar_os_processor_select(const char *kind, const char *source, uint32_t slot,
    solar_os_processor_cancel_fn cancel, void *user, char **json);
esp_err_t solar_os_processor_upload(const char *kind, const char *source, const char *bundle,
    uint32_t slot, uint32_t flags, solar_os_processor_cancel_fn cancel,
    solar_os_processor_progress_fn progress, void *user, char **json);

/* An OS-owned native processor. Model processors retain a resident bundle;
 * destruction releases the reference, never unloads the model. The first image
 * adapter requires exactly one image input. Other tensor contracts remain
 * available through the general inference service. Calls must be serialized. */
esp_err_t solar_os_processor_create(const char *kind, uint32_t model,
    uint32_t timeout_ms, solar_os_processor_cancel_fn cancel, void *user,
    solar_os_processor_t **out);
void solar_os_processor_destroy(solar_os_processor_t *processor);
esp_err_t solar_os_processor_close_checked(solar_os_processor_t *processor);
bool solar_os_processor_captures(const solar_os_processor_t *processor);
esp_err_t solar_os_processor_bind_source(solar_os_processor_t *processor, const char *source);
esp_err_t solar_os_processor_capture(solar_os_processor_t *processor, const char *source,
    uint64_t sequence, solar_os_media_sample_t **sample, char **json);
/* Borrow an immutable image until return; owned JSON has a 64 KiB bound.
 * QR binary payloads and raw tensors use lossless lowercase hexadecimal. */
esp_err_t solar_os_processor_run(solar_os_processor_t *processor,
    solar_os_raster_image_t *image, char **json);
/* Encoded input stays unchanged. Current image processors decode on demand;
 * consumers that accept encoded images need not allocate a raster. */
esp_err_t solar_os_processor_run_sample(solar_os_processor_t *processor,
    const solar_os_media_sample_t *sample, char **json, uint64_t *decode_us);
