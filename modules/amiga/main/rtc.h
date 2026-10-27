#pragma once
#include <stdint.h>
void solar_os_amiga_rtc_reset(void);
uint8_t solar_os_amiga_rtc_read(unsigned reg);
void solar_os_amiga_rtc_write(unsigned reg, uint8_t value);
uint64_t solar_os_amiga_clock_us(void);
bool solar_os_amiga_clock_calendar(uint16_t *year, uint8_t *fields);
