#pragma once
#include "solar_os_aogmaneo_abi.h"

typedef struct {
    bool image;
    size_t input_count, layer_count;
    solar_os_aogmaneo_size_v1_t inputs[8], layers[8];
    int32_t types[8];
} solar_os_aogmaneo_info_t;
typedef struct {
    uint32_t token, lease, model;
    const solar_os_aogmaneo_api_v1_t *api;
    solar_os_aogmaneo_info_t info;
} solar_os_aogmaneo_entry_t;
/* One zero-initialized context per interpreter invocation, stored in its cold
 * state. Tokens are local to this context and never reused within a run.
 * Close every entry before freeing the context, including after an error.
 * active protects native calls against forced interpreter-task deletion. */
typedef struct {
    solar_os_aogmaneo_entry_t entries[SOLAR_OS_AOGMANEO_MAX_MODELS];
    uint32_t next_token;
    unsigned busy;
} solar_os_aogmaneo_context_t;
enum {
    SOLAR_OS_AOGMANEO_READ_PREDICTION,
    SOLAR_OS_AOGMANEO_READ_ACTIVATIONS,
    SOLAR_OS_AOGMANEO_READ_VALUES,
    SOLAR_OS_AOGMANEO_READ_HIDDEN,
};
bool solar_os_aogmaneo_active(const solar_os_aogmaneo_context_t *ctx);
const solar_os_aogmaneo_info_t *solar_os_aogmaneo_info(solar_os_aogmaneo_context_t *ctx, uint32_t token);
int solar_os_aogmaneo_create(solar_os_aogmaneo_context_t *ctx,
    const solar_os_aogmaneo_io_v1_t *inputs, size_t count,
    const solar_os_aogmaneo_layer_v1_t *layers, size_t layer_count, uint64_t seed, uint32_t *token);
int solar_os_aogmaneo_image_create(solar_os_aogmaneo_context_t *ctx,
    solar_os_aogmaneo_size_v1_t hidden, const solar_os_aogmaneo_visible_v1_t *visible,
    size_t count, uint64_t seed, uint32_t *token);
int solar_os_aogmaneo_destroy(solar_os_aogmaneo_context_t *ctx, uint32_t token);
int solar_os_aogmaneo_close_all(solar_os_aogmaneo_context_t *ctx);
int solar_os_aogmaneo_step(solar_os_aogmaneo_context_t *ctx, uint32_t token,
    const solar_os_aogmaneo_codes_v1_t *inputs, size_t count, bool learn, float reward, float mimic);
int solar_os_aogmaneo_read(solar_os_aogmaneo_context_t *ctx, uint32_t token,
    unsigned kind, unsigned index, void *output, size_t count);
int solar_os_aogmaneo_clear(solar_os_aogmaneo_context_t *ctx, uint32_t token);
int solar_os_aogmaneo_image_step(solar_os_aogmaneo_context_t *ctx, uint32_t token,
    const solar_os_aogmaneo_bytes_v1_t *inputs, size_t count, bool learn, bool reconstruction,
    int32_t *codes, size_t code_count);
int solar_os_aogmaneo_image_reconstruct(solar_os_aogmaneo_context_t *ctx, uint32_t token,
    const int32_t *codes, size_t count, unsigned visible, uint8_t *output, size_t bytes);
