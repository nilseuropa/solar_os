#include "solar_os_native_message_provider.h"
#include "solar_os_config.h"

#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_MESSAGING
#include "solar_os_memory.h"
#include "solar_os_messaging.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"
#include "solar_os_native_support.h"

typedef struct {
    char name[SOLAR_OS_NATIVE_MESSAGE_CLASS_NAME_MAX];
    void *user;
    solar_os_native_message_provider_operations_v1_t callbacks;
    solar_os_messaging_provider_class_t type;
    solar_os_native_handle_t lease;
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
    bool registered;
} native_class_t;

typedef struct {
    native_class_t *type;
    solar_os_messaging_provider_id_t id;
    void *user;
} native_instance_t;

static esp_err_t create_from_args(void *opaque, const char *name, int argc,
    const char *const *argv, solar_os_messaging_provider_id_t *id)
{
    native_class_t *type = opaque;
    *id = 0;
    const esp_err_t error = solar_os_native_callback_result(type->callbacks.create(type->user, name, argc, argv, id));
    if (error == ESP_OK && !solar_os_messaging_provider_is_class(*id, type->name)) return ESP_ERR_INVALID_STATE;
    return error;
}

static esp_err_t settings_read(void *opaque, void *context, unsigned form, solar_os_messaging_settings_t *out)
{
    native_class_t *type = opaque;
    native_instance_t *instance = context;
    return solar_os_native_callback_result(type->callbacks.settings_read(type->user,
        instance ? instance->user : NULL, instance ? instance->id : 0, form, out));
}

static esp_err_t settings_write(void *opaque, void *context, unsigned form, const char *name,
    const solar_os_message_setting_values_v1_t *in, solar_os_messaging_provider_id_t *id)
{
    native_class_t *type = opaque;
    native_instance_t *instance = context;
    *id = instance ? instance->id : 0;
    const esp_err_t error = solar_os_native_callback_result(type->callbacks.settings_write(type->user,
        instance ? instance->user : NULL, instance ? instance->id : 0, form, name, in, id));
    if (error == ESP_OK && (instance ? *id != instance->id : !solar_os_messaging_provider_is_class(*id, type->name)))
        return ESP_ERR_INVALID_STATE;
    return error;
}

static esp_err_t instance_configure(void *opaque, int argc, const char *const *argv)
{
    native_instance_t *instance = opaque;
    if (!instance) return ESP_ERR_INVALID_STATE;
    return solar_os_native_callback_result(instance->type->callbacks.configure(instance->user, argc, argv));
}

static esp_err_t instance_connect(void *opaque, bool enabled)
{
    native_instance_t *instance = opaque;
    if (!instance) return ESP_ERR_INVALID_STATE;
    return solar_os_native_callback_result(instance->type->callbacks.connect(instance->user, enabled));
}

static esp_err_t instance_join(void *opaque, const char *target)
{
    native_instance_t *instance = opaque;
    if (!instance) return ESP_ERR_INVALID_STATE;
    return solar_os_native_callback_result(instance->type->callbacks.join(instance->user, target));
}

static esp_err_t instance_join_open(void *opaque, const char *target, solar_os_conversation_id_t *conversation)
{
    native_instance_t *instance = opaque;
    if (!instance) return ESP_ERR_INVALID_STATE;
    return solar_os_native_callback_result(instance->type->callbacks.join_open(instance->user, target, conversation));
}

static esp_err_t instance_leave(void *opaque, const char *target)
{
    native_instance_t *instance = opaque;
    if (!instance) return ESP_ERR_INVALID_STATE;
    return solar_os_native_callback_result(instance->type->callbacks.leave(instance->user, target));
}

static esp_err_t instance_direct_open(void *opaque, const char *target, solar_os_conversation_id_t *conversation)
{
    native_instance_t *instance = opaque;
    if (!instance) return ESP_ERR_INVALID_STATE;
    return solar_os_native_callback_result(instance->type->callbacks.direct_open(instance->user, target, conversation));
}

static esp_err_t normalize(void *opaque, const char *target, char *out, size_t capacity)
{
    native_instance_t *instance = opaque;
    if (!instance) return ESP_ERR_INVALID_STATE;
    memset(out, 0, capacity);
    const esp_err_t error = solar_os_native_callback_result(
        instance->type->callbacks.normalize_target(instance->user, target, out, capacity));
    return error == ESP_OK && (!out[0] || !memchr(out, 0, capacity)) ? ESP_ERR_INVALID_SIZE : error;
}

