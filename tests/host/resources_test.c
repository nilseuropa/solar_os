#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_resources.h"

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0U) {
        const size_t copy = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

int main(void)
{
    assert(solar_os_resources_init() == ESP_OK);

    /* The Freenove TFT board has 31 persistent claims before a user action. */
    for (int pin = 0; pin < 31; pin++) {
        char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
        snprintf(owner, sizeof(owner), "fixed:%d", pin);
        assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN,
                                       pin,
                                       -1,
                                       owner,
                                       "fixed") == ESP_OK);
    }

    const solar_os_resource_request_t adc_requests[] = {
        {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = 100,
            .secondary = -1,
            .label = "adc-gpio",
        },
        {
            .kind = SOLAR_OS_RESOURCE_ADC_PIN,
            .primary = 100,
            .secondary = -1,
            .label = "adc",
        },
    };
    assert(solar_os_resource_claim_bundle(adc_requests,
                                          sizeof(adc_requests) / sizeof(adc_requests[0]),
                                          "adc:100",
                                          NULL) == ESP_OK);
    assert(solar_os_resource_claim_count() == 33U);
    assert(solar_os_resource_release_owner("adc:100") == 2U);
    assert(solar_os_resource_claim_count() == 31U);

    puts("resource registry tests: ok");
    return 0;
}
