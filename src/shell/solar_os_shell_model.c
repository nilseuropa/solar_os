#include "solar_os_shell_commands.h"
#include "solar_os_shell.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"
#include "solar_os_inference.h"
#include "solar_os_model_bundle.h"
#include "solar_os_keys.h"
#include "solar_os_module_packages.h"
#include "solar_os_storage.h"
#if SOLAR_OS_PACKAGE_JOB_MODEL_UPLOAD
#include "solar_os_processor.h"
#include "solar_os_memory.h"
#include "solar_os_jobs.h"
#include "solar_os_json.h"
#endif
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
        solar_os_shell_io_table_printf(io, "%s %s %s shape=[", side, port->name, solar_os_tensor_dtype_name(port->dtype));
        for (size_t j = 0; j < port->rank; ++j)
            solar_os_shell_io_table_printf(io, "%s%lu", j ? "," : "", (unsigned long)port->shape[j]);
        solar_os_shell_io_table_printf(io, "] bytes=%u exponents=[", (unsigned)port->bytes);
        for (size_t j = 0; j < port->exponent_count; ++j)
            solar_os_shell_io_table_printf(io, "%s%ld", j ? "," : "", (long)port->exponents[j]);
        solar_os_shell_io_table_writeln(io, "]");
    }
}
#if SOLAR_OS_PACKAGE_JOB_MODEL_UPLOAD
static esp_err_t print_slots(solar_os_shell_io_t *io, const char *json)
{
    solar_os_json_doc_t *doc = NULL;
    esp_err_t error = solar_os_json_parse_cstr(json, &doc);
    const solar_os_json_value_t *root = solar_os_json_root(doc);
    const solar_os_json_value_t *slots = solar_os_json_object_get(root, "slots");
    char device[129], path[SOLAR_OS_STORAGE_PATH_MAX], inventory[24];
    if (!error && (!solar_os_json_is_array(slots) ||
        solar_os_json_get_path_string(root, "device", device, sizeof(device)) ||
        solar_os_json_get_path_string(root, "inventory_path", path, sizeof(path)) ||
        solar_os_json_get_path_string(root, "inventory_state", inventory, sizeof(inventory)))) error = ESP_ERR_INVALID_RESPONSE;
    if (!error) {
        solar_os_shell_io_printf(io, "Device %s; inventory %s\n", device, inventory);
        solar_os_shell_io_table_writeln(io, "SLOT ACTIVE STATE       MODEL VERSION");
        for (size_t i = 0; i < solar_os_json_array_size(slots); ++i) {
            const solar_os_json_value_t *slot = solar_os_json_array_get(slots, i);
            const solar_os_json_value_t *record = solar_os_json_object_get(slot, "record");
            uint32_t number; bool active;
            char state[24] = "unknown", id[101] = "-", version[101] = "-";
            if (solar_os_json_get_path_uint32(slot, "slot", &number) ||
                solar_os_json_get_bool(solar_os_json_object_get(slot, "active"), &active)) { error = ESP_ERR_INVALID_RESPONSE; break; }
            if (solar_os_json_is_object(record) &&
                (solar_os_json_get_path_string(record, "state", state, sizeof(state)) ||
                 solar_os_json_get_path_string(record, "id", id, sizeof(id)) ||
                 solar_os_json_get_path_string(record, "version", version, sizeof(version)))) { error = ESP_ERR_INVALID_RESPONSE; break; }
            solar_os_shell_io_table_printf(io, "%lu    %-6s %-11s %s %s\n", (unsigned long)number, active ? "*" : "", state, id, version);
        }
        if (*path) solar_os_shell_io_printf(io, "%s\n", path);
        solar_os_shell_io_writeln(io, "Model identities are last recorded uploads; active selection is live.");
    }
    solar_os_json_free(doc); return error;
}
#endif
void solar_os_shell_cmd_model(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    if (argc < 2) {
        solar_os_shell_io_writeln(io, "model backend | list | load <file.espdl> | bundle <bundle.json> [timeout_ms] | info <handle> | mode <handle> <single|auto|dual> | unload <handle|all>");
#if SOLAR_OS_PACKAGE_JOB_MODEL_UPLOAD
        solar_os_shell_io_writeln(io, "model slots <processor> <source> | select <processor> <source> <slot> | upload <processor> <source> <bundle.json> <slot> [--replace]");
#endif
        return;
    }
    if (!strcmp(argv[1], "backend") && argc == 2) {
#if SOLAR_OS_PACKAGE_BACKEND_INFERENCE_ESPDL
        solar_os_shell_io_printf(io, "ESP-DL %s: built into firmware\n", SOLAR_OS_INFERENCE_BACKEND_VERSION);
#else
        char path[SOLAR_OS_STORAGE_PATH_MAX]; solar_os_storage_metadata_t metadata;
        bool installed = solar_os_module_package_path(SOLAR_OS_MODULE_TYPE_LIBRARY, "espdl", path, sizeof(path)) == ESP_OK &&
            solar_os_storage_stat(path, &metadata) == ESP_OK && metadata.type == SOLAR_OS_STORAGE_ENTRY_FILE;
        solar_os_shell_io_printf(io, "ESP-DL %s: library %s\n", SOLAR_OS_INFERENCE_BACKEND_VERSION,
            installed ? "installed" : "missing");
        if (!installed) solar_os_shell_io_writeln(io, "Install with: pkg install espdl");
#endif
        return;
    }
    model_client_t state = {.ctx = ctx, .io = io}; solar_os_inference_t *client = NULL;
#if SOLAR_OS_PACKAGE_JOB_MODEL_UPLOAD
    if (!strcmp(argv[1], "slots")) {
        char *json = NULL;
        esp_err_t error = argc == 4 ? solar_os_processor_query(argv[2], argv[3], "slots", &json) : ESP_ERR_INVALID_ARG;
        if (!error) error = print_slots(io, json);
        solar_os_memory_free(json);
        if (error) solar_os_shell_diag_esp(io, "model slots", error, NULL, "model slots <processor> <source>");
        return;
    }
    if ((!strcmp(argv[1], "select") && argc == 5) || (!strcmp(argv[1], "upload") && (argc == 6 || argc == 7))) {
        int slot_arg = !strcmp(argv[1], "select") ? 4 : 5;
        uint64_t slot = 0; esp_err_t error = ESP_ERR_INVALID_ARG;
        for (const char *p = argv[slot_arg]; *p; ++p) {
            if (*p < '0' || *p > '9' || slot > UINT32_MAX/10U) goto management_done;
            slot = slot*10 + *p-'0'; if (slot > UINT32_MAX) goto management_done;
        }
        if (!slot) goto management_done;
        if (slot_arg == 4) {
            char *json = NULL;
            error = solar_os_processor_select(argv[2], argv[3], slot, cancelled, &state, &json);
            if (!error) solar_os_shell_io_writeln(io, json);
            solar_os_memory_free(json);
        } else {
            char path[SOLAR_OS_STORAGE_PATH_MAX];
            error = solar_os_shell_resolve_path(ctx, argv[4], path, sizeof(path));
            if (!error) {
                char *args[] = {"model-upload", argv[2], argv[3], path, argv[5], argc == 7 ? argv[6] : NULL};
                error = solar_os_jobs_start(ctx, "model-upload", argc-1, args);
                if (!error) solar_os_shell_io_writeln(io, "Upload started; job status model-upload / job stop model-upload");
            }
        }
management_done:
        if (error) solar_os_shell_diag_esp(io, "model", error, NULL, slot_arg == 5 ?
            "Upload requires an explicit inactive slot; --replace authorizes replacing its contents" : "Select a supported device model slot");
        return;
    }
#endif
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
            solar_os_shell_io_table_printf(io, "%lu %s %s refs=%lu\n", (unsigned long)handles[i],
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
