#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "solar_os_cardkb_codec.h"
#include "solar_os_keys.h"

static void expect_key(uint8_t wire_value, uint8_t expected)
{
    uint8_t key = 0;
    assert(solar_os_cardkb_decode(wire_value, &key));
    assert(key == expected);
}

int main(void)
{
    uint8_t key = 0xffU;
    assert(!solar_os_cardkb_decode(0, &key));
    assert(!solar_os_cardkb_decode('a', NULL));

    expect_key('a', 'a');
    expect_key('\b', '\b');
    expect_key('\t', '\t');
    expect_key(0x1bU, SOLAR_OS_KEY_ESCAPE);
    expect_key('\r', SOLAR_OS_KEY_ENTER);
    expect_key(0x7fU, SOLAR_OS_KEY_DELETE);
    expect_key(180U, SOLAR_OS_KEY_LEFT);
    expect_key(181U, SOLAR_OS_KEY_UP);
    expect_key(182U, SOLAR_OS_KEY_DOWN);
    expect_key(183U, SOLAR_OS_KEY_RIGHT);

    for (uint16_t value = 128U; value <= 175U; value++) {
        assert(!solar_os_cardkb_decode((uint8_t)value, &key));
    }

    puts("CardKB codec tests: ok");
    return 0;
}
