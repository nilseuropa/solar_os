#include "epd_pixel_budget.h"

#include <string.h>

#define WHITE_BIT 0x80U
#define CYCLES_MASK 0x3fU
#define LISTED_BIT 0x40U

epd_pulse_tuning_t epd_pulse_default_tuning(void)
{
    return (epd_pulse_tuning_t){.drive_cycles = 1, .pulse_frames = 1,
        .waveform_fr = 8, .cleanup_every = 32, .cleanup_adaptive = false};
}

esp_err_t epd_pulse_tuning_set(epd_pulse_tuning_t *t, const char *mode)
{
    if (t == NULL || mode == NULL)
        return ESP_ERR_INVALID_ARG;
    if (strncmp(mode, "cleanup-policy=", 15) == 0) {
        if (strcmp(mode + 15, "adaptive") == 0)
            t->cleanup_adaptive = true;
        else if (strcmp(mode + 15, "fixed") == 0)
            t->cleanup_adaptive = false;
        else
            return ESP_ERR_INVALID_ARG;
        return ESP_OK;
    }
    struct option { const char *key; uint8_t *value; unsigned min, max; } options[] = {
        {"drive-cycles=", &t->drive_cycles, 1, 32},
        {"pulse-frames=", &t->pulse_frames, 1, 32},
        {"changed-drive-frames=", &t->pulse_frames, 1, 32},
        {"black-redrives=", &t->black_redrives, 0, 31},
        {"white-redrives=", &t->white_redrives, 0, 31},
        {"waveform-fr=", &t->waveform_fr, 1, 12},
        {"cleanup-every=", &t->cleanup_every, 1, 128},
    };
    for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
        const size_t n = strlen(options[i].key);
        if (strncmp(mode, options[i].key, n) != 0)
            continue;
        const char *p = mode + n;
        unsigned value = 0;
        if (*p == '\0')
            return ESP_ERR_INVALID_ARG;
        for (; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9' || value > options[i].max)
                return ESP_ERR_INVALID_ARG;
            value = value * 10U + (unsigned)(*p - '0');
        }
        if (value < options[i].min || value > options[i].max)
            return ESP_ERR_INVALID_ARG;
        *options[i].value = (uint8_t)value;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

uint8_t epd_pulse_target_cycles(const epd_pulse_tuning_t *t, bool white)
{
    return (uint8_t)(t->drive_cycles + (white ? t->white_redrives : t->black_redrives));
}

esp_err_t epd_pixel_budget_init(epd_pixel_budget_t *b, uint8_t *storage,
                                size_t pixels)
{
    if (b == NULL || storage == NULL || pixels == 0)
        return ESP_ERR_INVALID_ARG;
    *b = (epd_pixel_budget_t){.pixels = storage, .count = pixels};
    memset(storage, 0, pixels);
    return ESP_OK;
}

esp_err_t epd_pixel_budget_set_active_storage(epd_pixel_budget_t *b,
                                             uint32_t *storage, size_t entries)
{
    if (b == NULL || b->pixels == NULL || storage == NULL || entries < b->count ||
        b->count > UINT32_MAX || b->pending != 0 || b->active_count != 0)
        return ESP_ERR_INVALID_ARG;
    b->active = storage;
    return ESP_OK;
}

esp_err_t epd_pixel_budget_set_drive_history(epd_pixel_budget_t *b,
                                            uint16_t *storage, size_t entries)
{
    if (b == NULL || b->pixels == NULL || storage == NULL || entries < b->count ||
        b->count > SIZE_MAX / sizeof(*storage) || b->pending != 0 || b->active_count != 0)
        return ESP_ERR_INVALID_ARG;
    b->drive_history = storage;
    memset(storage, 0, b->count * sizeof(*storage));
    b->peak_drive_cycles = 0;
    return ESP_OK;
}

void epd_pixel_budget_reset(epd_pixel_budget_t *b)
{
    if (b == NULL || b->pixels == NULL)
        return;
    memset(b->pixels, 0, b->count);
    b->pending = 0;
    b->active_count = 0;
    if (b->drive_history != NULL)
        memset(b->drive_history, 0, b->count * sizeof(*b->drive_history));
    b->peak_drive_cycles = 0;
}

void epd_pixel_budget_seed(epd_pixel_budget_t *b, size_t p, bool white)
{
    if (b == NULL || p >= b->count)
        return;
    if ((b->pixels[p] & CYCLES_MASK) != 0)
        --b->pending;
    b->pixels[p] = (white ? WHITE_BIT : 0) | (b->pixels[p] & LISTED_BIT);
}

void epd_pixel_budget_target(epd_pixel_budget_t *b, size_t p, bool white,
                              uint8_t cycles)
{
    if (b == NULL || p >= b->count || cycles == 0 || cycles > CYCLES_MASK)
        return;
    const uint8_t color = white ? WHITE_BIT : 0;
    if ((b->pixels[p] & WHITE_BIT) == color)
        return; /* Same target retains its remaining budget, never refills it. */
    if ((b->pixels[p] & CYCLES_MASK) == 0)
        ++b->pending;
    uint8_t listed = b->pixels[p] & LISTED_BIT;
    if (b->active != NULL && listed == 0) {
        b->active[b->active_count++] = (uint32_t)p;
        listed = LISTED_BIT;
    }
    b->pixels[p] = color | listed | cycles;
}

epd_pixel_drive_t epd_pixel_budget_drive(const epd_pixel_budget_t *b, size_t p)
{
    if (b == NULL || p >= b->count ||
        (b->pixels[p] & CYCLES_MASK) == 0)
        return EPD_PIXEL_HOLD;
    return b->pixels[p] & WHITE_BIT ? EPD_PIXEL_DRIVE_WHITE :
                                    EPD_PIXEL_DRIVE_BLACK;
}

static void epd_pixel_budget_record_drive(epd_pixel_budget_t *b, size_t p)
{
    if (b->drive_history == NULL) return;
    if (b->drive_history[p] != UINT16_MAX) ++b->drive_history[p];
    if (b->drive_history[p] > b->peak_drive_cycles)
        b->peak_drive_cycles = b->drive_history[p];
}

void epd_pixel_budget_advance(epd_pixel_budget_t *b)
{
    if (b == NULL)
        return;
    if (b->active != NULL) {
        size_t keep = 0;
        for (size_t i = 0; i < b->active_count; ++i) {
            const size_t p = b->active[i];
            if ((b->pixels[p] & CYCLES_MASK) != 0) {
                epd_pixel_budget_record_drive(b, p);
                --b->pixels[p];
                if ((b->pixels[p] & CYCLES_MASK) == 0)
                    --b->pending;
            }
            if ((b->pixels[p] & CYCLES_MASK) != 0)
                b->active[keep++] = (uint32_t)p;
            else
                b->pixels[p] &= (uint8_t)~LISTED_BIT;
        }
        b->active_count = keep;
        return;
    }
    for (size_t p = 0; p < b->count; ++p) {
        if ((b->pixels[p] & CYCLES_MASK) != 0) {
            epd_pixel_budget_record_drive(b, p);
            --b->pixels[p];
            if ((b->pixels[p] & CYCLES_MASK) == 0)
                --b->pending;
        }
    }
}
