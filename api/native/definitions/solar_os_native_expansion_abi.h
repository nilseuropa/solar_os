#pragma once

#include "solar_os_expansion_types_v1.h"
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_EXPANSION_SERVICE "expansion"
#define SOLAR_OS_NATIVE_EXPANSION_ABI 1U
#define SOLAR_OS_NATIVE_EXPANSION_LABEL_BINDING 0U
#define SOLAR_OS_NATIVE_EXPANSION_LABEL_CATEGORY 1U
#define SOLAR_OS_NATIVE_EXPANSION_LABEL_ORIGIN 2U

/* The device registry owns attached devices and copies all bindings. They
 * survive the calling app. Enumeration copies one current device per call;
 * indices can move when another caller changes the registry. Names identify
 * devices for operations. Bindings follow the same validation as the shell. */
typedef struct {
    uint32_t abi_version, struct_size;
    bool (*available)(void);
    size_t (*device_count)(void);
    bool (*device_info)(size_t index, solar_os_expansion_device_t *device);
    int (*attach)(const char *driver, const char *name,
        const solar_os_expansion_binding_t *bindings, size_t count);
    int (*attach_board_device)(const char *name);
    int (*detach)(const char *name);
    int (*set_ready)(const char *name, bool ready);
    size_t (*driver_count)(void);
    bool (*driver_supported)(const char *name);
    int (*validate)(const char *driver, const solar_os_expansion_binding_t *bindings,
        size_t count, solar_os_expansion_binding_validation_t *validation);
    bool (*binding_pin_supported)(solar_os_expansion_binding_kind_t kind, const char *target, int pin);
    bool (*stream_binding_valid)(solar_os_expansion_binding_kind_t kind, const char *stream_id);
    const char *(*label)(uint32_t field, uint32_t value);
    /* These calls copy metadata and never expose driver callbacks. Enumeration
     * and named lookup include drivers available for manual attachment. */
    int (*driver_info)(size_t index, solar_os_expansion_driver_info_t *info);
    int (*driver_find)(const char *driver, solar_os_expansion_driver_info_t *info);
    /* Pass values=NULL and capacity=0 for metadata only. If a provided array
     * is too small, metadata still reports the full required value count. */
    int (*driver_binding)(const char *driver, size_t spec_index, solar_os_expansion_binding_spec_info_t *spec,
        int *values, size_t capacity);
} solar_os_native_expansion_api_v1_t;
