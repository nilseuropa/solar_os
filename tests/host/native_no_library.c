#include "solar_os_native_service_abi.h"
/* These service fixtures have no executable-library backend. */
int solar_os_native_library_retain(uint32_t lease, uint32_t *out)
{ (void)lease;if(out)*out=0;return SOLAR_OS_NATIVE_ERROR_NOT_FOUND; }
int solar_os_native_library_release(uint32_t lease)
{ (void)lease;return SOLAR_OS_NATIVE_ERROR_NOT_FOUND; }
