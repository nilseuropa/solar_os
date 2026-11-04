#include "solar_os_shell_commands.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_camera.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"

static const char * const camera_subcommands[] = {"status", "capture", "orientation", "off"};
static const char * const orientation_values[] = {"normal", "mirror", "flip", "180"};
#define CAMERA_SHELL_OWNER "shell:camera"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static bool parse_frame_size(const char *text,
                             solar_os_camera_frame_size_t *frame_size)
{
    if (strcmp(text, "qvga") == 0) {
        *frame_size = SOLAR_OS_CAMERA_FRAME_SIZE_QVGA;
        return true;
    }
    if (strcmp(text, "vga") == 0) {
        *frame_size = SOLAR_OS_CAMERA_FRAME_SIZE_VGA;
        return true;
    }
    return false;
}

static void print_status(solar_os_shell_io_t *io)
{
    solar_os_camera_status_t status;
    esp_err_t error = solar_os_camera_get_status(&status);
    if (error == ESP_OK && !status.initialized && !status.owner_leased) {
        solar_os_camera_owner_t owner = {0};
        const solar_os_camera_config_t config = solar_os_camera_default_config();
        error = solar_os_camera_acquire(CAMERA_SHELL_OWNER, &owner);
        if (error == ESP_OK) {
            error = solar_os_camera_start(&owner, &config);
        }
        if (owner.generation != 0U) {
            const esp_err_t release_error = solar_os_camera_release_owner(&owner);
            if (error == ESP_OK) {
                error = release_error;
            }
        }
        if (error == ESP_OK) {
            error = solar_os_camera_get_status(&status);
        }
    }
    if (error != ESP_OK) {
        solar_os_shell_io_printf(io, "camera: probe failed: %s (0x%x)\r\n",
                                 esp_err_to_name(error), (unsigned)error);
        return;
    }
    solar_os_shell_io_printf(
        io,
        "driver=%s sensor=%s pid=0x%04x format=jpeg size=%s "
        "quality=%u orientation=%s buffers=%u storage=psram owner=%s frame=%s captures=%lu "
        "timeout=%ums last=%s\r\n",
        status.driver,
        status.sensor.name,
        (unsigned)status.sensor.product_id,
        solar_os_camera_frame_size_name(status.config.frame_size),
        (unsigned)status.config.jpeg_quality,
        solar_os_camera_orientation_name(status.orientation),
        (unsigned)status.sensor.frame_buffer_count,
        status.owner_leased ? status.owner : "-",
        status.frame_leased ? "leased" : "free",
        (unsigned long)status.capture_count,
        (unsigned)SOLAR_OS_CAMERA_CAPTURE_TIMEOUT_MS,
        esp_err_to_name(status.last_error));
}

static void capture(solar_os_context_t *ctx,
                    solar_os_shell_io_t *io,
                    const char *path_argument,
                    solar_os_camera_frame_size_t frame_size)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (!solar_os_shell_resolve_path_for_command(
            ctx, io, "camera capture", path_argument, path, sizeof(path))) {
        return;
    }

    solar_os_camera_config_t config = solar_os_camera_default_config();
    config.frame_size = frame_size;
    solar_os_camera_owner_t owner = {0};
    esp_err_t error = solar_os_camera_acquire(CAMERA_SHELL_OWNER, &owner);
    if (error != ESP_OK) {
        solar_os_camera_status_t status;
        if (error == ESP_ERR_INVALID_STATE &&
            solar_os_camera_get_status(&status) == ESP_OK &&
            status.owner_leased) {
            solar_os_shell_io_printf(io, "camera: busy: owned by %s\r\n",
                                     status.owner);
        } else {
            solar_os_shell_io_printf(io, "camera: acquire failed: %s (0x%x)\r\n",
                                     esp_err_to_name(error), (unsigned)error);
        }
        return;
    }
    error = solar_os_camera_start(&owner, &config);
    if (error != ESP_OK) {
        solar_os_shell_io_printf(io, "camera: start failed: %s (0x%x)\r\n",
                                 esp_err_to_name(error), (unsigned)error);
        (void)solar_os_camera_release_owner(&owner);
        return;
    }

    const solar_os_camera_frame_t *frame = NULL;
    error = solar_os_camera_capture(&owner, &frame);
    if (error != ESP_OK) {
        solar_os_shell_io_printf(io, "camera: capture failed: %s (0x%x)\r\n",
                                 esp_err_to_name(error), (unsigned)error);
        (void)solar_os_camera_release_owner(&owner);
        return;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        solar_os_shell_io_printf(io, "camera: cannot open %s: %s\r\n",
                                 path_argument, strerror(errno));
        (void)solar_os_camera_release_frame(&owner, frame);
        (void)solar_os_camera_release_owner(&owner);
        return;
    }
    const size_t written = fwrite(frame->data, 1U, frame->length, file);
    const int close_error = fclose(file);
    if (written != frame->length || close_error != 0) {
        const int write_errno = errno;
        (void)remove(path);
        solar_os_shell_io_printf(io, "camera: write failed for %s: %s\r\n",
                                 path_argument, strerror(write_errno));
        (void)solar_os_camera_release_frame(&owner, frame);
        (void)solar_os_camera_release_owner(&owner);
        return;
    }

    solar_os_shell_io_printf(io,
                             "captured %ux%u JPEG, %zu bytes -> %s\r\n",
                             (unsigned)frame->width,
                             (unsigned)frame->height,
                             frame->length,
                             path_argument);
    (void)solar_os_camera_release_frame(&owner, frame);
    (void)solar_os_camera_release_owner(&owner);
}

