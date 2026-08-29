#include "solar_os_audio_backend.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

typedef struct {
    const solar_os_audio_backend_ops_t *ops;
    void *context;
} solar_os_audio_backend_entry_t;

static solar_os_audio_backend_entry_t active_backend;
static portMUX_TYPE backend_lock = portMUX_INITIALIZER_UNLOCKED;

static bool backend_ops_valid(const solar_os_audio_backend_ops_t *ops)
{
    return ops != NULL && ops->init != NULL && ops->deinit != NULL &&
        ops->set_volume != NULL && ops->write != NULL &&
        ops->get_status != NULL && ops->get_info != NULL;
}

static solar_os_audio_backend_entry_t backend_snapshot(void)
{
    portENTER_CRITICAL(&backend_lock);
    const solar_os_audio_backend_entry_t snapshot = active_backend;
    portEXIT_CRITICAL(&backend_lock);
    return snapshot;
}

esp_err_t solar_os_audio_backend_attach(const solar_os_audio_backend_ops_t *ops,
                                        void *context)
{
    if (!backend_ops_valid(ops) || context == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_audio_backend_info_t info = {0};
    ops->get_info(context, &info);
    if (info.id == NULL || info.id[0] == '\0' ||
        info.name == NULL || info.name[0] == '\0' ||
        (info.has_input && (ops->read == NULL || ops->set_mic_gain == NULL))) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&backend_lock);
    if (active_backend.ops != NULL) {
        portEXIT_CRITICAL(&backend_lock);
        return ESP_ERR_NOT_ALLOWED;
    }
    active_backend = (solar_os_audio_backend_entry_t) {
        .ops = ops,
        .context = context,
    };
    portEXIT_CRITICAL(&backend_lock);
    return ESP_OK;
}

esp_err_t solar_os_audio_backend_detach(void *context)
{
    if (context == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&backend_lock);
    if (active_backend.ops == NULL || active_backend.context != context) {
        portEXIT_CRITICAL(&backend_lock);
        return ESP_ERR_NOT_FOUND;
    }
    active_backend = (solar_os_audio_backend_entry_t){0};
    portEXIT_CRITICAL(&backend_lock);
    return ESP_OK;
}

bool solar_os_audio_backend_available(void)
{
    return backend_snapshot().ops != NULL;
}

bool solar_os_audio_backend_has_input(void)
{
    const solar_os_audio_backend_entry_t backend = backend_snapshot();
    solar_os_audio_backend_info_t info = {0};
    if (backend.ops != NULL) {
        backend.ops->get_info(backend.context, &info);
    }
    return info.has_input;
}

esp_err_t solar_os_audio_backend_init(void)
{
    const solar_os_audio_backend_entry_t backend = backend_snapshot();
    return backend.ops != NULL ? backend.ops->init(backend.context) :
        ESP_ERR_NOT_SUPPORTED;
}

void solar_os_audio_backend_deinit(void)
{
    const solar_os_audio_backend_entry_t backend = backend_snapshot();
    if (backend.ops != NULL) {
        backend.ops->deinit(backend.context);
    }
}

esp_err_t solar_os_audio_backend_set_volume(uint8_t volume)
{
    const solar_os_audio_backend_entry_t backend = backend_snapshot();
    return backend.ops != NULL ? backend.ops->set_volume(backend.context, volume) :
        ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_backend_set_mic_gain(float gain_db)
{
    const solar_os_audio_backend_entry_t backend = backend_snapshot();
    return backend.ops != NULL && backend.ops->set_mic_gain != NULL ?
        backend.ops->set_mic_gain(backend.context, gain_db) :
        ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_backend_write(const void *data, size_t len)
{
    const solar_os_audio_backend_entry_t backend = backend_snapshot();
    return backend.ops != NULL ? backend.ops->write(backend.context, data, len) :
        ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_backend_read(void *data, size_t len)
{
    const solar_os_audio_backend_entry_t backend = backend_snapshot();
    return backend.ops != NULL && backend.ops->read != NULL ?
        backend.ops->read(backend.context, data, len) : ESP_ERR_NOT_SUPPORTED;
}

void solar_os_audio_backend_get_status(solar_os_audio_backend_status_t *status)
{
    if (status == NULL) {
        return;
    }
    const solar_os_audio_backend_entry_t backend = backend_snapshot();
    if (backend.ops == NULL) {
        memset(status, 0, sizeof(*status));
        status->i2s_port = -1;
        status->mclk_pin = -1;
        status->bclk_pin = -1;
        status->ws_pin = -1;
        status->din_pin = -1;
        status->dout_pin = -1;
        status->pa_pin = -1;
        status->output_codec = "-";
        status->input_codec = "-";
        return;
    }
    backend.ops->get_status(backend.context, status);
}

void solar_os_audio_backend_get_info(solar_os_audio_backend_info_t *info)
{
    if (info == NULL) {
        return;
    }
    *info = (solar_os_audio_backend_info_t){0};
    const solar_os_audio_backend_entry_t backend = backend_snapshot();
    if (backend.ops != NULL) {
        backend.ops->get_info(backend.context, info);
    }
}
