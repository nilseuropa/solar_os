#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_display.h"
#include "solar_os_memory.h"
#include "solar_os_native_display_provider.h"
#include "solar_os_native_display_provider_abi.h"
#include "solar_os_native_handles.h"

static const solar_os_native_display_provider_api_v1_t *api;
static solar_os_native_handle_t registration;
static solar_os_display_target_t target;
static unsigned library_refs, driver_refs, frames, colors, modes, sleeps, maintenance, allocations;
static bool binding, presenting, fail_colors, busy_library;

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    assert(kind == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag);
    void *memory = calloc(count, size);
    if (memory) ++allocations;
    return memory;
}

void solar_os_memory_free(void *memory)
{
    if (memory) --allocations;
    free(memory);
}

int solar_os_native_library_retain(uint32_t lease, uint32_t *retained)
{
    assert(retained && !*retained);
    if (lease != 42) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    ++library_refs;
    *retained = 43;
    return SOLAR_OS_NATIVE_OK;
}

int solar_os_native_library_release(uint32_t lease)
{
    assert(lease == 43 && library_refs);
    if (busy_library) return SOLAR_OS_NATIVE_ERROR_BUSY;
    --library_refs;
    return SOLAR_OS_NATIVE_OK;
}

void *solar_os_native_driver_pin(void)
{
    if (!binding) return NULL;
    ++driver_refs;
    return &driver_refs;
}

void solar_os_native_driver_unpin(void *lease)
{
    assert(lease == &driver_refs && driver_refs);
    --driver_refs;
}

esp_err_t solar_os_display_register_target(const solar_os_display_target_t *value)
{
    if (target.u8g2) return ESP_ERR_INVALID_STATE;
    assert(u8g2_GetDisplayWidth(value->u8g2) == 33 && u8g2_GetDisplayHeight(value->u8g2) == 17);
    assert(value->native_width == 33 && value->native_height == 17 && value->ready && value->epaper);
    assert(!strcmp(value->name, "panel") && !strcmp(value->source, "native"));
    if (value->set_colors) {
        esp_err_t err = value->set_colors(value->color_context, 0x234567, 0x123456);
        if (err != ESP_OK) return err;
    }
    target = *value;
    return ESP_OK;
}

esp_err_t solar_os_display_unregister_target_owned(const char *name, const u8g2_t *canvas)
{
    if (target.u8g2 != canvas || strcmp(name, target.name)) return ESP_ERR_NOT_FOUND;
    if (presenting) return ESP_ERR_INVALID_STATE;
    memset(&target, 0, sizeof(target));
    return ESP_OK;
}

static int display_mono(void *user, const solar_os_native_display_mono_view_v1_t *frame)
{
    assert(user == &frames && (library_refs || driver_refs));
    assert(frame->width == 33 && frame->height == 17 && frame->stride == 40 && frame->size == 120);
    assert(frame->pixels[0] == 0x81 && frame->pixels[119] == 0x55);
    assert(api->close(registration) == SOLAR_OS_NATIVE_ERROR_BUSY);
    ++frames;
    return SOLAR_OS_NATIVE_OK;
}

static int display_colors(void *user, uint32_t foreground, uint32_t background)
{
    assert(user == &frames && (library_refs || driver_refs));
    assert(foreground == 0x234567 && background == 0x123456);
    ++colors;
    return fail_colors ? SOLAR_OS_NATIVE_ERROR_FAILED : SOLAR_OS_NATIVE_OK;
}

static int display_direct(void *user, const solar_os_display_raster_t *frame)
{
    assert(user == &frames && (library_refs || driver_refs) && frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565);
    ++frames;
    return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
}

static int display_surface(void *user, const solar_os_display_surface_t *surface)
{
    assert(user == &frames && (library_refs || driver_refs) && surface->format == SOLAR_OS_DISPLAY_FORMAT_INDEX8);
    surface->presented_hashes[0] = 123;
    return SOLAR_OS_NATIVE_OK;
}

