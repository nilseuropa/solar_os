#define SOLAR_OS_MAC_PORT_IMPLEMENTATION
#include "port.h"

const solar_os_native_host_api_v1_t *solar_os_mac_host;
const solar_os_native_foreground_api_v1_t *solar_os_mac_app;
const solar_os_native_memory_api_v1_t *solar_os_mac_memory;
jmp_buf solar_os_mac_trap;
const char *solar_os_mac_failure;
uint8_t solar_os_mac_pram[32];
bool solar_os_mac_pram_dirty;
static uint8_t *pixels;
static char pram_path[256];
static char snapshot_path[256];

SOLAR_OS_NATIVE_APP_MANIFEST = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
    .lifecycle_abi = SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_app_manifest_v2_t),
    .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI,
    .flags = SOLAR_OS_NATIVE_APP_RESUMABLE,
    .worker_stack_bytes = 16U * 1024U, .worker_priority = 1,
    .minimum_internal_bytes = 8U * 1024U,
    .minimum_psram_bytes = 5U * 1024U * 1024U,
    .tick_interval_ms = 5,
    .id = "mac", .summary = "Macintosh Plus emulator",
};

void solar_os_mac_report(const char *text) { solar_os_mac_host->write_utf8(text, strlen(text)); }
int solar_os_mac_quiet(const char *format, ...) { (void)format; return 0; }
_Noreturn void solar_os_mac_fail(const char *text)
{
    solar_os_mac_failure = text;
    longjmp(solar_os_mac_trap, 1);
}
void saveRtcMem(char *mem)
{
    memcpy(solar_os_mac_pram, mem, 32);
    solar_os_mac_pram_dirty = true;
}
static int load_rom(const char *path)
{
    uint32_t size = 0;
    int result = solar_os_mac_app->file_size(path, &size);
    if (result) return result;
    /* Some ROM dumps append a resource fork. Validate the full 128 KiB ROM
     * prefix before accepting it; no trailing bytes are mapped into the guest. */
    if (size < MAC_ROM_SIZE) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    for (uint32_t offset = 0; offset < MAC_ROM_SIZE; offset += 32768U) {
        size_t actual = 0;
        result = solar_os_mac_app->read_file(path, offset, solar_os_mac_rom + offset, 32768U, &actual);
        if (result || actual != 32768U) return result ? result : SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    if (!solar_os_mac_rom_valid(solar_os_mac_rom)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!memcmp(solar_os_mac_rom, "\x4d\x1f\x81\x72", 4)) {
        /* Verified Plus v3: omit the long power-on RAM-test loops, as in
         * esp32_mac. Guest RAM was allocated and cleared by this worker. */
        solar_os_mac_rom[0xe90] = solar_os_mac_rom[0xea8] = 0x4e;
        solar_os_mac_rom[0xe91] = solar_os_mac_rom[0xea9] = 0x71;
        solar_os_mac_rom[0xd92] = 0xb3; solar_os_mac_rom[0xd93] = 0x81;
    }
    return 0;
}
static void load_pram(const char *rom_path)
{
    /* Valid, offline Plus settings. Undefined port use (zero) makes System
     * probe LocalTalk during startup, which this module does not provide. */
    static const uint8_t defaults[32] = {
        [0x09] = 0x88, [0x0b] = 0x4c,
        [0x10] = 0xa8, [0x13] = 0x22,
        [0x14] = 0xcc, [0x15] = 0x0a, [0x16] = 0xcc, [0x17] = 0x0a,
        [0x1d] = 2, [0x1e] = 0x63,
    };
    memcpy(solar_os_mac_pram, defaults, 32);
    solar_os_mac_pram_dirty = true;
    pram_path[0] = 0;
    if (!rom_path) return;
    size_t length = strlen(rom_path);
    if (length + 6U >= sizeof(pram_path)) solar_os_mac_fail("ROM path too long for PRAM sidecar");
    memcpy(pram_path, rom_path, length);
    memcpy(pram_path + length, ".pram", 6);
    uint32_t size;
    int result = solar_os_mac_app->file_size(pram_path, &size);
    if (result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return;
    if (result || size != 32U) solar_os_mac_fail("PRAM file must contain exactly 32 bytes");
    size_t actual;
    if (solar_os_mac_app->read_file(pram_path, 0, solar_os_mac_pram, 32, &actual) || actual != 32U)
        solar_os_mac_fail("PRAM read failed");
    solar_os_mac_pram_dirty = solar_os_mac_pram[0x13] != 0x22;
    solar_os_mac_pram[0x13] = 0x22;
}
static int save_pram(void)
{
    if (!pram_path[0] || !solar_os_mac_pram_dirty) return 0;
    int result = solar_os_mac_app->replace_file(pram_path, solar_os_mac_pram, 32);
    if (!result) solar_os_mac_pram_dirty = false;
    return result;
}
static char *decimal(char *out, unsigned value)
{
    char digits[5]; unsigned n = 0;
    do { digits[n++] = '0' + value % 10U; value /= 10U; } while (value);
    while (n) *out++ = digits[--n];
    return out;
}
void solar_os_mac_snapshot(void)
{
    if (!snapshot_path[0]) { solar_os_mac_report("mac: snapshot requires a ROM path"); return; }
    char header[20] = "P4\n", *end = decimal(header + 3, solar_os_mac_width);
    *end++ = ' '; end = decimal(end, solar_os_mac_height); *end++ = '\n';
    size_t head = end - header, stride = (solar_os_mac_width + 7U) / 8U;
    size_t bytes = stride * solar_os_mac_height;
    uint8_t *picture = solar_os_mac_memory->alloc(head + bytes);
    if (!picture) { solar_os_mac_report("mac: not enough memory for snapshot"); return; }
    memcpy(picture, header, head);
    const uint8_t *fb = solar_os_mac_framebuffer();
    for (unsigned y = 0; y < solar_os_mac_height; ++y) {
        uint8_t *row = picture + head + y * stride;
        memcpy(row, fb + y * solar_os_mac_stride, stride);
        if (solar_os_mac_width & 7U) row[stride - 1] &= 0xffU << (8U - (solar_os_mac_width & 7U));
    }
    int result = solar_os_mac_app->replace_file(snapshot_path, picture, head + bytes);
    solar_os_mac_memory->free(picture);
    solar_os_mac_report(result ? "mac: snapshot save failed" : "mac: snapshot saved beside ROM (.pbm)");
}
static bool exchanged_frames;
static void run_guest(uint16_t width, uint16_t height)
{
    uint64_t now = solar_os_mac_app->time_us(), next_frame = now, next_mouse = now;
    while (!solar_os_mac_app->stop_requested()) {
        if (solar_os_mac_app->suspend_requested()) {
            solar_os_mac_input_reset();
            if (save_pram()) solar_os_mac_fail("PRAM save failed");
            int suspended = solar_os_mac_app->suspend();
            if (suspended) {
                if (solar_os_mac_app->stop_requested()) break;
                solar_os_mac_fail("foreground resume failed");
            }
            if (solar_os_mac_app->stop_requested()) break;
            solar_os_mac_input_reset();
            solar_os_mac_render_reset();
            next_frame = next_mouse = solar_os_mac_app->time_us();
        }
        for (unsigned i = 0; i < 64U; ++i) {
            solar_os_native_foreground_key_v1_t key;
            bool available;
            if (solar_os_mac_app->read_key(&key, &available)) solar_os_mac_fail("input read failed");
            if (!available) break;
            solar_os_mac_input(&key);
        }
        now = solar_os_mac_app->time_us();
        if (now >= next_mouse) {
            solar_os_mac_pointer_tick(); next_mouse = now + 16667U;
        }
        if (!solar_os_mac_paused) {
            uint64_t deadline = now + 8000U;
            for (unsigned i = 0; i < 64U; ++i) {
                solar_os_mac_step();
                if (solar_os_mac_app->time_us() >= deadline || solar_os_mac_app->stop_requested() ||
                    solar_os_mac_app->suspend_requested()) break;
            }
        }
        now = solar_os_mac_app->time_us();
        if (now >= next_frame) {
            if (solar_os_mac_render(pixels, width, height) &&
                (exchanged_frames ? solar_os_mac_app->video_exchange(&pixels) :
                 solar_os_mac_app->present(pixels, (size_t)width * height)))
                solar_os_mac_fail("display submission failed");
            next_frame = now + 40000U;
        }
        if (solar_os_mac_paused) solar_os_mac_app->sleep_ms(1);
        else solar_os_native_foreground_cooperate(solar_os_mac_app);
    }
}

int main(int argc, char **argv)
{
    solar_os_mac_host = solar_os_native_host_v1();
    if (!solar_os_mac_host || solar_os_mac_host->abi_version != 1U ||
        solar_os_mac_host->struct_size < sizeof(*solar_os_mac_host) || !solar_os_mac_host->get_service) return 1;
    solar_os_mac_app = solar_os_mac_host->get_service(SOLAR_OS_NATIVE_FOREGROUND_SERVICE, 1,
        offsetof(solar_os_native_foreground_api_v1_t, disk_close) + sizeof(solar_os_mac_app->disk_close));
    solar_os_mac_memory = solar_os_mac_host->get_service(SOLAR_OS_NATIVE_MEMORY_SERVICE, 1,
        offsetof(solar_os_native_memory_api_v1_t, alloc_internal));
    if (!solar_os_mac_app || !solar_os_mac_memory) {
        solar_os_mac_report("mac: compatible foreground and memory services required"); return 1;
    }
    bool diagnostic = argc == 2 && strlen(argv[1]) == 11U && !memcmp(argv[1], "--self-test", 11);
    /* strcmp is deliberately avoided: only the standard module imports are needed. */
    if (argc < 2 || argc > 4 || (argc == 4 &&
        (strlen(argv[2]) != 8 || memcmp(argv[2], "--floppy", 8)))) {
        solar_os_mac_report("mac: mac <macplus.rom> [scsi-hd.img | --floppy disk.img] or mac --self-test");
        solar_os_mac_app->set_exit_code(1); return 1;
    }
    solar_os_mac_failure = NULL;
    solar_os_mac_ram = solar_os_mac_rom = pixels = NULL;
    exchanged_frames = false;
    solar_os_mac_paused = false;
    solar_os_mac_pram_dirty = false;
    pram_path[0] = 0;
    snapshot_path[0] = 0;
    if (!setjmp(solar_os_mac_trap)) {
        char rom_path[256], disk_path[256];
        uint16_t width = 0, height = 0;
        if (solar_os_mac_app->video_dimensions(&width, &height) || !width || !height ||
            width > 1024U || height > 1024U) solar_os_mac_fail("display dimensions unsupported");
        if (!diagnostic && solar_os_mac_app->resolve_path(argv[1], rom_path, sizeof(rom_path)))
            solar_os_mac_fail("invalid ROM path");
        solar_os_mac_rom = solar_os_mac_memory->alloc(MAC_ROM_SIZE);
        solar_os_mac_ram = solar_os_mac_memory->alloc(MAC_RAM_SIZE);
        if (!solar_os_mac_rom || !solar_os_mac_ram) solar_os_mac_fail("not enough PSRAM for guest RAM and ROM");
        memset(solar_os_mac_ram, 0, MAC_RAM_SIZE);
        if (!diagnostic && load_rom(rom_path)) solar_os_mac_fail("need a raw 131072-byte Mac Plus v1/v2/v3 ROM with a valid checksum");
        if (!solar_os_mac_video_configure(width, height, !diagnostic))
            solar_os_mac_fail("native video needs a Plus v3 ROM and a 64..1024 pixel display in each dimension");
        if (diagnostic) solar_os_mac_diagnostic_rom(solar_os_mac_rom);
        load_pram(diagnostic ? NULL : rom_path);
        if (!diagnostic) {
            size_t length = strlen(rom_path);
            if (length + 5U >= sizeof(snapshot_path)) solar_os_mac_fail("ROM path too long for snapshot");
            memcpy(snapshot_path, rom_path, length);
            memcpy(snapshot_path + length, ".pbm", 5);
        }
        solar_os_mac_machine_init();
        solar_os_mac_input_reset();
        if (argc >= 3 && solar_os_mac_app->resolve_path(argv[argc - 1], disk_path, sizeof(disk_path)))
            solar_os_mac_fail("invalid disk path");
        if (solar_os_mac_disk_open(argc == 3 ? disk_path : NULL))
            solar_os_mac_fail("cannot claim SCSI disk image; check its size, writable storage and other mounts");
        if (solar_os_mac_floppy_open(argc == 4 ? disk_path : NULL))
            solar_os_mac_fail("floppy needs a Mac Plus v3 ROM and a writable raw 400/800 KiB image");
        uint16_t palette[256] = { [0] = 0xffff, [1] = 0x0000 };
        if (solar_os_mac_app->video_open_ex(width, height, palette, 25, 0)) solar_os_mac_fail("display setup failed");
        exchanged_frames = solar_os_native_foreground_has_exchange(solar_os_mac_app);
        if (exchanged_frames) {
            size_t bytes;
            if (solar_os_mac_app->video_acquire(&pixels, &bytes) || bytes != (size_t)width * height)
                solar_os_mac_fail("not enough PSRAM for display frame");
        } else {
            pixels = solar_os_mac_memory->alloc((size_t)width * height);
            if (!pixels) solar_os_mac_fail("not enough PSRAM for display frame");
        }
        if (solar_os_mac_app->enable_suspend()) solar_os_mac_fail("foreground switching unavailable");
        solar_os_mac_report(diagnostic ? "mac: diagnostic 68000 guest running" : "mac: Mac Plus running; F2 snapshot; F8 mouse mode; F10 pause; Ctrl+] exit");
        run_guest(width, height);
    }
    int result = solar_os_mac_failure ? 1 : 0;
    if (solar_os_mac_floppy_close()) { solar_os_mac_report("mac: floppy close or sync failed"); result = 1; }
    if (solar_os_mac_disk_close()) { solar_os_mac_report("mac: disk close or sync failed"); result = 1; }
    if (save_pram()) { solar_os_mac_report("mac: PRAM save failed"); result = 1; }
    if (solar_os_mac_failure) { solar_os_mac_report("mac: guest stopped"); solar_os_mac_report(solar_os_mac_failure); }
    if (pixels && !exchanged_frames) solar_os_mac_memory->free(pixels);
    if (solar_os_mac_ram) solar_os_mac_memory->free(solar_os_mac_ram);
    if (solar_os_mac_rom) solar_os_mac_memory->free(solar_os_mac_rom);
    pixels = solar_os_mac_ram = solar_os_mac_rom = NULL;
    solar_os_mac_app->set_exit_code(result);
    return result;
}
