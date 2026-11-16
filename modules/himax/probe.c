#include "solar_os_native_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_processor_provider_abi.h"
#include "solar_os_native_time_abi.h"
#include "solar_os_native_data_abi.h"

/* Development probe: exercise the installed ELF even on firmware that has a
 * built-in provider. Only success/failure markers leave the device. */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    if (!host || host->abi_version != 1 || host->struct_size < sizeof(*host)) return 1;
    const solar_os_native_library_service_api_v1_t *libraries = host->get_service("library", 1, sizeof(*libraries));
    const solar_os_native_memory_api_v1_t *memory = host->get_service("memory", 1, sizeof(*memory));
    const solar_os_native_time_api_v1_t *time = host->get_service("time", 1, sizeof(*time));
    const solar_os_native_json_api_v1_t *parser = host->get_service("json", 1, sizeof(*parser));
    if (!libraries || !memory || !time || !parser) return 2;
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t lease = 0; const void *table = NULL;
        int error = libraries->acquire("processor-himax", 1, sizeof(solar_os_processor_provider_api_v1_t), &lease, &table);
        if (error) return 3;
        const solar_os_processor_provider_api_v1_t *api = table;
        char *json = NULL;
        error = api->query(host->get_service, "camera0", "models", 4000, &json);
        if (!error && !json) error = -1;
        memory->free(json); json = NULL;
        if (!error) error = api->query(host->get_service, "camera0", "model", 4000, &json);
        solar_os_native_json_value_t *root = NULL; uint64_t slot = 0;
        if (!error) error = parser->parse(json, __builtin_strlen(json), &root);
        if (!error && !parser->uint_value(parser->get(parser->get(root, "model"), "id"), &slot)) error = -1;
        parser->free(root); memory->free(json); json = NULL;
        root = NULL;
        if (!error) error = api->query(host->get_service, "camera0", "slots", 4000, &json);
        if (!error) error = parser->parse(json, __builtin_strlen(json), &root);
        uint64_t live = 0;
        if (!error && (!parser->uint_value(parser->get(root, "active_slot"), &live) || live != slot ||
            parser->count(parser->get(root, "slots")) != 4)) error = -1;
        parser->free(root); memory->free(json); json = NULL;
        /* Select the existing slot: exercise the external management entry
         * point without modifying a model or changing the active selection. */
        if (!error) error = api->select(host->get_service, "camera0", slot, 4000, NULL, NULL, &json);
        memory->free(json); json = NULL;
        void *client = NULL;
        if (!error) error = api->open(host->get_service, "camera0", 0, 4000, NULL, NULL, &client);
        if (!error) {
            solar_os_processor_frame_v1_t frame = {0};
            error = api->capture(client, &frame, &json);
            if (!error && (!json || !frame.data || !frame.width || !frame.height || !frame.length)) error = -1;
            api->release_frame(client);
        }
        memory->free(json);
        if (client) while (api->close(client)) time->sleep_ms(20);
        libraries->release(lease);
        int unloaded = libraries->unload("processor-himax");
        if (error || unloaded) return 4;
    }
    static const char message[] = "HIMAX_LIBRARY_OK cycles=3 captures=3\n";
    return host->write_utf8(message, sizeof(message) - 1);
}
