#pragma once
#include "solar_os_gfx.h"
#include "solar_os_tui_types_v1.h"

void solar_os_gfx_label(solar_os_gfx_t *gfx, int x, int baseline, int width, const char *text);
void solar_os_gfx_centered_label(solar_os_gfx_t *gfx, int x, int baseline, int width, const char *text);
void solar_os_gfx_input(solar_os_gfx_t *gfx, int x, int baseline, int width,
                         const char *text, solar_os_tui_input_state_t *state);
void solar_os_gfx_dialog(solar_os_gfx_t *gfx, int width, int height,
                          const char *title, const char *text,
                          solar_os_tui_input_state_t *state, const char *help);
