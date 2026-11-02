#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_board_caps.h"

int main(void)
{
    const solar_os_board_info_t info = solar_os_board_info();
    assert(strcmp(info.id, "host") == 0);
    assert(strcmp(info.name, "Host test board") == 0);
    assert(strcmp(info.module, "") == 0);
    assert(info.psram_bytes == 8388608U);
    assert(info.capabilities == solar_os_board_capabilities());

    static const char expected[] =
        "psram display gfx cdc uart sd i2c spi rtc battery audio audio_input "
        "wifi ble gpio adc pwm expansion_gpio expansion_i2c expansion_spi "
        "expansion_uart expansion_adc expansion_pwm expansion_i2s key "
        "temperature humidity simd pointer streaming_display";
    char capabilities[SOLAR_OS_BOARD_CAPABILITIES_TEXT_MAX];

    assert(sizeof(expected) <= sizeof(capabilities));
    assert(solar_os_board_capabilities_format(capabilities,
                                              sizeof(capabilities)));
    assert(strcmp(capabilities, expected) == 0);

    char short_buffer[16];
    assert(!solar_os_board_capabilities_format(short_buffer,
                                               sizeof(short_buffer)));
    assert(strcmp(short_buffer, "psram display") == 0);

    puts("board_caps_test: ok");
    return 0;
}
