#pragma once
#include "host/ble_hs.h"
int ble_hs_id_gen_rnd(int, ble_addr_t *);
int ble_hs_id_set_rnd(const uint8_t *);
int ble_hs_id_copy_addr(uint8_t, uint8_t *, int *);
