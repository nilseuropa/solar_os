#include "solar_os_expansion.h"
#include "solar_os_buses.h"

static solar_os_expansion_binding_t captured[16];
static size_t captured_count;
esp_err_t solar_os_expansion_attach(const char *driver, const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    assert(!strcmp(driver, "pcnt-encoder") && !strcmp(name, "wheel") && count <= 16);
    memcpy(captured, bindings, count * sizeof(*bindings)); captured_count = count;
    return ESP_OK;
}
bool solar_os_expansion_find_uart_port(const char *name, solar_os_expansion_uart_port_t *port, size_t *index)
{ (void)name; (void)port; (void)index; return false; }
bool solar_os_bus_find(const char *name, solar_os_bus_protocol_t protocol, solar_os_bus_info_t *info)
{ (void)name; (void)protocol; (void)info; return false; }
static bool solar_os_key_parse(const char *text, uint8_t *key)
{ (void)text; (void)key; return false; }
static void check_attachment(void)
{
    assert(captured_count == 6);
    const char *roles[] = {"a", "b", "mode", "invert", "pull", "filter_ns"};
    const int values[] = {1, 2, 4, 1, 2, 1000};
    for (unsigned i = 0; i < 6; i++) {
        bool found = false;
        for (size_t j = 0; j < captured_count; j++) if (!strcmp(roles[i], captured[j].role)) {
            assert(captured[j].value == values[i] && captured[j].kind ==
                (i < 2 ? SOLAR_OS_EXPANSION_BINDING_GPIO : SOLAR_OS_EXPANSION_BINDING_PARAMETER));
            found = true;
        }
        assert(found);
    }
}
