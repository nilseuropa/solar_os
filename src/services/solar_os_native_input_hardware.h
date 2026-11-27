#pragma once

#include "solar_os_native_input_hardware_abi.h"

const void *solar_os_native_input_hardware_get_service(const char *name, uint32_t abi,
                                                        uint32_t minimum);
