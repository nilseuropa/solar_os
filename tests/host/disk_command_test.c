#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"

#define SOLAR_OS_STORAGE_PATH_MAX 160
typedef int solar_os_context_t;
typedef int solar_os_shell_io_t;
static char output[2048];
static unsigned volume_calls, image_calls;
static const char *expected_volume, *expected_mount;
static unsigned expected_partition;
static esp_err_t volume_error;
static const char *disk_subcommands[] = {"status", "lsblk", "mount", "umount", "format"};
static solar_os_shell_io_t *terminal(solar_os_context_t *ctx) { return ctx; }
static void solar_os_shell_io_printf(solar_os_shell_io_t *io, const char *format, ...)
{
    (void)io;
    va_list args; va_start(args, format);
    vsnprintf(output + strlen(output), sizeof(output) - strlen(output), format, args);
    va_end(args);
}
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io, const char *text)
{ solar_os_shell_io_printf(io, "%s\n", text); }
static void disk_print_status(solar_os_shell_io_t *io) { (void)io; }
static void disk_print_lsblk(solar_os_shell_io_t *io) { (void)io; }
static bool disk_print_mounted_volume(solar_os_shell_io_t *io, const char *name)
{ (void)io; (void)name; return true; }
static const char *solar_os_shell_error_text(esp_err_t err)
{ return err == ESP_ERR_INVALID_STATE ? "invalid state" : "error"; }
static esp_err_t solar_os_shell_resolve_path(solar_os_context_t *ctx, const char *path, char *out, size_t size)
{ (void)ctx; snprintf(out, size, "%s", path); return ESP_OK; }
static esp_err_t solar_os_storage_mount_volume(const char *name, const char *mount)
{
    assert(!strcmp(name, expected_volume));
    assert((!mount && !expected_mount) || (mount && expected_mount && !strcmp(mount, expected_mount)));
    volume_calls++; return volume_error;
}
static esp_err_t solar_os_disk_image_mount(const char *path, const char *mount, unsigned partition)
{
    assert(!strcmp(path, expected_volume) && !strcmp(mount, expected_mount));
    assert(partition == expected_partition); image_calls++; return ESP_OK;
}
static esp_err_t solar_os_storage_mount(void) { assert(false); return ESP_FAIL; }
static esp_err_t solar_os_storage_unmount(void) { assert(false); return ESP_FAIL; }
static esp_err_t solar_os_storage_unmount_volume(const char *target) { (void)target; assert(false); return ESP_FAIL; }
static esp_err_t solar_os_storage_format(const char *target) { (void)target; assert(false); return ESP_FAIL; }
static esp_err_t solar_os_disk_image_create(const char *path, uint32_t size)
{ (void)path; (void)size; assert(false); return ESP_FAIL; }
static bool shell_print_not_supported(solar_os_shell_io_t *io, const char *command, const char *feature, esp_err_t err)
{ (void)io; (void)command; (void)feature; (void)err; return false; }
static void solar_os_shell_diag_unexpected(solar_os_shell_io_t *io, const char *command, const char *arg, const char *usage)
{ (void)io; (void)command; (void)arg; (void)usage; assert(false); }
static void solar_os_shell_diag_subcommand(solar_os_shell_io_t *io, const char *command, int argc,
    char **argv, const char *usage, const char *const *names, size_t count)
{ (void)io; (void)command; (void)argc; (void)argv; (void)usage; (void)names; (void)count; assert(false); }

#include "disk_command.inc"

static void run_mount(const char *name, const char *mount, const char *partition, bool image)
{
    solar_os_context_t ctx = 0;
    char *argv[] = {"disk", "mount", (char *)name, (char *)mount, (char *)partition};
    expected_volume = name; expected_mount = mount;
    expected_partition = partition ? (unsigned)(partition[0] - '0') : 0;
    volume_calls = image_calls = 0; output[0] = '\0';
    solar_os_shell_cmd_disk(&ctx, partition ? 5 : mount ? 4 : 3, argv);
    assert(volume_calls == (unsigned)!image && image_calls == (unsigned)image);
    assert(!strstr(output, "usage:"));
}

int main(void)
{
    run_mount("usb0", NULL, NULL, false);
    run_mount("usb0", "/usb0", NULL, false);
    volume_error = ESP_ERR_INVALID_STATE; /* A missing drive must reach storage, not image usage. */
    run_mount("usb0", NULL, NULL, false);
    assert(strstr(output, "disk mount failed: invalid state"));
    volume_error = ESP_OK;
    run_mount("flash", NULL, NULL, false);
    run_mount("sd0p1", "/sdcard", NULL, false);
    run_mount("usb0.img", "/mnt", NULL, true);
    run_mount("./usb0", "/mnt", NULL, true);
    run_mount("/sdcard/disk.img", "/mnt", "2", true);
    puts("Disk command volume and image routing: ok");
    return 0;
}