static esp_err_t prepare_retire(void *opaque)
{
    native_instance_t *instance = opaque;
    return instance->type->callbacks.prepare_retire ?
        solar_os_native_callback_result(instance->type->callbacks.prepare_retire(instance->user)) : ESP_OK;
}

static void finish_retire(void *opaque, bool retired)
{
    native_instance_t *instance = opaque;
    if (instance->type->callbacks.finish_retire)
        instance->type->callbacks.finish_retire(instance->user, retired);
    if (retired) solar_os_memory_free(instance);
}

static int class_destroy(void *opaque)
{
    native_class_t *type = opaque;
    if (type->registered) {
        const esp_err_t error = solar_os_messaging_class_unregister(&type->type);
        if (error == ESP_ERR_INVALID_STATE) return SOLAR_OS_NATIVE_ERROR_BUSY;
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        type->registered = false;
    }
    if (type->lease) {
        const int result = solar_os_native_library_release(type->lease);
        if (result != SOLAR_OS_NATIVE_OK && result != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return result;
    }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (type->driver_pin) solar_os_native_driver_unpin(type->driver_pin);
#endif
    solar_os_memory_free(type);
    return SOLAR_OS_NATIVE_OK;
}

static int class_close(solar_os_native_handle_t handle)
{
    return solar_os_native_handle_close_kind(handle, SOLAR_OS_NATIVE_HANDLE_MESSAGE_CLASS);
}

static int class_register(const solar_os_native_message_class_v1_t *in, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!in || !in->name || !*in->name || strnlen(in->name, SOLAR_OS_NATIVE_MESSAGE_CLASS_NAME_MAX) >= SOLAR_OS_NATIVE_MESSAGE_CLASS_NAME_MAX ||
        ((in->operations.prepare_retire == NULL) != (in->operations.finish_retire == NULL)))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_class_t *type = solar_os_memory_calloc(1, sizeof(*type), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.messages.class");
    if (!type) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strlcpy(type->name, in->name, sizeof(type->name));
    type->user = in->user;
    type->callbacks = in->operations;
    const solar_os_native_message_provider_operations_v1_t *ops = &type->callbacks;
    type->type = (solar_os_messaging_provider_class_t){
        .name = type->name, .owner = type, .capabilities = in->capabilities,
        .connection_oriented = in->connection_oriented,
        .create_owned = ops->create ? create_from_args : NULL,
        .settings_read_owned = ops->settings_read ? settings_read : NULL,
        .settings_write_owned = ops->settings_write ? settings_write : NULL,
        .configure = ops->configure ? instance_configure : NULL,
        .connect = ops->connect ? instance_connect : NULL,
        .join = ops->join ? instance_join : NULL,
        .join_open = ops->join_open ? instance_join_open : NULL,
        .leave = ops->leave ? instance_leave : NULL,
        .direct_open = ops->direct_open ? instance_direct_open : NULL,
        .normalize_target = ops->normalize_target ? normalize : NULL,
        .remove_prepare = prepare_retire, .remove_finish = finish_retire,
    };
    solar_os_native_handle_t handle;
    int result = solar_os_native_handle_create_exclusive(SOLAR_OS_NATIVE_HANDLE_MESSAGE_CLASS,
        type, class_destroy, NULL, &handle);
    if (result) {
        solar_os_memory_free(type);
        return result;
    }
    if (in->library_lease) {
        result = solar_os_native_library_retain(in->library_lease, &type->lease);
    } else {
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
        type->driver_pin = solar_os_native_driver_pin();
        result = type->driver_pin ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#else
        result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#endif
    }
    if (!result) {
        result = solar_os_native_result(solar_os_messaging_class_register(&type->type));
        type->registered = result == SOLAR_OS_NATIVE_OK;
    }
    solar_os_native_handle_release_exclusive(handle);
    if (result && class_close(handle) == SOLAR_OS_NATIVE_OK) return result;
    *out = handle;
    return result;
}

static native_class_t *class_acquire(solar_os_native_handle_t handle)
{
    native_class_t *type = solar_os_native_handle_acquire(handle, SOLAR_OS_NATIVE_HANDLE_MESSAGE_CLASS);
    if (type && !type->registered) {
        solar_os_native_handle_release(handle);
        return NULL;
    }
    return type;
}

static int instance_create(solar_os_native_handle_t handle, const char *name, const void *profile,
    size_t size, solar_os_messaging_provider_id_t *id)
{
    if (!id) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *id = 0;
    if (!profile || !size || size > SOLAR_OS_NATIVE_MESSAGE_PROFILE_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    native_class_t *type = class_acquire(handle);
    if (!type) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_messaging_instance_create(type->name, name, profile, size, id);
    solar_os_native_handle_release(handle);
    return solar_os_native_result(error);
}

static int instance_attach(solar_os_native_handle_t handle, solar_os_messaging_provider_id_t id, void *user)
{
    native_class_t *type = class_acquire(handle);
    if (!type) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    native_instance_t *instance = solar_os_memory_alloc(sizeof(*instance),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "native.messages.instance");
    esp_err_t error = ESP_ERR_NO_MEM;
    if (instance) {
        *instance = (native_instance_t){.type = type, .id = id, .user = user};
        error = solar_os_messaging_instance_attach(id, &type->type, instance);
        if (error != ESP_OK) solar_os_memory_free(instance);
    }
    solar_os_native_handle_release(handle);
    return solar_os_native_result(error);
}

static int instance_detach(solar_os_native_handle_t handle, solar_os_messaging_provider_id_t id)
{
    native_class_t *type = class_acquire(handle);
    if (!type) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_messaging_instance_detach(id, &type->type);
    solar_os_native_handle_release(handle);
    return error == ESP_ERR_INVALID_STATE ? SOLAR_OS_NATIVE_ERROR_BUSY : solar_os_native_result(error);
}

static esp_err_t retain_profile(const native_class_t *type, solar_os_messaging_provider_id_t id, void **lease)
{
    if (!solar_os_messaging_provider_is_class(id, type->name)) return ESP_ERR_NOT_FOUND;
    return solar_os_messaging_instance_retain(id, lease);
}

static int profile_read(solar_os_native_handle_t handle, solar_os_messaging_provider_id_t id,
    void *out, size_t size)
{
    if (!out || !size || size > SOLAR_OS_NATIVE_MESSAGE_PROFILE_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    native_class_t *type = class_acquire(handle);
    if (!type) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    void *lease = NULL;
    esp_err_t error = retain_profile(type, id, &lease);
    if (error == ESP_OK) error = solar_os_messaging_profile_load(id, out, size);
    if (error != ESP_OK) explicit_bzero(out, size);
    solar_os_messaging_instance_release(lease);
    solar_os_native_handle_release(handle);
    return solar_os_native_result(error);
}

static int profile_write(solar_os_native_handle_t handle, solar_os_messaging_provider_id_t id,
    const void *profile, size_t size)
{
    if (!profile || !size || size > SOLAR_OS_NATIVE_MESSAGE_PROFILE_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    native_class_t *type = class_acquire(handle);
    if (!type) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    void *lease = NULL;
    esp_err_t error = retain_profile(type, id, &lease);
    if (error == ESP_OK) error = solar_os_messaging_profile_save(id, profile, size);
    solar_os_messaging_instance_release(lease);
    solar_os_native_handle_release(handle);
    return solar_os_native_result(error);
}

static int lease_destroy(void *opaque)
{
    solar_os_messaging_instance_release(opaque);
    return SOLAR_OS_NATIVE_OK;
}

static int instance_retain(solar_os_messaging_provider_id_t id, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    void *lease;
    const esp_err_t error = solar_os_messaging_instance_retain(id, &lease);
    if (error != ESP_OK) return solar_os_native_result(error);
    const int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_MESSAGE_LEASE, lease, lease_destroy, out);
    if (result) solar_os_messaging_instance_release(lease);
    return result;
}

static int instance_release(solar_os_native_handle_t lease)
{
    return solar_os_native_handle_close_kind(lease, SOLAR_OS_NATIVE_HANDLE_MESSAGE_LEASE);
}

static const solar_os_native_message_provider_api_v1_t message_provider_api = {
    .abi_version = SOLAR_OS_NATIVE_MESSAGE_PROVIDER_ABI,
    .struct_size = sizeof(message_provider_api),
    .register_class = class_register, .close_class = class_close,
    .create_instance = instance_create, .attach_instance = instance_attach, .detach_instance = instance_detach,
    .profile_read = profile_read, .profile_write = profile_write,
    .retain_instance = instance_retain, .release_instance = instance_release,
};
#endif

const void *solar_os_native_message_provider_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_message_provider_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
