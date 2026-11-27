#pragma once
#include "solar_os_native_display_link_abi.h"

#define SOLAR_OS_NATIVE_DISPLAY_LINK_TARGET_SERVICE "display.link.target"
#define SOLAR_OS_NATIVE_DISPLAY_LINK_TARGET_ABI 1U

typedef struct {
    uint32_t abi_version, struct_size;
    /* Register an open display.link as a selectable Core display. Its stream
     * must support I/O from the display task. Frame presentation goes through
     * Core while attached. On failure the link remains available to its caller.
     * Close through display.link on the owning task to unregister and release
     * it. A refused close keeps the link for retry. */
    int (*attach)(solar_os_native_handle_t link, const char *name, uint32_t pixel_budget);
} solar_os_native_display_link_target_api_v1_t;
