#include "solar_os_aogmaneo.h"
#include "solar_os_native.h"
#include <string.h>

bool solar_os_aogmaneo_active(const solar_os_aogmaneo_context_t *ctx)
{ return ctx && __atomic_load_n(&ctx->busy, __ATOMIC_ACQUIRE); }
static bool enter(solar_os_aogmaneo_context_t *ctx)
{ return ctx && !__atomic_exchange_n(&ctx->busy, 1, __ATOMIC_ACQUIRE); }
static int leave(solar_os_aogmaneo_context_t *ctx, int result)
{ __atomic_store_n(&ctx->busy, 0, __ATOMIC_RELEASE); return result; }
static solar_os_aogmaneo_entry_t *find(solar_os_aogmaneo_context_t *ctx, uint32_t token)
{
    if (ctx && token) for (unsigned i = 0; i < SOLAR_OS_AOGMANEO_MAX_MODELS; ++i)
        if (ctx->entries[i].token == token) return &ctx->entries[i];
    return NULL;
}
const solar_os_aogmaneo_info_t *solar_os_aogmaneo_info(solar_os_aogmaneo_context_t *ctx, uint32_t token)
{
    solar_os_aogmaneo_entry_t *entry = find(ctx, token);
    return entry && entry->model ? &entry->info : NULL;
}
static int close_entry(solar_os_aogmaneo_entry_t *entry)
{
    int result = 0;
    if (entry->model) {
        result = entry->api->destroy(entry->model);
        if (result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) result = 0;
        if (!result) entry->model = 0;
    }
    if (!result && entry->lease) {
        result = solar_os_native_library_release(entry->lease);
        if (!result) entry->lease = 0;
    }
    if (!result) memset(entry, 0, sizeof(*entry));
    return result;
}
static int acquire(solar_os_aogmaneo_context_t *ctx, solar_os_aogmaneo_entry_t **out)
{
    *out = NULL;
    if (ctx->next_token >= 0x3fffffffU) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    for (unsigned i = 0; i < SOLAR_OS_AOGMANEO_MAX_MODELS; ++i) {
        solar_os_aogmaneo_entry_t *entry = &ctx->entries[i];
        if (entry->token) continue;
        const void *table = NULL;
        entry->token = ++ctx->next_token;
        int result = solar_os_native_library_acquire(SOLAR_OS_AOGMANEO_LIBRARY,
            SOLAR_OS_AOGMANEO_ABI, sizeof(*entry->api), &entry->lease, &table);
        entry->api = table;
        if (!result && (!entry->api || !entry->api->create || !entry->api->destroy ||
            !entry->api->step || !entry->api->prediction || !entry->api->activations ||
            !entry->api->values || !entry->api->hidden || !entry->api->clear ||
            !entry->api->image_create || !entry->api->image_step || !entry->api->image_reconstruct))
            result = SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
        if (result) { (void)close_entry(entry); return result; }
        *out = entry;
        return 0;
    }
    return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
}
int solar_os_aogmaneo_create(solar_os_aogmaneo_context_t *ctx,
    const solar_os_aogmaneo_io_v1_t *inputs, size_t count,
    const solar_os_aogmaneo_layer_v1_t *layers, size_t layer_count, uint64_t seed, uint32_t *token)
{
    if (!token) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *token = 0;
    if (!inputs || !layers || !count || count > 8 || !layer_count || layer_count > 8)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!enter(ctx)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_aogmaneo_entry_t *entry;
    int result = acquire(ctx, &entry);
    if (!result) result = entry->api->create(inputs, count, layers, layer_count, seed, &entry->model);
    if (!result) {
        entry->info.input_count = count;
        entry->info.layer_count = layer_count;
        for (size_t i = 0; i < count; ++i) {
            entry->info.inputs[i] = inputs[i].size;
            entry->info.types[i] = inputs[i].type;
        }
        for (size_t i = 0; i < layer_count; ++i) entry->info.layers[i] = layers[i].size;
        *token = entry->token;
    } else if (entry) (void)close_entry(entry);
    return leave(ctx, result);
}
int solar_os_aogmaneo_image_create(solar_os_aogmaneo_context_t *ctx,
    solar_os_aogmaneo_size_v1_t hidden, const solar_os_aogmaneo_visible_v1_t *visible,
    size_t count, uint64_t seed, uint32_t *token)
{
    if (!token) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *token = 0;
    if (!visible || !count || count > 8) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!enter(ctx)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_aogmaneo_entry_t *entry;
    int result = acquire(ctx, &entry);
    if (!result) result = entry->api->image_create(hidden, visible, count, seed, &entry->model);
    if (!result) {
        entry->info.image = true;
        entry->info.input_count = count;
        entry->info.layer_count = 1;
        entry->info.layers[0] = hidden;
        for (size_t i = 0; i < count; ++i) entry->info.inputs[i] = visible[i].size;
        *token = entry->token;
    } else if (entry) (void)close_entry(entry);
    return leave(ctx, result);
}
int solar_os_aogmaneo_destroy(solar_os_aogmaneo_context_t *ctx, uint32_t token)
{
    if (!enter(ctx)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_aogmaneo_entry_t *entry = find(ctx, token);
    return leave(ctx, entry ? close_entry(entry) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
}
int solar_os_aogmaneo_close_all(solar_os_aogmaneo_context_t *ctx)
{
    if (!enter(ctx)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    int result = 0;
    for (unsigned i = 0; i < SOLAR_OS_AOGMANEO_MAX_MODELS; ++i) if (ctx->entries[i].token) {
        int one = close_entry(&ctx->entries[i]);
        if (!result) result = one;
    }
    return leave(ctx, result);
}
#define BEGIN_MODEL(image_kind) \
    if (!enter(ctx)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE; \
    solar_os_aogmaneo_entry_t *entry = find(ctx, token); \
    if (!entry || !entry->model) return leave(ctx, SOLAR_OS_NATIVE_ERROR_NOT_FOUND); \
    if (entry->info.image != (image_kind)) return leave(ctx, SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT)
int solar_os_aogmaneo_step(solar_os_aogmaneo_context_t *ctx, uint32_t token,
    const solar_os_aogmaneo_codes_v1_t *inputs, size_t count, bool learn, float reward, float mimic)
{
    BEGIN_MODEL(false);
    return leave(ctx, entry->api->step(entry->model, inputs, count, learn, reward, mimic));
}
int solar_os_aogmaneo_read(solar_os_aogmaneo_context_t *ctx, uint32_t token,
    unsigned kind, unsigned index, void *output, size_t count)
{
    BEGIN_MODEL(false);
    int result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    switch (kind) {
    case SOLAR_OS_AOGMANEO_READ_PREDICTION: result = entry->api->prediction(entry->model, index, output, count); break;
    case SOLAR_OS_AOGMANEO_READ_ACTIVATIONS: result = entry->api->activations(entry->model, index, output, count); break;
    case SOLAR_OS_AOGMANEO_READ_VALUES: result = entry->api->values(entry->model, index, output, count); break;
    case SOLAR_OS_AOGMANEO_READ_HIDDEN: result = entry->api->hidden(entry->model, index, output, count); break;
    }
    return leave(ctx, result);
}
int solar_os_aogmaneo_clear(solar_os_aogmaneo_context_t *ctx, uint32_t token)
{
    BEGIN_MODEL(false);
    return leave(ctx, entry->api->clear(entry->model));
}
int solar_os_aogmaneo_image_step(solar_os_aogmaneo_context_t *ctx, uint32_t token,
    const solar_os_aogmaneo_bytes_v1_t *inputs, size_t count, bool learn, bool reconstruction,
    int32_t *codes, size_t code_count)
{
    BEGIN_MODEL(true);
    return leave(ctx, entry->api->image_step(entry->model, inputs, count, learn, reconstruction, codes, code_count));
}
int solar_os_aogmaneo_image_reconstruct(solar_os_aogmaneo_context_t *ctx, uint32_t token,
    const int32_t *codes, size_t count, unsigned visible, uint8_t *output, size_t bytes)
{
    BEGIN_MODEL(true);
    return leave(ctx, entry->api->image_reconstruct(entry->model, codes, count, visible, output, bytes));
}
#undef BEGIN_MODEL
