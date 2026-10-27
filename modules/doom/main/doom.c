// SPDX-License-Identifier: GPL-2.0-or-later
#include "port.h"
#include "doomdef.h"
#include "i_system.h"

const solar_os_native_host_api_v1_t *solar_os_doom_host;
const solar_os_native_foreground_api_v1_t *solar_os_doom_app;
const solar_os_native_memory_api_v1_t *solar_os_doom_memory;
const solar_os_native_libc_api_v1_t *solar_os_doom_libc;
jmp_buf solar_os_doom_trap;
int solar_os_doom_result;
char solar_os_doom_path[256];
uint64_t solar_os_doom_epoch;

SOLAR_OS_NATIVE_APP_MANIFEST = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
    .lifecycle_abi = SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_app_manifest_v2_t),
    .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI,
    .flags = SOLAR_OS_NATIVE_APP_RESUMABLE,
    .worker_stack_bytes = 24U * 1024U, .worker_priority = 1,
    .minimum_internal_bytes = 8U * 1024U,
    .minimum_psram_bytes = 4U * 1024U * 1024U,
    .tick_interval_ms = 5,
    .id = "doom", .summary = "DOOM (Espressif PrBoom)",
    .file_extensions = ".wad",
};

int main(int argc, char **argv)
{
    solar_os_doom_host = solar_os_native_host_v1();
    if (!solar_os_doom_host || solar_os_doom_host->abi_version != SOLAR_OS_NATIVE_ABI_VERSION ||
        solar_os_doom_host->struct_size < sizeof(*solar_os_doom_host) ||
        !solar_os_doom_host->get_service) return 1;
    solar_os_doom_app = solar_os_doom_host->get_service("foreground", 1,
        offsetof(solar_os_native_foreground_api_v1_t, suspend) + sizeof(solar_os_doom_app->suspend));
    solar_os_doom_memory = solar_os_doom_host->get_service("memory", 1,
        offsetof(solar_os_native_memory_api_v1_t, alloc_internal));
    solar_os_doom_libc = solar_os_doom_host->get_service("libc", 1,
        sizeof(solar_os_native_libc_api_v1_t));
    if (!solar_os_doom_app || !solar_os_doom_memory || !solar_os_doom_libc) return 1;
    if (argc != 2) {
        printf("Usage: doom /sdcard/doom/doom1.wad\n");
        return 1;
    }
    if (solar_os_doom_app->resolve_path(argv[1], solar_os_doom_path, sizeof(solar_os_doom_path))) {
        printf("doom: invalid WAD path\n"); return 1;
    }
    solar_os_doom_epoch = solar_os_doom_app->time_us();
    if (!setjmp(solar_os_doom_trap)) {
        if (solar_os_doom_app->enable_suspend()) solar_os_doom_exit(1);
        /* Validate the directory before the legacy engine uses its counts. */
        extern void solar_os_doom_validate_wad(void);
        solar_os_doom_validate_wad();
        const char *args[] = { "doom", "-nomusic", "-noautoload" };
        doom_main(sizeof(args) / sizeof(args[0]), args);
    }
    solar_os_doom_sound_close();
    solar_os_doom_cleanup();
    return solar_os_doom_result;
}
