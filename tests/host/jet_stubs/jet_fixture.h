#pragma once
#include "solar_os_jet.h"
void jet_test_reset(bool mono);
solar_os_gfx_t *jet_test_gfx(void);
void jet_test_assert_clean(void);
const uint8_t *jet_test_image(size_t *bytes);
