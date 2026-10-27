#include "port.h"
#include "m68k.h"
#include "ncr.h"
#include "via.h"
#include "scc.h"
#include <assert.h>
#include <stdio.h>

int solar_os_mac_main(int argc, char **argv);
void viaCbPortAWrite(unsigned int value);
#ifdef SOLAR_OS_MAC_TRACE
static uint32_t history[32], history_sp[32];
static unsigned trace_pos;
void m68k_instruction(void)
{
    uint32_t pc = m68k_get_reg(NULL, M68K_REG_PC);
    if (pc & 1) {
        for (unsigned i = trace_pos; i < trace_pos + 32; ++i) {
            uint32_t p = history[i % 32];
            fprintf(stderr, "TRACE pc=%08x op=%04x sp=%08x\n", p, m68k_read_memory_16(p), history_sp[i % 32]);
        }
        fprintf(stderr, "ODD PC=%08x sp=%08x\n", pc, m68k_get_reg(NULL, M68K_REG_A7));
        abort();
    }
    history[trace_pos % 32] = pc;
    history_sp[trace_pos++ % 32] = m68k_get_reg(NULL, M68K_REG_A7);
}
#endif
static unsigned allocations, alloc_attempt, fail_allocation, frames, disk_reads, disk_writes;
static unsigned sleeps, suspends, resumes, stop_after = 120;
static uint64_t clock_us, stop_deadline;
static int exit_code, display_error, disk_error;
static bool suspended_once, diagnostic, saw_guest_counter;
static uint8_t *last_frame;
static uint16_t frame_width, frame_height;
static uint16_t selected_width = 480, selected_height = 320;
static unsigned guest_row_bytes, guest_height, guest_width;
static uint32_t guest_screen, guest_ticks;
static const char *rom_file, *disk_file;
static FILE *disk;
static unsigned disk_closes;

