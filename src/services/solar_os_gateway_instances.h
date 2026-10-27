#pragma once

#include "solar_os_chat.h"

/* Compatibility entry points in solar_os_chat.h address the default instance.
 * The job and class callbacks bind an explicit store for the duration of work.
 * Each task has its own binding; child transport tasks bind their own owner. */
esp_err_t solar_os_gateway_instances_init(void);
void *solar_os_gateway_scope_enter(void *instance);
void solar_os_gateway_scope_leave(void *previous);
solar_os_messaging_provider_id_t solar_os_gateway_current_id(void);
const char *solar_os_gateway_current_name(void);
void **solar_os_gateway_transport_slot(void);
void **solar_os_gateway_worker_slot(void);
void *solar_os_gateway_current_instance(void);
void solar_os_gateway_workers_lock(void);
void solar_os_gateway_workers_unlock(void);
/* Caller holds the workers lock, including while using the returned store. */
void *solar_os_gateway_instance_next(solar_os_messaging_provider_id_t *cursor);
typedef struct {
    esp_err_t (*prepare)(void);
    void (*destroy)(void);
} solar_os_gateway_runtime_hooks_t;
void solar_os_gateway_set_runtime_hooks(const solar_os_gateway_runtime_hooks_t *hooks);

esp_err_t solar_os_gateway_instance_create(const char *name, const char *url,
    const char *token, solar_os_messaging_provider_id_t *id);
esp_err_t solar_os_gateway_instance_configure(solar_os_messaging_provider_id_t id,
    const char *url, const char *token);
esp_err_t solar_os_gateway_instance_get_status(solar_os_messaging_provider_id_t id,
    solar_os_chat_status_t *status);
size_t solar_os_gateway_instance_channels(solar_os_messaging_provider_id_t id,
    solar_os_chat_channel_t *channels, size_t capacity);
