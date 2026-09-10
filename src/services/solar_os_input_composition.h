#pragma once

#include <stddef.h>
#include <stdint.h>

#include "solar_os_input.h"

typedef struct {
    uint8_t modifiers;
    uint8_t input_key;
    uint8_t output_key;
} solar_os_input_chord_t;

void solar_os_input_composition_set_modifiers(solar_os_input_source_t source,
                                              uint8_t modifiers);
uint8_t solar_os_input_composition_modifiers(void);
uint8_t solar_os_input_composition_apply(uint8_t key);