static void *allocate(size_t n)
{
    if (++alloc_attempt == fail_allocation) return NULL;
    void *p = malloc(n);
    if (p) ++allocations;
    return p;
}
static void release(void *p) { assert(p && allocations); --allocations; free(p); }
static uint64_t now(void) { clock_us += 20; return clock_us; }
static void sleep_ms(uint32_t ms)
{
    clock_us += ms * 1000U; sleeps++;
    if (solar_os_mac_ram && solar_os_mac_ram[0x1003]) saw_guest_counter = true;
    if (solar_os_mac_ram) {
        guest_row_bytes = m68k_read_memory_16(0x106);
        guest_height = m68k_read_memory_16(0x838); guest_width = m68k_read_memory_16(0x83a);
        guest_screen = m68k_read_memory_32(0x824); guest_ticks = m68k_read_memory_32(0x16a);
    }
}
static bool stop(void) { return clock_us >= stop_deadline; }
static bool suspend_requested(void) { return !suspended_once && sleeps >= 30; }
static int suspend_guest(void)
{
    suspended_once = true; suspends++; resumes++;
    clock_us += 5000000; stop_deadline += 5000000;
    return 0;
}
static int enable_suspend(void) { return 0; }
static int dimensions(uint16_t *w, uint16_t *h) { *w = selected_width; *h = selected_height; return 0; }
static int video(uint16_t w, uint16_t h, const uint16_t *p, uint16_t fps, uint8_t bg)
{
    assert(w == selected_width && h == selected_height && p[0] == 0xffff && p[1] == 0 && fps == 25 && !bg);
    frame_width = w; frame_height = h;
    return display_error;
}
static int present(const uint8_t *p, size_t n)
{
    assert(n == (size_t)selected_width * selected_height);
    last_frame = realloc(last_frame, n); assert(last_frame);
    memcpy(last_frame, p, n);
    ++frames;
    if (!diagnostic && frames == stop_after) {
        printf("screen=%08x primary=%08x alt=%08x selected=%08x pc=%08x\n",
            m68k_read_memory_32(0x824), m68k_read_memory_32(MAC_SCREEN),
            m68k_read_memory_32(MAC_SCREEN_ALT), *(const uint32_t *)solar_os_mac_framebuffer(),
            m68k_get_reg(NULL, M68K_REG_PC));
        printf("ticks=%u time=%08x zone=%08x error=%04x\n", m68k_read_memory_32(0x16a),
            m68k_read_memory_32(0x20c), m68k_read_memory_32(0x118), m68k_read_memory_16(0xaf0));
        uint32_t pb = m68k_get_reg(NULL, M68K_REG_A0);
        printf("SR=%04x A0=%08x trap=%04x result=%04x ref=%04x\n", m68k_get_reg(NULL, M68K_REG_SR),
            pb, m68k_read_memory_16(pb+6), m68k_read_memory_16(pb+16), m68k_read_memory_16(pb+24));
    }
    return 0;
}
static uint8_t host_frames[2][480 * 320];
static unsigned host_writing;
static int acquire_frame(uint8_t **p, size_t *n)
{
    host_writing = 0; *p = host_frames[0];
    *n = (size_t)selected_width * selected_height; return 0;
}
static int exchange_frame(uint8_t **p)
{
    assert(*p == host_frames[host_writing]);
    present(*p, (size_t)selected_width * selected_height);
    *p = host_frames[host_writing ^= 1];
    memset(*p, 0xcd, (size_t)selected_width * selected_height);
    return 0;
}
static int read_key(solar_os_native_foreground_key_v1_t *key, bool *available)
{
    (void)key; *available = false; return 0;
}
static void set_exit(int code) { exit_code = code; }
static int resolve(const char *s, char *out, size_t n)
{
    if (strlen(s) + 1U > n) return -2;
    memcpy(out, s, strlen(s) + 1U); return 0;
}
static int file_size(const char *p, uint32_t *size)
{
    FILE *f = fopen(p, "rb");
    if (!f) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    fseek(f, 0, SEEK_END); *size = (uint32_t)ftell(f); fclose(f); return 0;
}
static int read_file(const char *p, uint32_t off, void *out, size_t n, size_t *got)
{
    assert(n <= 65536);
    FILE *f = fopen(p, "rb"); if (!f) return -5;
    fseek(f, off, SEEK_SET); *got = fread(out, 1, n, f); fclose(f); return 0;
}
static int replace_file(const char *p, const void *data, size_t n)
{
    (void)p; (void)data; assert(n == 32); return 0; /* Never write beside owned ROMs. */
}
static int disk_open(const char *p, uint32_t *handle, uint32_t *size)
{
    assert(!disk); (void)p;
    if (disk_error) return disk_error;
    disk = tmpfile(); assert(disk);
    FILE *source = disk_file ? fopen(disk_file, "rb") : NULL;
    if (source) {
        uint8_t buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), source))) assert(fwrite(buf, 1, n, disk) == n);
        fclose(source);
    } else { uint8_t sector[512] = {0}; for (int i = 0; i < 1600; ++i) fwrite(sector, 1, 512, disk); }
    fseek(disk, 0, SEEK_END); *size = (uint32_t)ftell(disk); *handle = 1; return 0;
}
static int disk_read(uint32_t handle, uint32_t off, void *p, size_t n)
{
    assert(handle == 1 && disk && n <= 65536); disk_reads++;
    fseek(disk, off, SEEK_SET); return fread(p, 1, n, disk) == n ? 0 : -1;
}
static int disk_write(uint32_t handle, uint32_t off, const void *p, size_t n)
{
    assert(handle == 1 && disk && n <= 65536); disk_writes++;
    fseek(disk, off, SEEK_SET); return fwrite(p, 1, n, disk) == n ? 0 : -1;
}
static int disk_close(uint32_t handle)
{
    assert(handle == 1 && disk); fclose(disk); disk = NULL; disk_closes++; return 0;
}
static int report(const char *p, size_t n) { fwrite(p, 1, n, stdout); putchar('\n'); return 0; }
static solar_os_native_foreground_api_v1_t app = {
    .abi_version = 1, .struct_size = sizeof(app),
    .time_us = now, .sleep_ms = sleep_ms, .stop_requested = stop,
    .suspend_requested = suspend_requested, .suspend = suspend_guest,
    .enable_suspend = enable_suspend, .video_dimensions = dimensions,
    .video_open_ex = video, .present = present, .read_key = read_key,
    .set_exit_code = set_exit, .resolve_path = resolve, .file_size = file_size,
    .read_file = read_file, .replace_file = replace_file,
    .disk_open = disk_open, .disk_read = disk_read, .disk_write = disk_write, .disk_close = disk_close,
};
static const solar_os_native_memory_api_v1_t memory = { .abi_version = 1,
    .struct_size = sizeof(memory), .alloc = allocate, .free = release };
