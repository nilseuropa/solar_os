#pragma once
/* Core lifecycle and lease management, independent of ELF loading. */
#include "solar_os_components.h"
#include "solar_os_module_packages.h"
#include "solar_os_system_api.h"
#include "solar_os_expansion_types_v1.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Shared try-lock for image relocation and resident lifecycle changes.
 * Successful begin must be paired on the same task with end. */
esp_err_t solar_os_native_operation_begin(void);
void solar_os_native_operation_end(void);
esp_err_t solar_os_native_module_activate(solar_os_module_type_t type,
                                          const char *id,
                                          const char *path,
                                          char *detail,
                                          size_t detail_len);
esp_err_t solar_os_native_module_deactivate(solar_os_module_type_t type,
                                            const char *id,
                                            char *detail,
                                            size_t detail_len);
bool solar_os_native_module_active(solar_os_module_type_t type,
                                   const char *id);

/* Available during a bound driver's attach callback. Retain code until a
 * native provider or retained bus handle has been closed. */
void *solar_os_native_driver_pin(void);
void solar_os_native_driver_unpin(void *pin);
bool solar_os_native_driver_has_binding(const char *owner, solar_os_expansion_binding_kind_t kind, const char *target);

/* Stable native service results, usable by firmware consumers as well. */
int solar_os_native_library_acquire(const char *id, uint32_t abi_version,
                                    uint32_t minimum_struct_size,
                                    uint32_t *lease, const void **api);
int solar_os_native_library_retain(uint32_t lease, uint32_t *retained_lease);
int solar_os_native_library_release(uint32_t lease);
int solar_os_native_library_unload(const char *id);

#ifdef __cplusplus
}
#endif
