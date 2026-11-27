#include "solar_os_native_zip.h"
#include "solar_os_config.h"
#include "solar_os_native_zip_abi.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_ZIP
#include "solar_os_native_support.h"
#include "solar_os_zip.h"

static int zip_list(const char *path, solar_os_zip_progress_cb_t visit, void *user)
{
    return solar_os_native_result(solar_os_zip_list(path, visit, user));
}

static int zip_read(const char *path, const char *name, void *data,
                    size_t capacity, size_t *size)
{
    return solar_os_native_result(solar_os_zip_read_into(path, name, data, capacity, size));
}

static int zip_write_result(esp_err_t error)
{
    if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
    if (error == ESP_ERR_TIMEOUT) return SOLAR_OS_NATIVE_ERROR_CANCELLED;
    return solar_os_native_result(error);
}

static int zip_create(const char *archive, const char *const *sources, size_t count,
                       bool store_only, solar_os_zip_progress_cb_t progress, void *user)
{
    const solar_os_zip_options_t options = {
        .store_only = store_only, .progress = progress, .user = user,
    };
    return zip_write_result(solar_os_zip_create(archive, sources, count, &options));
}

static int zip_extract(const char *archive, const char *destination,
                        solar_os_zip_progress_cb_t progress, bool (*cancelled)(void *), void *user)
{
    const solar_os_unzip_options_t options = {
        .progress = progress, .user = user, .cancelled = cancelled,
    };
    return zip_write_result(solar_os_zip_extract(archive, destination, &options));
}

static const solar_os_native_zip_api_v1_t zip_api = {
    .abi_version = SOLAR_OS_NATIVE_ZIP_ABI,
    .struct_size = sizeof(zip_api),
    .list = zip_list,
    .read = zip_read,
    .create = zip_create,
    .extract = zip_extract,
};
#endif

const void *solar_os_native_zip_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_zip_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
