#include "solar_os_shell_commands.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_inference.h"
#include "solar_os_model_bundle.h"
#include "solar_os_keys.h"
#if SOLAR_OS_PACKAGE_SERVICE_BLE
#include "solar_os_ble_keyboard.h"
#endif
#include <stdlib.h>
#include <string.h>

typedef struct { solar_os_context_t *ctx; solar_os_shell_io_t *io; bool cancelled; } model_client_t;
static bool cancelled(void *user)
{
    model_client_t *state = user;
    if (state->cancelled || state->ctx->exit_requested) return true;
    uint8_t keys[8]; size_t count = 0;
#if SOLAR_OS_PACKAGE_SERVICE_BLE
    while ((count = solar_os_ble_keyboard_read_chars((char *)keys, sizeof(keys))) != 0)
        for (size_t i = 0; i < count; ++i) if (keys[i] == SOLAR_OS_KEY_APP_EXIT) state->cancelled = true;
#endif
    if (state->io->kind == SOLAR_OS_SHELL_IO_KIND_PORT)
        do {
            count = 0;
            if (solar_os_port_read(&state->io->port, keys, sizeof(keys), 0, &count) != ESP_OK) break;
            for (size_t i = 0; i < count; ++i) if (keys[i] == 0x1d || keys[i] == SOLAR_OS_KEY_APP_EXIT) state->cancelled = true;
        } while (count);
    return state->cancelled;
}
static void print_ports(solar_os_shell_io_t *io, const char *side,
    const solar_os_inference_tensor_t *ports, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        const solar_os_inference_tensor_t *port = &ports[i];
        solar_os_shell_io_printf(io, "%s %s %s shape=[", side, port->name, solar_os_tensor_dtype_name(port->dtype));
        for (size_t j = 0; j < port->rank; ++j)
            solar_os_shell_io_printf(io, "%s%lu", j ? "," : "", (unsigned long)port->shape[j]);
        solar_os_shell_io_printf(io, "] bytes=%u exponents=[", (unsigned)port->bytes);
        for (size_t j = 0; j < port->exponent_count; ++j)
            solar_os_shell_io_printf(io, "%s%ld", j ? "," : "", (long)port->exponents[j]);
        solar_os_shell_io_writeln(io, "]");
    }
}
void solar_os_shell_cmd_model(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc < 2) {
        solar_os_shell_io_writeln(io, "model list | load <file.espdl> | bundle <bundle.json> [timeout_ms] | info <handle> | mode <handle> <single|auto|dual> | unload <handle|all>");
        return;
    }
    model_client_t state = {.ctx = ctx, .io = io}; solar_os_inference_t *client = NULL;
    esp_err_t error = solar_os_inference_create(cancelled, &state, &client);
    uint32_t handle = 0;
    if (error) goto done;
    if (!strcmp(argv[1], "list") && argc == 2) {
        uint32_t handles[SOLAR_OS_INFERENCE_MODELS_MAX]; size_t count;
        error = solar_os_inference_list(handles, SOLAR_OS_INFERENCE_MODELS_MAX, &count);
        if (error) goto done;
        if (!count) solar_os_shell_io_writeln(io, "No resident models");
        for (size_t i = 0; i < count; ++i) {
            const solar_os_inference_model_info_t *info;
            error = solar_os_inference_info(client, handles[i], &info); if (error) break;
            solar_os_shell_io_printf(io, "%lu %s %s refs=%lu\n", (unsigned long)handles[i],
                info->bundle ? info->bundle_id : "espdl", solar_os_inference_mode_name(info->mode),
                (unsigned long)info->references);
            solar_os_shell_io_write(io, "  "); solar_os_shell_io_writeln(io, info->path);
        }
    } else if ((!strcmp(argv[1], "load") || !strcmp(argv[1], "bundle")) && (argc == 3 || argc == 4)) {
        char path[256];
        uint32_t timeout = !strcmp(argv[1], "bundle") ? 60000 : 10000;
        if (argc == 4) {
            char *end = NULL; unsigned long value = strtoul(argv[3], &end, 10);
            error = ESP_ERR_INVALID_ARG;
            if (!*argv[3] || *end || value < 1 || value > 60000) goto done;
            timeout = value;
        }
        error = solar_os_shell_resolve_path(ctx, argv[2], path, sizeof(path));
        if (!error) error = !strcmp(argv[1], "bundle") ? solar_os_inference_load_bundle(client, path, timeout, &handle) :
            solar_os_inference_load(client, path, timeout, &handle);
        if (!error) solar_os_shell_io_printf(io, "Resident model %lu\n", (unsigned long)handle);
    } else if (!strcmp(argv[1], "unload") && argc == 3 && !strcmp(argv[2], "all")) {
        error = solar_os_inference_close_all(client);
    } else if (argc >= 3) {
        char *end = NULL; unsigned long id = strtoul(argv[2], &end, 10);
        error = ESP_ERR_INVALID_ARG;
        if (!*argv[2] || *end || id == 0 || id > 0x1fffffffUL) goto done;
        handle = id;
        if (!strcmp(argv[1], "unload") && argc == 3) error = solar_os_inference_close(client, handle);
        else if (!strcmp(argv[1], "mode") && argc == 4) {
            solar_os_inference_mode_t mode; error = solar_os_inference_mode_parse(argv[3], &mode);
            if (!error) error = solar_os_inference_set_mode(client, handle, mode);
        } else if (!strcmp(argv[1], "info") && argc == 3) {
            const solar_os_inference_model_info_t *info; error = solar_os_inference_info(client, handle, &info);
            if (!error) {
                solar_os_shell_io_printf(io, "handle=%lu backend=espdl version=%s mode=%s refs=%lu\n",
                    (unsigned long)handle, SOLAR_OS_INFERENCE_BACKEND_VERSION,
                    solar_os_inference_mode_name(info->mode), (unsigned long)info->references);
                solar_os_shell_io_write(io, "path="); solar_os_shell_io_writeln(io, info->path);
                solar_os_shell_io_printf(io, "bundle=%s\n", info->bundle ? info->bundle_id : "-");
                solar_os_shell_io_printf(io, "version=%s\n", info->bundle_version);
                solar_os_shell_io_printf(io, "inputs=%u outputs=%u model=%u internal=%u psram=%u bytes\n",
                    (unsigned)info->input_count, (unsigned)info->output_count, (unsigned)info->model_bytes,
                    (unsigned)info->internal_bytes, (unsigned)info->external_bytes);
            }
            if (!error) {
                print_ports(io, "input", info->inputs, info->input_count);
                print_ports(io, "output", info->outputs, info->output_count);
            }
        }
    } else error = ESP_ERR_INVALID_ARG;
done:
    solar_os_inference_destroy(client);
    if (error) solar_os_shell_diag_esp(io, "model", error, NULL, error == ESP_ERR_INVALID_STATE ? "Model is in use or inference is busy" : NULL);
}
