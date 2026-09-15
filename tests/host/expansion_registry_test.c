#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_board_caps.h"
#include "solar_os_buses.h"
#include "solar_os_expansion.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_memory.h"
#include "solar_os_pins.h"
#include "solar_os_resources.h"
#include "solar_os_stream.h"

static size_t live_allocations;
static size_t allocation_requests;
static solar_os_memory_class_t last_memory_class;
static bool fail_allocation;
static esp_err_t claim_result = ESP_OK;
static solar_os_resource_request_t last_claim_requests[SOLAR_OS_RESOURCE_BUNDLE_MAX];
static size_t last_claim_request_count;

size_t solar_os_gpio_controller_count(void)
{
    return 0U;
}

bool solar_os_gpio_controller_find(const char *name,
                                   solar_os_gpio_controller_info_t *info)
{
    (void)name;
    (void)info;
    return false;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0) {
        const size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

const char *esp_err_to_name(esp_err_t err)
{
    (void)err;
    return "error";
}

void *solar_os_memory_calloc(size_t count,
                             size_t size,
                             solar_os_memory_class_t memory_class,
                             const char *tag)
{
    assert(tag != NULL && strcmp(tag, "expansion.device") == 0);
    allocation_requests++;
    last_memory_class = memory_class;
    if (fail_allocation) {
        return NULL;
    }
    void *ptr = calloc(count, size);
    if (ptr != NULL) {
        live_allocations++;
    }
    return ptr;
}

void solar_os_memory_free(void *ptr)
{
    if (ptr != NULL) {
        assert(live_allocations > 0);
        live_allocations--;
    }
    free(ptr);
}

solar_os_board_capabilities_t solar_os_board_capabilities(void)
{
    return SOLAR_OS_BOARD_CAP_EXPANSION_GPIO;
}

bool solar_os_board_has(solar_os_board_capability_t capability)
{
    return capability == SOLAR_OS_BOARD_CAP_EXPANSION_GPIO;
}

bool solar_os_pin_is_direct_gpio(int pin)
{
    return pin >= 0 && pin < 64;
}

bool solar_os_pin_get_info_by_pin(int pin, solar_os_pin_info_t *info)
{
    (void)info;
    return solar_os_pin_is_direct_gpio(pin);
}

esp_err_t solar_os_resources_init(void)
{
    return ESP_OK;
}

esp_err_t solar_os_resource_claim_bundle(const solar_os_resource_request_t *requests,
                                         size_t request_count,
                                         const char *owner,
                                         solar_os_resource_conflict_t *conflict)
{
    (void)owner;
    (void)conflict;
    assert(request_count <= SOLAR_OS_RESOURCE_BUNDLE_MAX);
    memcpy(last_claim_requests, requests, request_count * sizeof(requests[0]));
    last_claim_request_count = request_count;
    return claim_result;
}

size_t solar_os_resource_release_owner(const char *owner)
{
    (void)owner;
    return 0;
}

esp_err_t solar_os_buses_init(void)
{
    return ESP_OK;
}

size_t solar_os_bus_count_protocol(solar_os_bus_protocol_t protocol)
{
    (void)protocol;
    return 0;
}

bool solar_os_bus_get_protocol(solar_os_bus_protocol_t protocol,
                               size_t index,
                               solar_os_bus_info_t *info)
{
    (void)protocol;
    (void)index;
    (void)info;
    return false;
}

bool solar_os_bus_find(const char *name,
                       solar_os_bus_protocol_t protocol,
                       solar_os_bus_info_t *info)
{
    (void)name;
    (void)protocol;
    (void)info;
    return false;
}

esp_err_t solar_os_bus_acquire(const char *name,
                               solar_os_bus_protocol_t protocol,
                               const char *owner)
{
    (void)name;
    (void)protocol;
    (void)owner;
    return ESP_OK;
}

size_t solar_os_bus_release_owner(const char *owner)
{
    (void)owner;
    return 0;
}

esp_err_t solar_os_stream_get_info(const char *id, solar_os_stream_info_t *info)
{
    (void)id;
    (void)info;
    return ESP_ERR_NOT_FOUND;
}

static solar_os_expansion_binding_t gpio_binding(int pin)
{
    solar_os_expansion_binding_t binding = {
        .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
        .value = pin,
    };
    strlcpy(binding.role, "pin", sizeof(binding.role));
    return binding;
}

static void assert_device(size_t index,
                          const char *name,
                          solar_os_expansion_origin_t origin,
                          bool autostart,
                          bool detachable)
{
    solar_os_expansion_device_t device;
    assert(solar_os_expansion_get_device(index, &device));
    assert(strcmp(device.name, name) == 0);
    assert(device.origin == origin);
    assert(device.autostart == autostart);
    assert(device.detachable == detachable);
    assert(device.active);
    assert(device.ready);
}

int main(void)
{
    const char *expected_categories[SOLAR_OS_EXPANSION_CATEGORY_COUNT] = {
        "Audio", "Display", "Input", "Power",
        "Radio", "Sensor", "Storage", "Utility",
    };
    for (solar_os_expansion_category_t category = SOLAR_OS_EXPANSION_CATEGORY_AUDIO;
         category < SOLAR_OS_EXPANSION_CATEGORY_COUNT;
         category++) {
        assert(strcmp(solar_os_expansion_category_name(category),
                      expected_categories[category]) == 0);
    }
    solar_os_expansion_driver_t manual_driver;
    assert(solar_os_expansion_get_driver(0, &manual_driver));
    assert(manual_driver.category == SOLAR_OS_EXPANSION_CATEGORY_UTILITY);

    assert(solar_os_expansion_init_early() == ESP_OK);
    assert(solar_os_expansion_device_count() == 0);

    assert(solar_os_expansion_init() == ESP_OK);
    assert(solar_os_expansion_device_count() == 9);
    assert(live_allocations == 9);
    assert(last_memory_class == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    for (size_t i = 0; i < 9; i++) {
        char name[16];
        snprintf(name, sizeof(name), "board%u", (unsigned)i);
        assert_device(i,
                      name,
                      SOLAR_OS_EXPANSION_ORIGIN_BOARD,
                      true,
                      false);
    }
    assert(solar_os_expansion_detach("board8") == ESP_ERR_NOT_SUPPORTED);

    solar_os_expansion_binding_t native_line = {
        .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE,
        .role = "power",
        .value = 23,
    };
    last_claim_request_count = 0U;
    assert(solar_os_expansion_attach("manual", "native-line", &native_line, 1) == ESP_OK);
    assert(last_claim_request_count == 1U);
    assert(last_claim_requests[0].kind == SOLAR_OS_RESOURCE_GPIO_PIN);
    assert(last_claim_requests[0].primary == 23);
    assert(last_claim_requests[0].secondary == -1);
    assert(solar_os_expansion_detach("native-line") == ESP_OK);

    for (int i = 0; i < 3; i++) {
        char name[16];
        snprintf(name, sizeof(name), "runtime%d", i);
        const solar_os_expansion_binding_t binding = gpio_binding(16 + i);
        assert(solar_os_expansion_attach("manual", name, &binding, 1) == ESP_OK);
    }
    assert(solar_os_expansion_device_count() == 12);
    assert(live_allocations == 12);
    assert_device(11,
                  "runtime2",
                  SOLAR_OS_EXPANSION_ORIGIN_RUNTIME,
                  false,
                  true);

    const solar_os_expansion_binding_t binding = gpio_binding(24);
    const size_t before_duplicate = live_allocations;
    assert(solar_os_expansion_attach("manual", "runtime2", &binding, 1) ==
           ESP_ERR_INVALID_STATE);
    assert(live_allocations == before_duplicate);

    fail_allocation = true;
    assert(solar_os_expansion_attach("manual", "no-memory", &binding, 1) ==
           ESP_ERR_NO_MEM);
    fail_allocation = false;
    assert(solar_os_expansion_device_count() == 12);

    claim_result = ESP_ERR_INVALID_STATE;
    assert(solar_os_expansion_attach("manual", "claim-failure", &binding, 1) ==
           ESP_ERR_INVALID_STATE);
    claim_result = ESP_OK;
    assert(live_allocations == 12);
    assert(solar_os_expansion_attach("manual", "claim-failure", &binding, 1) == ESP_OK);
    assert(solar_os_expansion_device_count() == 13);
    assert(solar_os_expansion_detach("claim-failure") == ESP_OK);
    assert(solar_os_expansion_device_count() == 12);
    assert(live_allocations == 12);

    assert(solar_os_expansion_detach("runtime1") == ESP_OK);
    assert(solar_os_expansion_device_count() == 11);
    assert(live_allocations == 11);
    assert(allocation_requests >= 16);

    puts("expansion registry tests passed");
    return 0;
}
