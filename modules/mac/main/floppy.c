/* GPL-2.0-or-later. Host adapter for umac's replacement Sony driver. */
#define SOLAR_OS_MAC_PORT_IMPLEMENTATION
#include "port.h"
#include "vendor/sony/disc.h"

static bool opened, configured;
static uint32_t handle;
static const uint8_t driver[] = {
#include "vendor/sony/sonydrv.h"
};

void *solar_os_mac_guest_span(uint32_t address, size_t length)
{
    address &= 0xffffffU;
    if (address >= MAC_RAM_SIZE || length > MAC_RAM_SIZE - address)
        solar_os_mac_fail("floppy driver buffer outside guest RAM");
    return solar_os_mac_ram + address;
}
static int read_floppy(void *ctx, uint8_t *data, unsigned offset, unsigned length)
{
    (void)ctx;
    while (length) {
        unsigned n = length > 32768U ? 32768U : length;
        if (solar_os_mac_app->disk_read(handle, offset, data, n)) solar_os_mac_fail("floppy read failed");
        offset += n; data += n; length -= n;
        if (solar_os_mac_app->stop_requested()) solar_os_mac_fail("floppy transfer cancelled");
    }
    return 0;
}
static int write_floppy(void *ctx, uint8_t *data, unsigned offset, unsigned length)
{
    (void)ctx;
    while (length) {
        unsigned n = length > 32768U ? 32768U : length;
        if (solar_os_mac_app->disk_write(handle, offset, data, n)) solar_os_mac_fail("floppy write or sync failed");
        offset += n; data += n; length -= n;
        if (solar_os_mac_app->stop_requested()) solar_os_mac_fail("floppy transfer cancelled");
    }
    return 0;
}
void umac_disc_ejected(void) { solar_os_mac_report("mac: guest ejected the floppy; close to release its image"); }
bool solar_os_mac_floppy_hook(unsigned operation)
{
    if (!configured) return false;
    if (disc_pv_hook((uint8_t)operation)) solar_os_mac_fail("unknown floppy driver operation");
    return true;
}
int solar_os_mac_floppy_open(const char *path)
{
    opened = false;
    configured = false;
    /* Offsets apply only to Plus v3, whose complete checksum was validated
     * before any patch. Other ROM versions retain their original driver. */
    if (memcmp(solar_os_mac_rom, "\x4d\x1f\x81\x72", 4))
        return path ? SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED : 0;
    uint32_t size = 0;
    if (path) {
        int result = solar_os_mac_app->disk_open(path, &handle, &size);
        if (result) return result;
        opened = true;
        if (size != 409600U && size != 819200U) {
            (void)solar_os_mac_floppy_close(); return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        }
    }
    disc_descr_t discs[DISC_NUM_DRIVES] = {{
        .size = size, .op_read = read_floppy, .op_write = write_floppy
    }};
    disc_init(discs);
    configured = true;
    memcpy(solar_os_mac_rom + 0x17d30U, driver, sizeof(driver));
    /* The driver's final longword is its host-call address: $c00069. */
    size_t address = 0x17d30U + sizeof(driver) - 4U;
    solar_os_mac_rom[address] = 0; solar_os_mac_rom[address+1] = 0xc0;
    solar_os_mac_rom[address+2] = 0; solar_os_mac_rom[address+3] = 0x69;
    solar_os_mac_rom[0xd92] = 0xb3; solar_os_mac_rom[0xd93] = 0x81; /* checksum comparison */
    return 0;
}
int solar_os_mac_floppy_close(void)
{
    configured = false;
    if (!opened) return 0;
    opened = false;
    return solar_os_mac_app->disk_close(handle);
}
