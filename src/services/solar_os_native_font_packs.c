#include "solar_os_native_font_packs.h"
#include "solar_os_config.h"
#include "solar_os_native_font_packs_abi.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os_font_packs.h"
#include "solar_os_native_support.h"

static int info(size_t index, solar_os_font_pack_info_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    return solar_os_font_pack_get(index, out) ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}

static int load(const char *name)
{ return solar_os_native_result(solar_os_font_pack_load(name)); }

static int unload(const char *name)
{ return solar_os_native_result(solar_os_font_pack_unload(name)); }

static int save_selection(void)
{ return solar_os_native_result(solar_os_font_pack_save_selection()); }

static int saved_selection(char *names, size_t capacity)
{ return solar_os_native_result(solar_os_font_pack_saved_selection(names, capacity)); }

static int directory(char *path, size_t capacity)
{ return solar_os_native_result(solar_os_font_pack_dir(path, capacity)); }

static int visit_files(solar_os_font_pack_visit_fn visit, void *user)
{ return solar_os_native_result(solar_os_font_pack_list_files(visit, user)); }

static const solar_os_native_font_packs_api_v1_t font_packs_api = {
    .abi_version = SOLAR_OS_NATIVE_FONT_PACKS_ABI, .struct_size = sizeof(font_packs_api),
    .count = solar_os_font_pack_count, .info = info, .generation = solar_os_font_pack_generation,
    .name_valid = solar_os_font_pack_name_valid, .load = load, .unload = unload,
    .save_selection = save_selection, .saved_selection = saved_selection,
    .directory = directory, .visit_files = visit_files,
};
#endif

const void *solar_os_native_font_packs_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_font_packs_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
