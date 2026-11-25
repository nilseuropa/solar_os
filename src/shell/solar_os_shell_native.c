#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES

#include "solar_os_shell_commands.h"

#include "solar_os_native.h"
#include "solar_os_native_foreground.h"
#include "solar_os_native_storage.h"
#include "solar_os_input.h"
#include <stdio.h>
#include <string.h>
#include "solar_os_module_packages.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_storage.h"

static esp_err_t native_shell_write(const char *text, size_t text_len, void *user)
{
    solar_os_shell_io_t *io = (solar_os_shell_io_t *)user;
    if (io == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return solar_os_shell_io_write_len(io, text, text_len);
}

static esp_err_t native_shell_launch_foreground(solar_os_context_t *ctx,
                                        const char *command,
                                        const char *path,
                                        const solar_os_app_t *app,
                                        int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (argc >= 1 && argc <= SOLAR_OS_APP_ARG_MAX) {
        char *module_argv[SOLAR_OS_APP_ARG_MAX] = { (char *)path };
        for (int i = 1; i < argc; ++i) module_argv[i] = argv[i];
        err = solar_os_native_foreground_launch(ctx, app, argc, module_argv);
    }
    if (err == ESP_OK) solar_os_shell_session_prepare_foreground_launch(ctx, true);
    else solar_os_shell_diag_esp(io, command, err,
        "foreground apps require a display shell and at most seven arguments", NULL);
    return err;
}

static bool native_shell_cancelled(void *user)
{
    solar_os_context_t *ctx = user;
    if (ctx->exit_requested || ctx->sleep_requested || ctx->deep_sleep_requested) return true;
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    char keys[16]; size_t count = 0;
    if (solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        esp_err_t code = solar_os_port_read(&io->port, (uint8_t *)keys, sizeof(keys), 0, &count);
        if (code != ESP_OK && code != ESP_ERR_TIMEOUT) return true;
    } else count = solar_os_input_read_chars(keys, sizeof(keys));
    for (size_t i = 0; i < count; ++i)
        if ((uint8_t)keys[i] == SOLAR_OS_KEY_APP_EXIT || keys[i] == 0x1d || keys[i] == 0x03) return true;
    return false;
}

static esp_err_t native_shell_run(solar_os_shell_io_t *io,
                                  const char *command,
                                  const char *path,
                                  int argc,
                                  char **argv,
                                  solar_os_native_run_result_t *result,
                                  solar_os_context_t *ctx,
                                  int (*builtin_entry)(int, char **))
{
    char cwd[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_native_storage_context_t storage;
    esp_err_t prepared = solar_os_shell_resolve_path(ctx, NULL, cwd, sizeof(cwd));
    if (prepared == ESP_OK) prepared = solar_os_native_storage_init(&storage, cwd, native_shell_cancelled, ctx);
    if (prepared != ESP_OK) {
        solar_os_shell_diag_esp(io, command, prepared, "could not resolve shell working directory", NULL);
        return prepared;
    }
    const solar_os_native_run_options_t options = {
        .write = native_shell_write,
        .write_user = io,
        .get_service = solar_os_native_storage_get_service,
        .service_user = &storage,
    };
    char detail[128];
    esp_err_t err;
    if (builtin_entry) {
        int status = 0;
        err = solar_os_native_run_builtin(builtin_entry, argc, argv, &options, &status);
        if (err == ESP_OK && status) err = ESP_FAIL;
        snprintf(detail, sizeof(detail), "built-in command returned %d", status);
    } else err = solar_os_native_run(path,
                                              argc,
                                              argv,
                                              &options,
                                              result,
                                              detail,
                                              sizeof(detail));
    if (err != ESP_OK) {
        solar_os_shell_diag_esp(io, command, err, detail, NULL);
    }
    return err;
}

esp_err_t solar_os_shell_run_native_builtin(solar_os_context_t *ctx,
    int (*entry)(int, char **), int argc, char **argv)
{
    return native_shell_run(solar_os_shell_command_io(ctx), "cc", NULL,
                            argc, argv, NULL, ctx, entry);
}

void solar_os_shell_cmd_load(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc < 2) {
        solar_os_shell_diag_missing(io,
                                    "load",
                                    "ELF path",
                                    "load <file.elf> [args...]");
        return;
    }

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (!solar_os_shell_resolve_path_for_command(ctx,
                                                 io,
                                                 "load",
                                                 argv[1],
                                                 path,
                                                 sizeof(path))) {
        return;
    }

    char *module_argv[16] = {0};
    const int module_argc = argc - 1;
    if ((size_t)module_argc > sizeof(module_argv) / sizeof(module_argv[0])) {
        solar_os_shell_diag_problem(io,
                                    "load",
                                    "too many module arguments",
                                    "load <file.elf> [args...]",
                                    NULL);
        return;
    }
    module_argv[0] = path;
    for (int i = 2; i < argc; i++) {
        module_argv[i - 1] = argv[i];
    }

    const solar_os_app_t *app = NULL;
    char detail[160] = {0};
    const esp_err_t inspected = solar_os_native_foreground_resolve(path,
        &app, detail, sizeof(detail));
    if (inspected != ESP_OK) {
        solar_os_shell_diag_esp(io, "load", inspected, detail, NULL);
        return;
    }
    if (app != NULL) {
        (void)native_shell_launch_foreground(ctx, "load", path, app, module_argc, module_argv);
        return;
    }

    solar_os_native_run_result_t result;
    const esp_err_t err = native_shell_run(io,
                                           "load",
                                           path,
                                           module_argc,
                                           module_argv,
                                           &result,
                                           ctx,
                                           NULL);
    if (err != ESP_OK) {
        return;
    }

    solar_os_shell_io_printf(io,
                             "load: completed %s (%u bytes, %s, ABI %u)\n",
                             argv[1],
                             (unsigned)result.elf.file_size,
                             solar_os_native_elf_machine_name(result.elf.machine),
                             (unsigned)SOLAR_OS_NATIVE_ABI_VERSION);
}

bool solar_os_shell_try_native_module(solar_os_context_t *ctx,
                                      int argc,
                                      char **argv,
                                      bool *matched)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_storage_metadata_t metadata;

    if (matched != NULL) {
        *matched = false;
    }
    if (ctx == NULL || argc < 1 || argv == NULL || argv[0] == NULL) {
        return true;
    }
    const bool found = solar_os_module_package_path(SOLAR_OS_MODULE_TYPE_APP,
        argv[0], path, sizeof(path)) == ESP_OK &&
        solar_os_storage_stat(path, &metadata) == ESP_OK &&
        metadata.type == SOLAR_OS_STORAGE_ENTRY_FILE;
    if (!found) return true;

    if (matched != NULL) {
        *matched = true;
    }
    const solar_os_app_t *app = NULL;
    char detail[160] = {0};
    const esp_err_t inspected = solar_os_native_foreground_resolve(path,
        &app, detail, sizeof(detail));
    if (inspected != ESP_OK) {
        solar_os_shell_diag_esp(solar_os_shell_command_io(ctx), argv[0], inspected, detail, NULL);
        return true;
    }
    if (app != NULL) {
        return native_shell_launch_foreground(ctx, argv[0], path, app, argc, argv) != ESP_OK;
    }
    (void)native_shell_run(solar_os_shell_command_io(ctx),
                           argv[0],
                           path,
                           argc,
                           argv,
                           NULL,
                           ctx,
                           NULL);
    return true;
}

#endif /* SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES */
