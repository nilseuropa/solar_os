#pragma once

#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK 0ULL
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK 0ULL
#define SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK 0U

#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT 9
#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES { \
    { .driver = "manual", .name = "board0", .binding_count = 1, \
      .bindings = {{.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pin", .value = 0}} }, \
    { .driver = "manual", .name = "board1", .binding_count = 1, \
      .bindings = {{.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pin", .value = 1}} }, \
    { .driver = "manual", .name = "board2", .binding_count = 1, \
      .bindings = {{.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pin", .value = 2}} }, \
    { .driver = "manual", .name = "board3", .binding_count = 1, \
      .bindings = {{.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pin", .value = 3}} }, \
    { .driver = "manual", .name = "board4", .binding_count = 1, \
      .bindings = {{.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pin", .value = 4}} }, \
    { .driver = "manual", .name = "board5", .binding_count = 1, \
      .bindings = {{.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pin", .value = 5}} }, \
    { .driver = "manual", .name = "board6", .binding_count = 1, \
      .bindings = {{.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pin", .value = 6}} }, \
    { .driver = "manual", .name = "board7", .binding_count = 1, \
      .bindings = {{.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pin", .value = 7}} }, \
    { .driver = "test-flaky", .name = "board8", .binding_count = 1, \
      .bindings = {{.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pin", .value = 8}} }, \
}
