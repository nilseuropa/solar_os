#pragma once
#include "solar_os_processor_provider_abi.h"
#define SOLAR_OS_HIMAX_PROVIDER_VERSION "1.2.0"
int solar_os_himax_model_slots(solar_os_processor_service_fn, const char *, uint32_t, char **);
int solar_os_himax_model_select(solar_os_processor_service_fn, const char *, uint32_t,
    uint32_t, solar_os_native_cancel_fn, void *, char **);
int solar_os_himax_model_upload(solar_os_processor_service_fn, const char *, const char *, uint32_t,
    uint32_t, uint32_t, solar_os_native_cancel_fn, solar_os_processor_progress_fn, void *, char **);