void solar_os_shell_cmd_camera(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) {
        print_status(io);
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "orientation") == 0) {
        const char *usage = "camera orientation [normal|mirror|flip|180]";
        if (argc > 3) {
            solar_os_shell_diag_unexpected(io, "camera orientation", argv[3], usage);
            return;
        }
        esp_err_t error = ESP_OK;
        if (argc == 3) {
            size_t index = 0U;
            while (index < sizeof(orientation_values) / sizeof(orientation_values[0]) &&
                   strcmp(argv[2], orientation_values[index]) != 0) index++;
            if (index == sizeof(orientation_values) / sizeof(orientation_values[0])) {
                solar_os_shell_diag_invalid(io, "camera orientation", "orientation",
                    argv[2], "normal, mirror, flip, or 180", usage, false);
                return;
            }
            error = solar_os_camera_set_orientation((solar_os_camera_orientation_t)index);
        }
        solar_os_camera_status_t status;
        if (error == ESP_OK) error = solar_os_camera_get_status(&status);
        if (error == ESP_OK && !status.backend_registered) error = ESP_ERR_NOT_SUPPORTED;
        if (error == ESP_OK) {
            solar_os_shell_io_printf(io, "orientation=%s\r\n",
                                     solar_os_camera_orientation_name(status.orientation));
        } else {
            solar_os_shell_io_printf(io, "camera: orientation failed: %s (0x%x)\r\n",
                                     esp_err_to_name(error), (unsigned)error);
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "off") == 0) {
        solar_os_camera_owner_t owner = {0};
        esp_err_t error = solar_os_camera_acquire(CAMERA_SHELL_OWNER, &owner);
        if (error == ESP_OK) {
            error = solar_os_camera_stop(&owner);
        }
        if (owner.generation != 0U) {
            const esp_err_t release_error = solar_os_camera_release_owner(&owner);
            if (error == ESP_OK) {
                error = release_error;
            }
        }
        if (error == ESP_OK) {
            solar_os_shell_io_writeln(io, "camera stopped");
        } else if (error == ESP_ERR_INVALID_STATE) {
            solar_os_camera_status_t status;
            if (solar_os_camera_get_status(&status) == ESP_OK &&
                status.owner_leased) {
                solar_os_shell_io_printf(io, "camera: busy: owned by %s\r\n",
                                         status.owner);
            } else {
                solar_os_shell_io_printf(io, "camera: stop failed: %s (0x%x)\r\n",
                                         esp_err_to_name(error), (unsigned)error);
            }
        } else {
            solar_os_shell_io_printf(io, "camera: stop failed: %s (0x%x)\r\n",
                                     esp_err_to_name(error), (unsigned)error);
        }
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "capture") == 0) {
        if (argc < 3) {
            solar_os_shell_diag_missing(
                io, "camera capture", "path",
                "camera capture <path> [qvga|vga]");
            return;
        }
        if (argc > 4) {
            solar_os_shell_diag_unexpected(
                io, "camera capture", argv[4],
                "camera capture <path> [qvga|vga]");
            return;
        }
        solar_os_camera_frame_size_t frame_size =
            SOLAR_OS_CAMERA_FRAME_SIZE_QVGA;
        if (argc == 4 && !parse_frame_size(argv[3], &frame_size)) {
            solar_os_shell_diag_invalid(
                io, "camera capture", "size", argv[3], "qvga or vga",
                "camera capture <path> [qvga|vga]", false);
            return;
        }
        capture(ctx, io, argv[2], frame_size);
        return;
    }
    solar_os_shell_diag_subcommand(
        io, "camera", argc, argv,
        "camera [status] | camera capture <path> [qvga|vga] | "
        "camera orientation [normal|mirror|flip|180] | camera off",
        camera_subcommands,
        sizeof(camera_subcommands) / sizeof(camera_subcommands[0]));
}
