#include "solar_os_native_ui.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_ui_abi.h"

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_gfx_widgets.h"
#include "solar_os_tui_widgets.h"
static const solar_os_native_ui_api_v1_t ui_api = {
    .abi_version = SOLAR_OS_NATIVE_UI_ABI, .struct_size = sizeof(ui_api),
    .label = solar_os_gfx_label, .centered_label = solar_os_gfx_centered_label,
    .input = solar_os_gfx_input, .dialog = solar_os_gfx_dialog,
    .input_key = solar_os_tui_input_key, .input_text = solar_os_tui_input_text,
    .viewport_reconcile = solar_os_tui_viewport_reconcile, .viewport_key = solar_os_tui_viewport_key,
};
#endif

const void *solar_os_native_ui_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_ui_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
