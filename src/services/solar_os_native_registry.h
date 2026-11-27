#pragma once
#include "solar_os_native_system_abi.h"

typedef struct {
    const char *name;
    uint32_t abi;
    uint32_t flags;
} native_service_descriptor_t;

static const native_service_descriptor_t native_service_descriptors[] = {
#include "generated/native_service_descriptors.inc"
};
