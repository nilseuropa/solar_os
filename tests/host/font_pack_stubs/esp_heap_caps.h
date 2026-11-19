#pragma once

#include "../esp_heap_caps.h"

/* Total PSRAM; font_packs_test switches it to test both memory modes. */
size_t heap_caps_get_total_size(uint32_t caps);
