#pragma once
#include "solar_os_native_abi.h"
#include "solar_os_native_app_abi.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif
void *solar_os_amiga_alloc(size_t bytes);
void *solar_os_amiga_alloc_internal(size_t bytes);
void solar_os_amiga_cpu_prepare(void);
void solar_os_amiga_cpu_release(void);
int solar_os_amiga_beam_wait(uint32_t pc, const unsigned int *address_regs);
void solar_os_amiga_audio_sample(int16_t left, int16_t right);
#ifdef __cplusplus
}
#endif

/* The core's file-based tracing is disabled. Runtime media uses the host's
 * bounded read API; core allocations are tracked and released on every exit. */
#define heap_caps_malloc(bytes, caps) solar_os_amiga_alloc(bytes)
#define fprintf(...) ((void)0)
