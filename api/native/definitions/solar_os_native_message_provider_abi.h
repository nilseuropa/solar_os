#pragma once
#include "solar_os_native_messages_abi.h"

#define SOLAR_OS_NATIVE_MESSAGE_PROVIDER_SERVICE "messages.provider"
#define SOLAR_OS_NATIVE_MESSAGE_PROVIDER_ABI 1U
#define SOLAR_OS_NATIVE_MESSAGE_CLASS_NAME_MAX 16U
#define SOLAR_OS_NATIVE_MESSAGE_PROFILE_MAX 3072U

typedef struct {
    int (*create)(void *user, const char *name, int argc, const char *const *argv,
        solar_os_messaging_provider_id_t *id);
    int (*configure)(void *instance, int argc, const char *const *argv);
    int (*connect)(void *instance, bool enabled);
    int (*join)(void *instance, const char *target);
    int (*join_open)(void *instance, const char *target, solar_os_conversation_id_t *conversation);
    int (*leave)(void *instance, const char *target);
    int (*direct_open)(void *instance, const char *target, solar_os_conversation_id_t *conversation);
    int (*normalize_target)(void *instance, const char *target, char *normalized, size_t capacity);
    /* A zero provider ID describes creation. Form enumeration ends with
     * NOT_FOUND. The service validates field bounds and masks secret values. */
    int (*settings_read)(void *user, void *instance, solar_os_messaging_provider_id_t provider,
        uint32_t form, solar_os_message_settings_v1_t *settings);
    int (*settings_write)(void *user, void *instance, solar_os_messaging_provider_id_t provider,
        uint32_t form, const char *name, const solar_os_message_setting_values_v1_t *settings,
        solar_os_messaging_provider_id_t *result);
    /* Provide both retirement hooks or neither. A successful prepare
     * quiesces workers until finish. On a prepare error the instance
     * remains installed. A false finish resumes it after a failed save. */
    int (*prepare_retire)(void *instance);
    void (*finish_retire)(void *instance, bool retired);
} solar_os_native_message_provider_operations_v1_t;

typedef struct {
    const char *name;
    uint32_t capabilities;
    bool connection_oriented;
    solar_os_native_handle_t library_lease;
    void *user;
    solar_os_native_message_provider_operations_v1_t operations;
} solar_os_native_message_class_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    /* Register with the same Messaging registry as built-in transports.
     * Metadata and callbacks are copied. The class retains its code lease,
     * or the current expansion driver if registering during attachment. */
    int (*register_class)(const solar_os_native_message_class_v1_t *type, solar_os_native_handle_t *registration);
    /* Refuses while any instance context, callback or instance lease
     * remains. Persisted profiles stay available for a later registration. */
    int (*close_class)(solar_os_native_handle_t type);
    /* Reserve a name and persist a provider-owned, nonempty versioned
     * profile. Call attach_instance to activate its transport. */
    int (*create_instance)(solar_os_native_handle_t type, const char *name, const void *profile,
        size_t bytes, solar_os_messaging_provider_id_t *id);
    int (*attach_instance)(solar_os_native_handle_t type, solar_os_messaging_provider_id_t id, void *instance);
    /* Quiesce and release a transport without deleting its profile or
     * history. Use messages.provider_remove to delete an instance. */
    int (*detach_instance)(solar_os_native_handle_t type, solar_os_messaging_provider_id_t id);
    /* Only the owning class can access its profiles. Read expects the
     * exact saved byte count. Keep the profile format stable or version it. */
    int (*profile_read)(solar_os_native_handle_t type, solar_os_messaging_provider_id_t id, void *out, size_t bytes);
    int (*profile_write)(solar_os_native_handle_t type, solar_os_messaging_provider_id_t id,
        const void *profile, size_t bytes);
    /* Retain an instance while another resource depends on it. Release
     * the lease when that resource closes. Removal and detach defer. */
    int (*retain_instance)(solar_os_messaging_provider_id_t id, solar_os_native_handle_t *lease);
    int (*release_instance)(solar_os_native_handle_t lease);
} solar_os_native_message_provider_api_v1_t;
