#pragma once
#include <stdbool.h>
#include "solar_os_raster_image.h"

#define SOLAR_OS_PROCESSOR_JSON_MAX 65536U
typedef struct solar_os_processor solar_os_processor_t;
typedef bool (*solar_os_processor_cancel_fn)(void *user);

/* An OS-owned native processor. Model processors retain a resident bundle;
 * destruction releases the reference, never unloads the model. The first image
 * adapter requires exactly one image input. Other tensor contracts remain
 * available through the general inference service. Calls must be serialized. */
esp_err_t solar_os_processor_create(const char *kind, uint32_t model,
    uint32_t timeout_ms, solar_os_processor_cancel_fn cancel, void *user,
    solar_os_processor_t **out);
void solar_os_processor_destroy(solar_os_processor_t *processor);
/* Borrow an immutable image until return; owned JSON has a 64 KiB bound.
 * QR binary payloads and raw tensors use lossless lowercase hexadecimal. */
esp_err_t solar_os_processor_run(solar_os_processor_t *processor,
    solar_os_raster_image_t *image, char **json);