static const void *get_service(const char *s, uint32_t abi, uint32_t size)
{
    assert(abi == 1);
    if (!strcmp(s, "foreground")) { assert(size <= app.struct_size); return &app; }
    if (!strcmp(s, "memory")) { assert(size <= sizeof(memory)); return &memory; }
    return NULL;
}
static const solar_os_native_host_api_v1_t host = { .abi_version = 1,
    .struct_size = sizeof(host), .write_utf8 = report, .get_service = get_service };
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &host; }

static void reset(void)
{
    assert(!allocations && !disk);
    alloc_attempt = frames = sleeps = suspends = resumes = clock_us = 0;
    stop_deadline = (uint64_t)stop_after * 40000U;
    fail_allocation = display_error = disk_error = 0;
    suspended_once = saw_guest_counter = false;
}
static void scsi(const uint8_t *cmd, unsigned n)
{
    ncrInit();
    assert(!solar_os_mac_disk_open("test.img"));
    ncrWrite(0, 0, 0xc0);
    ncrWrite(2, 0, 1); /* arbitrate */
    ncrWrite(1, 0, 13); ncrWrite(1, 0, 5); ncrWrite(1, 0, 1);
    ncrWrite(3, 0, 2); /* command phase */
    for (unsigned i = 0; i < n; ++i) {
        ncrWrite(0, 0, cmd[i]);
        ncrWrite(1, 0, 17); ncrWrite(1, 0, 1);
    }
    ncrWrite(3, 0, 1); /* data in */
}
static void storage_and_bus_tests(void)
{
    reset();
    static const uint8_t capacity[10] = {0x25};
    scsi(capacity, sizeof(capacity));
    ncrWrite(2, 0, 2); /* DMA reads */
    uint32_t last = 0;
    for (unsigned i = 0; i < 4; ++i) last = (last << 8) | ncrRead(0, 1);
    assert(last == 1599); /* 800 KiB, last LBA rather than block count */
    assert(!solar_os_mac_disk_close());
    unsigned reads = disk_reads;
    static const uint8_t too_large[6] = {8,0,0,0,65,0};
    scsi(too_large, sizeof(too_large));
    assert(disk_reads == reads);
    ncrWrite(3, 0, 3); assert(ncrRead(0, 0) == 2);
    assert(!solar_os_mac_disk_close());
    static const uint8_t past_end[10] = {0x28,0,0xff,0xff,0xff,0xff,0,0,1,0};
    scsi(past_end, sizeof(past_end));
    assert(disk_reads == reads);
    assert(!solar_os_mac_disk_close());

    solar_os_mac_ram = calloc(1, MAC_RAM_SIZE); solar_os_mac_rom = malloc(MAC_ROM_SIZE);
    solar_os_mac_diagnostic_rom(solar_os_mac_rom);
    solar_os_mac_machine_init();
    /* T2 raises one interrupt per arm, including after its 16-bit wrap. */
    viaStep(70000); assert(!(viaRead(13) & 0x20));
    viaWrite(8, 5); viaWrite(9, 0); viaStep(6);
    assert(viaRead(13) & 0x20);
    (void)viaRead(8); viaStep(65536);
    assert(!(viaRead(13) & 0x20));
    viaWrite(6, 0x34); viaWrite(7, 0x12);
    assert(viaRead(7) == 0x12 && (viaRead(14) & 0x80));
    /* Clear external SCC status when acknowledged. A stale level 2 must
     * not permanently hide the lower-priority VIA interrupt. */
    sccWrite(2, 1); sccWrite(2, 1);
    sccWrite(2, 15); sccWrite(2, 8);
    sccWrite(2, 9); sccWrite(2, 8);
    sccSetDcd(SCC_CHANA, 0);
    sccWrite(2, 3); assert(sccRead(2) & 8);
    sccWrite(2, 0x10);
    sccWrite(2, 3); assert(!(sccRead(2) & 8));
    solar_os_mac_input_reset();
    solar_os_native_foreground_key_v1_t key = { .usage = 4 };
    solar_os_mac_input(&key); viaWrite(10, 0x10); assert(viaRead(10) == 1);
    key.action = SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
    solar_os_mac_input(&key); viaWrite(10, 0x10); assert(viaRead(10) == 0x81);
    key = (solar_os_native_foreground_key_v1_t){ .usage = 0x43 };
    solar_os_mac_paused = false;
    solar_os_mac_input(&key); assert(solar_os_mac_paused);
    key.action = SOLAR_OS_NATIVE_FOREGROUND_KEY_REPEAT;
    solar_os_mac_input(&key); assert(solar_os_mac_paused);
    key.action = SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS;
    solar_os_mac_input(&key); assert(!solar_os_mac_paused);
    unsigned first = m68k_read_memory_32(0x400000);
    m68k_write_memory_32(0x400000, 0xdeadbeef);
    assert(m68k_read_memory_32(0x400000) == first);
    for (unsigned i = 0; i < 500; ++i) solar_os_mac_step();
    assert(m68k_read_memory_32(0x1000));
    m68k_write_memory_32(MAC_RAM_SIZE - 2, 0x11223344);
    assert(solar_os_mac_ram[MAC_RAM_SIZE - 2] == 0x11 && solar_os_mac_ram[MAC_RAM_SIZE - 1] == 0x22);
    free(solar_os_mac_ram); free(solar_os_mac_rom);
    solar_os_mac_ram = solar_os_mac_rom = NULL;
    disk_closes = disk_reads = disk_writes = 0;
}

