#pragma once
#include "solar_os_native_app_abi.h"
void solar_os_amiga_keyboard_reset(void);
void solar_os_amiga_keyboard_release(void);
void solar_os_amiga_keyboard_input(const solar_os_native_foreground_key_v1_t &key);
void solar_os_amiga_keyboard_tick(unsigned eclocks);
void solar_os_amiga_keyboard_ack(void);
bool solar_os_amiga_keyboard_receive(uint8_t serial);
