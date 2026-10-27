// SPDX-License-Identifier: MIT
#include "rtc.h"
#include <string.h>

/* MSM6242B: sixteen nibble registers, low byte lane, four-byte spacing.
 * Calendar is seeded from host local time. Guest changes are session-local;
 * HOLD/STOP freeze the digits and never set the SolarOS system clock. */
static uint8_t digits[16];
static uint64_t last_us;
static unsigned value(unsigned reg) { return digits[reg] + 10U * digits[reg + 1]; }
static void set(unsigned reg, unsigned n) { digits[reg] = n % 10; digits[reg + 1] = n / 10; }
static unsigned hour(void)
{
    if (digits[15] & 4) return value(4);
    return (digits[4] + 10U * (digits[5] & 1)) % 12 + ((digits[5] & 4) ? 12 : 0);
}
static void set_hour(unsigned n)
{
    if (digits[15] & 4) set(4, n);
    else { unsigned h = n % 12; if (!h) h = 12; set(4, h); if (n >= 12) digits[5] |= 4; }
}
static void sync(void)
{
    uint64_t now = solar_os_amiga_clock_us();
    if ((digits[13] & 1) || (digits[15] & 3)) { last_us = now; return; }
    uint64_t elapsed = (now - last_us) / 1000000;
    if (!elapsed) return;
    last_us += elapsed * 1000000;
    uint64_t seconds = value(0) + 60U * value(2) + 3600U * hour() + elapsed;
    set(0, seconds % 60); set(2, seconds / 60 % 60); set_hour(seconds / 3600 % 24);
    unsigned day = value(6), month = value(8), year = value(10);
    if (!day || !month || month > 12) return; // software may be setting the digits
    static const uint8_t lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (uint64_t days = seconds / 86400; days; --days) {
        digits[12] = (digits[12] + 1) % 7;
        unsigned length = lengths[month - 1] + (month == 2 && year % 4 == 0);
        if (++day > length) { day = 1; if (++month > 12) { month = 1; year = (year + 1) % 100; } }
    }
    set(6, day); set(8, month); set(10, year);
}
void solar_os_amiga_rtc_reset(void)
{
    memset(digits, 0, sizeof(digits)); digits[15] = 4;
    uint16_t year = 1988;
    uint8_t fields[6] = {1,1,5,0,0,0}; // month, day, weekday, hour, minute, second
    if (!solar_os_amiga_clock_calendar(&year, fields)) {
        year = 1988; fields[0] = fields[1] = 1; fields[2] = 5;
        fields[3] = fields[4] = fields[5] = 0;
    }
    set(0, fields[5]); set(2, fields[4]); set(4, fields[3]);
    set(6, fields[1]); set(8, fields[0]); set(10, year % 100); digits[12] = fields[2];
    last_us = solar_os_amiga_clock_us();
}
uint8_t solar_os_amiga_rtc_read(unsigned reg) { sync(); return digits[reg & 15]; }
void solar_os_amiga_rtc_write(unsigned reg, uint8_t v)
{
    sync(); reg &= 15; v &= 15;
    if (reg == 13) v &= 9; // BUSY/IRQFLAG clear in this polled clock
    if (reg == 15 && ((v ^ digits[15]) & 4)) {
        unsigned h = hour(); digits[15] = v; set_hour(h);
    }
    digits[reg] = v;
    last_us = solar_os_amiga_clock_us();
}