/* Each guest bit must become exactly one display pixel, including padding. */
static void reference_render(uint8_t *out, unsigned width, unsigned height)
{
    const uint8_t *fb = solar_os_mac_framebuffer();
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
            out[y * width + x] = (fb[y * solar_os_mac_stride + x / 8U] >> (7U - (x & 7U))) & 1U;
        }
}
static void render_tests(void)
{
    static const uint16_t sizes[][2] = {
        {400,300}, {480,320}, {320,480}, {512,342}, {800,600}, {1024,1024},
        {321,243}, {64,64}, {64,1024}, {1024,64},
    };
    solar_os_mac_ram = calloc(1, MAC_RAM_SIZE); assert(solar_os_mac_ram);
    viaCbPortAWrite(0); /* primary screen, overlay disabled */
    uint32_t random = 1234;
    for (unsigned i = 0; i < MAC_RAM_SIZE; ++i) {
        random = random * 1664525U + 1013904223U;
        solar_os_mac_ram[i] = random >> 24;
    }
    uint8_t *out = malloc(1024 * 1024), *expected = malloc(1024 * 1024);
    assert(out && expected);
    solar_os_mac_render_reset();
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        unsigned w = sizes[i][0], h = sizes[i][1];
        assert(solar_os_mac_video_configure(w, h, false));
        memset(out, 0xcc, w * h);
        assert(solar_os_mac_render(out, w, h));
        reference_render(expected, w, h);
        assert(!memcmp(out, expected, w * h));
        assert(!solar_os_mac_render(out, w, h));
        assert(!memcmp(out, expected, w * h));
    }
    /* Scan the complete packed screen, including a single-bit tail change. */
    assert(solar_os_mac_video_configure(512, 342, false));
    assert(solar_os_mac_render(out, 512, 342));
    solar_os_mac_ram[MAC_SCREEN + MAC_WIDTH * MAC_HEIGHT / 8U - 1] ^= 1;
    assert(solar_os_mac_render(out, 512, 342));
    reference_render(expected, 512, 342);
    assert(!memcmp(out, expected, 512 * 342));
    assert(!solar_os_mac_render(out, 512, 342));
    /* Switching screen pages and resetting after resume must redraw. */
    viaCbPortAWrite(0x40);
    memset((uint8_t *)solar_os_mac_framebuffer(), 0, MAC_WIDTH * MAC_HEIGHT / 8U);
    assert(solar_os_mac_render(out, 512, 342));
    for (unsigned i = 0; i < 512 * 342; ++i) assert(!out[i]);
    assert(!solar_os_mac_render(out, 512, 342));
    solar_os_mac_render_reset();
    memset(out, 0xcc, 512 * 342);
    assert(solar_os_mac_render(out, 512, 342));
    for (unsigned i = 0; i < 512 * 342; ++i) assert(!out[i]);
    /* A solid guest fills every SolarTerm pixel, including the old bars. */
    assert(solar_os_mac_video_configure(400, 300, false));
    memset((uint8_t *)solar_os_mac_framebuffer(), 0xff, solar_os_mac_stride * solar_os_mac_height);
    assert(solar_os_mac_render(out, 400, 300));
    for (unsigned i = 0; i < 400 * 300; ++i) assert(out[i] == 1);
    assert(!solar_os_mac_render(out, 400, 300));
    assert(!solar_os_mac_video_configure(63, 300, false));
    assert(!solar_os_mac_video_configure(400, 1025, false));
    assert(solar_os_mac_video_configure(512, 342, false));
    free(out); free(expected); free(solar_os_mac_ram);
    solar_os_mac_ram = NULL;
}
int main(int argc, char **argv)
{
    solar_os_mac_app = &app;
    storage_and_bus_tests();
    render_tests();
    char *args[] = { "mac", "--self-test", NULL };
    app.struct_size = offsetof(solar_os_native_foreground_api_v1_t, read_pointer);
    reset(); diagnostic = true;
    assert(!solar_os_mac_main(2, args) && !exit_code);
    assert(!allocations && frames >= 2 && frames < stop_after && suspends == 1 && resumes == 1 && saw_guest_counter);
    unsigned white = 0, black = 0;
    for (unsigned i = 0; i < 480 * 320; ++i) { white += !last_frame[i]; black += last_frame[i] == 1; }
    assert(white > 60000 && black > 60000);
    app.struct_size = sizeof(app);
    for (unsigned fail = 1; fail <= 3; ++fail) {
        reset(); fail_allocation = fail;
        assert(solar_os_mac_main(2, args) == 1 && exit_code == 1 && !allocations);
    }
    reset(); display_error = -4;
    assert(solar_os_mac_main(2, args) == 1 && !allocations);
    reset(); char *bad[] = { "mac", "/nonexistent/mac.rom" };
    assert(solar_os_mac_main(2, bad) == 1 && !allocations);
    reset(); diagnostic = true;
    app.video_acquire = acquire_frame; app.video_exchange = exchange_frame;
    assert(!solar_os_mac_main(2, args) && !exit_code && !allocations);
    assert(frames >= 2 && suspends == 1 && resumes == 1 && saw_guest_counter);
    white = black = 0;
    for (unsigned i = 0; i < 480 * 320; ++i) { white += !last_frame[i]; black += last_frame[i] == 1; }
    assert(white > 60000 && black > 60000);
    app.video_acquire = NULL; app.video_exchange = NULL;
    if (argc == 3 || argc == 5) {
        if (argc == 5) { selected_width = atoi(argv[3]); selected_height = atoi(argv[4]); }
        stop_after = 1200; reset(); diagnostic = false; rom_file = argv[1]; disk_file = argv[2];
        if (!strcmp(disk_file, "-")) disk_file = NULL;
        char *boot[] = { "mac", (char *)rom_file, "--floppy", (char *)disk_file };
        int result = solar_os_mac_main(disk_file ? 4 : 2, boot);
        printf("BOOT result=%d frames=%u reads=%u writes=%u PC=%08x\n", result, frames, disk_reads, disk_writes,
            m68k_get_reg(NULL, M68K_REG_PC));
        assert(!result && !allocations);
        assert(disk_file ? disk_reads && disk_closes == 1 : !disk_reads && !disk_closes);
        assert(solar_os_mac_width == selected_width && solar_os_mac_height == selected_height);
        assert(guest_row_bytes == solar_os_mac_stride);
        assert(guest_height == selected_height && guest_width == selected_width);
        assert(guest_screen == solar_os_mac_screen && guest_ticks > 60);
        unsigned ink = 0;
        for (unsigned i = 0; i < (unsigned)selected_width * 12U; ++i) ink += last_frame[i];
        if (disk_file) assert(ink < (unsigned)selected_width * 12U / 4U); /* Finder menu bar */
        FILE *pgm = fopen("/tmp/solaros-mac-host.pgm", "wb"); assert(pgm);
        fprintf(pgm, "P5\n%u %u\n255\n", frame_width, frame_height);
        for (unsigned i = 0; i < (unsigned)frame_width * frame_height; ++i) fputc(last_frame[i] ? 0 : 255, pgm);
        fclose(pgm);
    }
    free(last_frame);
    puts("Mac module: PASS");
    return 0;
}
