#pragma once
#include "solar_os_messaging.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { SOLAR_OS_MESHCORE_BACKEND_RADIO = 1, SOLAR_OS_MESHCORE_BACKEND_BLE = 2 };
typedef struct {
    uint32_t version;
    uint8_t backend;
    bool enabled, public_channel, pin_supplied;
    uint8_t bda[6], addr_type;
    uint32_t pin;
    char radio[24], profile[32], name[33];
} solar_os_meshcore_instance_config_t;

/* Operations execute with an explicit task scope and workers excluded from
 * configuration/removal. Each backend owns a separate runtime state per scope. */
typedef struct {
    esp_err_t (*start)(const solar_os_meshcore_instance_config_t *, const char *owner);
    void (*poll)(void);
    void (*cancel)(void);
    void (*stop)(void);
    void (*destroy)(void);
    esp_err_t (*join)(const char *, solar_os_conversation_id_t *);
    esp_err_t (*leave)(const char *);
    esp_err_t (*configure)(int argc, const char *const *argv);
    esp_err_t (*remove_prepare)(void);
} solar_os_meshcore_backend_t;
void solar_os_meshcore_backend_register(unsigned kind, const solar_os_meshcore_backend_t *backend);
esp_err_t solar_os_meshcore_instances_init(void);
solar_os_messaging_provider_id_t solar_os_meshcore_current_id(void);
void *solar_os_meshcore_current_state(unsigned kind, size_t size);
void solar_os_meshcore_current_state_free(unsigned kind);
esp_err_t solar_os_meshcore_current_config(solar_os_meshcore_instance_config_t *config);
esp_err_t solar_os_meshcore_current_name_set(const char *name);
esp_err_t solar_os_meshcore_current_public_set(bool enabled);
/* Backend subscription edits can pause their own runtime while its manager
 * scope is held. The manager restarts it when the saved binding is enabled. */
void solar_os_meshcore_current_pause(void);

/* Legacy APIs prefer an unbound default or one matching the backend. Otherwise
 * use the sole instance for that backend; multiple matches are ambiguous.
 * Nested calls reuse their existing scope. End every returned lease. */
esp_err_t solar_os_meshcore_legacy_begin(unsigned kind, void **lease);
void solar_os_meshcore_legacy_end(void *lease);

esp_err_t solar_os_meshcore_instances_bind(unsigned kind, int argc, const char *const *argv);
void solar_os_meshcore_instances_poll(unsigned kind);
void solar_os_meshcore_instances_cancel(unsigned kind);
void solar_os_meshcore_instances_stop(unsigned kind);
bool solar_os_meshcore_instances_next(unsigned kind, solar_os_messaging_provider_id_t *cursor,
    solar_os_messaging_provider_status_t *status);

#ifdef __cplusplus
}
#endif
