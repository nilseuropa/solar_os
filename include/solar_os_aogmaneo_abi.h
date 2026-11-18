#pragma once
/* The bundled AOgmaNeo implementation is Copyright (c) 2020-2025
 * Ogma Intelligent Systems Corp., licensed under CC BY-NC-SA 4.0:
 * https://creativecommons.org/licenses/by-nc-sa/4.0/
 * Source: https://github.com/ogmacorp/AOgmaNeo
 * Revision: 645a54ace656b0ac2476a56a0dac19faacbd87ab
 * SolarOS changes: 64-bit PCG state and allocation-safe arrays.
 * See vendor/AOGMANEO_LICENSE.md and vendor/LICENSE.md in the module source.
 */
#include "solar_os_native_service_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_AOGMANEO_LIBRARY "aogmaneo"
#define SOLAR_OS_AOGMANEO_ABI 1U
#define SOLAR_OS_AOGMANEO_MAX_MODELS 8U
#define SOLAR_OS_AOGMANEO_MAX_INPUTS 8U
#define SOLAR_OS_AOGMANEO_MAX_LAYERS 8U

enum {
    SOLAR_OS_AOGMANEO_INPUT = 0,
    SOLAR_OS_AOGMANEO_PREDICTION = 1,
    SOLAR_OS_AOGMANEO_ACTION = 2,
};

/* x/y: 1..64, z: 2..256 cells per column; x*y*z <= 65536.
 * Image visible layers also accept z=1. Column order is y + x*size.y;
 * dense image bytes use channel + z*(y + x*size.y). */
typedef struct { int32_t x, y, z; } solar_os_aogmaneo_size_v1_t;
typedef struct {
    solar_os_aogmaneo_size_v1_t size;
    int32_t type;
    int32_t dendrites; /* even, 2..16 */
    int32_t up_radius, down_radius; /* 0..8 */
    /* Used only for ACTION inputs. */
    int32_t value_size; /* 2..256 */
    int32_t value_dendrites; /* even, 2..16 */
    int32_t history_capacity; /* 2..512 */
} solar_os_aogmaneo_io_v1_t;
typedef struct {
    solar_os_aogmaneo_size_v1_t size;
    int32_t dendrites; /* even, 2..16 */
    int32_t up_radius, recurrent_radius, down_radius; /* 0..8; recurrent -1 disables */
} solar_os_aogmaneo_layer_v1_t;
typedef struct { const int32_t *data; size_t count; } solar_os_aogmaneo_codes_v1_t;
typedef struct { const uint8_t *data; size_t count; } solar_os_aogmaneo_bytes_v1_t;
typedef struct {
    solar_os_aogmaneo_size_v1_t size;
    int32_t radius; /* 0..8 */
} solar_os_aogmaneo_visible_v1_t;

/* Synchronous, single-thread execution on the caller's task. All provider
 * calls are serialized; overlapping/reentrant calls return INVALID_STATE.
 * Keep a library lease through destroy. Handles are nonzero, provider-owned
 * tokens. No C++ types, borrowed outputs or exceptions cross this table.
 * Creation copies descriptors; input/output buffers are borrowed only for a
 * call. Counts must match exactly. Codes are in [0, size.z).
 * At most 4,194,304 elements are permitted in each upstream weight array.
 * Invalid arguments leave models unchanged. An allocation/engine failure in
 * step invalidates that model: destroy it before recreating it.
 * Learning parameters use upstream defaults. Model persistence is not exposed.
 */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*create)(const solar_os_aogmaneo_io_v1_t *inputs, size_t input_count,
                  const solar_os_aogmaneo_layer_v1_t *layers, size_t layer_count,
                  uint64_t seed, uint32_t *model);
    int (*destroy)(uint32_t model); /* accepts hierarchy or image handles */
    int (*step)(uint32_t model, const solar_os_aogmaneo_codes_v1_t *inputs,
                size_t input_count, bool learn, float reward, float mimic);
    int (*prediction)(uint32_t model, uint32_t input, int32_t *codes, size_t count);
    int (*activations)(uint32_t model, uint32_t input, float *values, size_t count);
    int (*values)(uint32_t model, uint32_t input, float *values, size_t count);
    int (*hidden)(uint32_t model, uint32_t layer, int32_t *codes, size_t count);
    int (*clear)(uint32_t model); /* hierarchy temporal state; retains weights */
    int (*image_create)(solar_os_aogmaneo_size_v1_t hidden,
                        const solar_os_aogmaneo_visible_v1_t *visible,
                        size_t visible_count, uint64_t seed, uint32_t *model);
    int (*image_step)(uint32_t model, const solar_os_aogmaneo_bytes_v1_t *inputs,
                      size_t input_count, bool learn, bool learn_reconstruction,
                      int32_t *codes, size_t count);
    int (*image_reconstruct)(uint32_t model, const int32_t *codes, size_t count,
                             uint32_t visible, uint8_t *bytes, size_t byte_count);
} solar_os_aogmaneo_api_v1_t;

#ifdef __cplusplus
}
#endif