static const char *display_mode(const void *user)
{ assert(user == &frames); return "fast"; }

static const char *display_choices(const void *user)
{ assert(user == &frames); return "fast slow"; }

static int display_mode_set(void *user, const char *mode)
{ assert(user == &frames && !strcmp(mode, "slow")); ++modes; return SOLAR_OS_NATIVE_OK; }

static int display_sleep(void *user, bool sleep)
{ assert(user == &frames && sleep); ++sleeps; return SOLAR_OS_NATIVE_OK; }

static int display_relax(void *user, solar_os_display_relax_phase_t phase)
{ assert(user == &frames && phase == SOLAR_OS_DISPLAY_RELAX_WHITE); ++maintenance; return SOLAR_OS_NATIVE_OK; }

int main(void)
{
    api = solar_os_native_display_provider_get_service(SOLAR_OS_NATIVE_DISPLAY_PROVIDER_SERVICE, 1, sizeof(*api));
    assert(api && api->abi_version == 1 && api->struct_size == sizeof(*api));
    solar_os_native_display_provider_v1_t configuration = {
        .name = "panel", .driver = "test", .width = 33, .height = 17, .preferred_stream_fps = 30,
        .flags = SOLAR_OS_NATIVE_DISPLAY_EPAPER | SOLAR_OS_NATIVE_DISPLAY_BLACK_IS_ONE,
        .surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT, .frame_formats = SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT,
        .callbacks = {
            .user = &frames, .present_mono = display_mono, .set_colors = display_colors,
            .present_frame = display_direct, .present_surface = display_surface,
            .get_mode = display_mode, .mode_choices = display_choices, .set_mode = display_mode_set,
            .relax = display_relax,
        },
    };
    assert(api->register_display(99, &configuration, &registration) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !registration);
    assert(api->register_display(0, &configuration, &registration) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !registration);
    fail_colors = true;
    assert(api->register_display(42, &configuration, &registration) == SOLAR_OS_NATIVE_ERROR_FAILED && !registration);
    assert(!target.u8g2 && !library_refs && !allocations);
    fail_colors = false;
    assert(!api->register_display(42, &configuration, &registration) && library_refs == 1);
    uint8_t *buffer = u8g2_GetBufferPtr(target.u8g2);
    buffer[0] = 0x81;
    buffer[119] = 0x55;
    presenting = true;
    u8x8_RefreshDisplay(u8g2_GetU8x8(target.u8g2));
    presenting = false;
    assert(frames == 1);
    const solar_os_display_raster_t frame = {.format = SOLAR_OS_DISPLAY_FORMAT_RGB565};
    assert(target.present_frame(target.frame_context, &frame) == ESP_ERR_TIMEOUT && frames == 2);
    uint32_t hash = 0;
    const solar_os_display_surface_t surface = {.format = SOLAR_OS_DISPLAY_FORMAT_INDEX8, .presented_hashes = &hash};
    assert(!target.present_surface(target.surface_context, &surface) && hash == 123);
    assert(!strcmp(target.controller_mode(target.controller_context), "fast"));
    assert(!strcmp(target.controller_mode_values(target.controller_context), "fast slow"));
    assert(!target.set_controller_mode(target.controller_context, "slow") && modes == 1);
    assert(!target.relax(target.relax_context, SOLAR_OS_DISPLAY_RELAX_WHITE) && maintenance == 1);

    busy_library = true;
    assert(api->close(registration) == SOLAR_OS_NATIVE_ERROR_BUSY && !target.u8g2 && library_refs == 1);
    binding = true;
    configuration.callbacks.sleep = display_sleep;
    solar_os_native_handle_t replacement;
    assert(!api->register_display(0, &configuration, &replacement) && driver_refs == 1);
    u8g2_SetPowerSave(target.u8g2, 1);
    assert(sleeps == 1 && colors == 3);
    busy_library = false;
    assert(!api->close(registration) && !library_refs && target.u8g2);
    assert(!api->close(replacement) && !driver_refs && !allocations && !solar_os_native_handle_count());
    return 0;
}
