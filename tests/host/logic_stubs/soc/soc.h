#pragma once
#include <stdint.h>
uint32_t mock_gpio_read(unsigned reg);
#define REG_READ(reg) mock_gpio_read(reg)
