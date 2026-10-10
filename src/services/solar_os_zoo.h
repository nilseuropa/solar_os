#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_storage.h"

#define SOLAR_OS_ZOO_DEFAULT_SOURCE "https://solar-os.eu/zoo/catalog.json"
#define SOLAR_OS_ZOO_CATALOG_MAX (512U * 1024U)
#define SOLAR_OS_ZOO_MODELS_MAX 256U
#define SOLAR_OS_ZOO_SOURCE_MAX 320U
typedef struct solar_os_zoo solar_os_zoo_t;
typedef struct {
    char id[101], version[101], name[121], summary[1025];
    char tasks[192], modalities[96], result[32], license[64], validation[32];
    char reason[96];
    uint32_t archive_bytes, unpacked_bytes, min_internal_bytes, min_psram_bytes;
    bool compatible, memory_known;
} solar_os_zoo_model_t;
typedef struct {
    char stage[24];
    uint32_t bytes, total;
} solar_os_zoo_progress_t;
typedef void (*solar_os_zoo_progress_fn)(const solar_os_zoo_progress_t *, void *);

esp_err_t solar_os_zoo_open(solar_os_zoo_t **out);
void solar_os_zoo_close(solar_os_zoo_t *zoo);
const char *solar_os_zoo_source(const solar_os_zoo_t *zoo);
const char *solar_os_zoo_root(const solar_os_zoo_t *zoo);
void solar_os_zoo_get_source(char *buffer, size_t size);
esp_err_t solar_os_zoo_set_source(const char *url);
esp_err_t solar_os_zoo_set_storage(const char *target);
const char *solar_os_zoo_storage(void);
esp_err_t solar_os_zoo_reload(solar_os_zoo_t *zoo);
/* Reload/refresh/install are serialized across contexts. Do not read or close the same
 * context while an operation is running; cancel it and wait for its return. */
esp_err_t solar_os_zoo_refresh(solar_os_zoo_t *zoo, const volatile bool *cancel,
    solar_os_zoo_progress_fn progress, void *user);
size_t solar_os_zoo_count(const solar_os_zoo_t *zoo);
bool solar_os_zoo_get(const solar_os_zoo_t *zoo, size_t index, solar_os_zoo_model_t *model);
bool solar_os_zoo_installed(const solar_os_zoo_t *zoo, size_t index);
esp_err_t solar_os_zoo_bundle_path(const solar_os_zoo_t *zoo, size_t index, char *out, size_t size);
esp_err_t solar_os_zoo_install(solar_os_zoo_t *zoo, size_t index, const volatile bool *cancel,
    solar_os_zoo_progress_fn progress, void *user);
