#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <cmath>
#define main solar_os_amiga_main
#include "../../modules/amiga/main/amiga.cpp"
#undef main
#undef fprintf

static unsigned attempts, fail_at, frames, suspends, opens, snapshots, writes;
static unsigned input_stage;
static int result, video_error, read_error, synth_error;
static bool did_suspend, stopping, saw_counter, callback_locked;
static uint64_t time_now;
static uint16_t selected_width = 480, selected_height = 320;
static std::map<void *, size_t> live;
static std::vector<uint8_t> rom_file, adf_file, saved;
static solar_os_native_foreground_audio_render_v1_t callback;
static void *callback_user;

static void *allocate(size_t n)
{
    if (++attempts == fail_at) return nullptr;
    void *p = calloc(1, n); if (p) live[p] = n; return p;
}
static void release(void *p)
{
    if (p == ring) assert(!callback);
    assert(live.erase(p) == 1); free(p);
}
static uint64_t now(void) { return time_now += 10; }
static void sleep_for(uint32_t ms)
{
    time_now += ms * 1000;
    if (chip_ram && (chip_ram[0x400] || chip_ram[0x401])) saw_counter = true;
    if (time_now > 2000000) stopping = true;
}
static bool stop(void) { return stopping; }
static bool should_suspend(void) { return frames >= 3 && !did_suspend; }
static int suspend_guest(void)
{
    assert(!callback); did_suspend = true; ++suspends;
    uint32_t counter = get32(chip_ram + 0x400);
    assert(counter); time_now += 5000000;
    time_now -= 5000000;
    assert(get32(chip_ram + 0x400) == counter); return 0;
}
static int enable(void) { return 0; }
static int open_video(uint16_t w, uint16_t h, const uint16_t *pal, uint16_t fps, uint8_t bg)
{
    assert(w == selected_width && h == selected_height && pal[255] == 0xffff && fps == 25 && bg == 0);
    return video_error;
}
static int present_frame(const uint8_t *data, size_t n)
{
    assert(n == (size_t)selected_width * selected_height); assert(data == frame); ++frames;
    assert(chip_ram[0x400] || chip_ram[0x401]);
    assert(m68k_get_reg(nullptr, M68K_REG_PC) >= 0xfc0010);
    if (frames > 8) stopping = true;
    return 0;
}
static uint8_t host_frames[2][480 * 320];
static unsigned host_writing;
static int acquire_frame(uint8_t **data, size_t *n)
{
    host_writing = 0; *data = host_frames[0];
    *n = (size_t)selected_width * selected_height; return 0;
}
static int exchange_frame(uint8_t **data)
{
    assert(*data == host_frames[host_writing]);
    if (chip_ram[0x400] || chip_ram[0x401]) present_frame(*data, (size_t)selected_width * selected_height);
    *data = host_frames[host_writing ^= 1];
    memset(*data, 0xcd, (size_t)selected_width * selected_height);
    return 0;
}
static int read_key(solar_os_native_foreground_key_v1_t *key, bool *available)
{
    *available = false;
    if (frames >= 2 && input_stage == 0) {
        *key = {}; key->key = SOLAR_OS_KEY_F2; *available = true; ++input_stage;
    }
    return 0;
}
static int resolve(const char *p, char *out, size_t n)
{
    if (!strcmp(p, "kick13.rom")) p = "/rom";
    if (!strcmp(p, "workbench.adf")) p = "/disk";
    if (strlen(p) >= n) return -1;
    memcpy(out, p, strlen(p) + 1); return 0;
}
static int size_file(const char *p, uint32_t *n)
{
    if (!strcmp(p, "/rom")) *n = rom_file.size();
    else if (!strcmp(p, "/disk")) *n = adf_file.size();
    else return -5;
    return 0;
}
static int read_file(const char *p, uint32_t off, void *out, size_t n, size_t *actual)
{
    const auto &f = !strcmp(p, "/rom") ? rom_file : adf_file;
    assert(n <= 32768 && off + n <= f.size());
    if (read_error) { *actual = n - 1; return 0; }
    memcpy(out, f.data() + off, n); *actual = n; return 0;
}
static int replace(const char *p, const void *data, size_t n)
{
    if (n == 96) { assert(!memcmp(data, "AMGP", 4)); return 0; }
    if (n == 32) { assert(!memcmp(data, "AMGA", 4)); return 0; }
    assert(!strcmp(p, "/snapshot") || !strcmp(p, "/rom.ppm"));
    saved.assign((const uint8_t *)data, (const uint8_t *)data + n); ++snapshots; ++writes; return 0;
}
static int start_synth(solar_os_native_foreground_audio_render_v1_t render, void *user, size_t n, uint32_t rate)
{
    assert(!callback && n == 256 && rate == 48000); ++opens;
    if (synth_error) return -1;
    callback = render; callback_user = user; return 0;
}
static void close_synth(void) { callback = nullptr; }
static int audio_open(uint32_t *rate, uint8_t *channels) { *rate = 48000; *channels = 2; return 0; }
static void audio_close(void) {}
static bool lock_synth(void) { assert(!callback_locked); callback_locked = true; return true; }
static void unlock_synth(void)
{
    assert(callback_locked); callback_locked = false;
    if (callback) { int16_t samples[64]; callback(samples, 32, 44100, callback_user); }
}
static void exit_status(int n) { result = n; }
static int video_stats(solar_os_native_foreground_video_stats_v1_t *out)
{
    *out = {}; out->frames = frames; return 0;
}
static int dimensions(uint16_t *w, uint16_t *h) { *w = selected_width; *h = selected_height; return 0; }
static int read_pointer(solar_os_native_foreground_pointer_v1_t *, bool *available) { *available = false; return 0; }
static solar_os_native_foreground_calendar_v1_t calendar_value = {2026,10,17,6,23,59,58};
static bool calendar_unavailable;
static int calendar(solar_os_native_foreground_calendar_v1_t *out) { *out = calendar_value; return calendar_unavailable ? -1 : 0; }
static int output(const char *, size_t) { return 0; }
static solar_os_native_foreground_api_v1_t foreground;
static solar_os_native_memory_api_v1_t allocator = {1, sizeof(allocator), allocate, release};
static const void *service(const char *name, uint32_t abi, uint32_t size)
{
    assert(abi == 1);
    if (!strcmp(name, "foreground")) { assert(size <= sizeof(foreground)); return &foreground; }
    if (!strcmp(name, "memory")) { assert(size <= sizeof(allocator)); return &allocator; }
    assert(false); return nullptr;
}
static solar_os_native_host_api_v1_t api = {1, sizeof(api), "host", "test", output, service};
extern "C" const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &api; }
static int execute(std::initializer_list<const char *> args)
{
    assert(live.empty()); attempts = frames = suspends = opens = snapshots = input_stage = 0;
    time_now = 0; stopping = did_suspend = saw_counter = false; saved.clear();
    std::vector<char *> argv; for (auto arg : args) argv.push_back(const_cast<char *>(arg));
    int status = solar_os_amiga_main(argv.size(), argv.data());
    assert(live.empty() && !callback && status == result); return status;
}
int main(void)
{
    // Rendering adapts to the 20 ms guest budget and remains bounded even
    // when CPU emulation alone is too expensive. Light workloads recover.
    video_pacing pacing;
    for (unsigned i = 0; i < 100; ++i) {
        bool draw = pacing.begin(0); pacing.end(draw ? 50000 : 10000, draw, 0);
    }
    assert(pacing.interval == 4);
    for (unsigned i = 0; i < 100; ++i) {
        bool draw = pacing.begin(0); pacing.end(draw ? 50000 : 25000, draw, 0);
    }
    assert(pacing.interval == 8);
    for (unsigned i = 0; i < 200; ++i) {
        bool draw = pacing.begin(0); pacing.end(draw ? 6000 : 5000, draw, 0);
    }
    assert(pacing.interval == 2);
    // When emulation itself exceeds the frame budget, skip rendering while
    // keeping visible updates within 100 ms, rather than eight slow frames.
    video_pacing overloaded;
    uint64_t wall = 1, last_draw = 0;
    for (unsigned i = 0; i < 200; ++i) {
        bool draw = overloaded.begin(wall);
        uint32_t elapsed = draw ? 50000 : 25000;
        wall += elapsed;
        overloaded.end(elapsed, draw, wall);
        if (draw) {
            if (last_draw) assert(wall - last_draw <= 100000);
            last_draw = wall;
        }
    }
    foreground.abi_version = 1; foreground.struct_size = sizeof(foreground);
    foreground.time_us = now; foreground.sleep_ms = sleep_for; foreground.present = present_frame;
    foreground.read_key = read_key; foreground.file_size = size_file; foreground.read_file = read_file;
    foreground.replace_file = replace; foreground.set_exit_code = exit_status;
    foreground.stop_requested = stop; foreground.video_open_ex = open_video;
    foreground.resolve_path = resolve; foreground.enable_suspend = enable;
    foreground.suspend_requested = should_suspend; foreground.suspend = suspend_guest;
    foreground.synth_open = start_synth; foreground.synth_close = close_synth;
    foreground.audio_open = audio_open; foreground.audio_close = audio_close;
    foreground.synth_lock = lock_synth; foreground.synth_unlock = unlock_synth;
    foreground.video_stats = video_stats;
    foreground.video_dimensions = dimensions;
    foreground.read_pointer = read_pointer; foreground.calendar_read = calendar;
    assert(execute({"amiga", "--self-test", "/snapshot"}) == 0);
    assert(frames >= 9 && suspends == 1 && saw_counter && snapshots == 1 && opens == 2);
    assert(saved.size() == 15 + 480 * 320 * 3 && !memcmp(saved.data(), "P6\n480 320\n255\n", 15));
    for (auto size : {std::pair<uint16_t,uint16_t>{400,300}, {384,168}, {320,240}, {320,480}}) {
        selected_width = size.first; selected_height = size.second;
        assert(execute({"amiga", "--self-test"}) == 0);
        assert(frames >= 9 && saw_counter);
    }
    selected_width = 480; selected_height = 320;
    foreground.video_acquire = acquire_frame; foreground.video_exchange = exchange_frame;
    assert(execute({"amiga", "--self-test", "/snapshot"}) == 0);
    assert(frames >= 9 && suspends == 1 && saw_counter && snapshots == 1 && opens == 2);
    assert(saved.size() == 15 + 480 * 320 * 3);
    foreground.video_acquire = nullptr; foreground.video_exchange = nullptr;
    for (fail_at = 1; fail_at <= 5; ++fail_at) assert(execute({"amiga", "--self-test"}) == 1);
    fail_at = 6; assert(execute({"amiga", "--self-test"}) == 0); // optional audio buffer
    fail_at = 0; synth_error = -1; assert(execute({"amiga", "--self-test"}) == 0); synth_error = 0;
    video_error = -1; assert(execute({"amiga", "--self-test"}) == 1); video_error = 0;
    allocator.alloc_internal = allocate;
    assert(execute({"amiga", "--self-test"}) == 0);
    fail_at = 7; assert(execute({"amiga", "--self-test"}) == 0); // optional fast CPU state
    fail_at = 8; assert(execute({"amiga", "--self-test"}) == 0); // optional bitmap row cache
    fail_at = 0; allocator.alloc_internal = nullptr;
    assert(execute({"amiga"}) == 1);
    rom_file.resize(ROM_SIZE - 1); assert(execute({"amiga", "/rom"}) == 1);
    rom_file.resize(ROM_SIZE); assert(execute({"amiga", "/rom"}) == 1); // bad checksum
    // Obtain the original diagnostic bytes; add a valid Kickstart-style checksum.
    memory = &allocator; kick_rom = rom_file.data(); diagnostic_rom();
    put32(kick_rom, 0x11114ef9); // Kickstart's ROM ID + JMP, deliberately not a stack address
    uint32_t sum = 0;
    for (unsigned i = 0; i < ROM_SIZE - 4; i += 4) { uint32_t p = sum; sum += get32(kick_rom + i); if (sum < p) ++sum; }
    put32(kick_rom + ROM_SIZE - 4, ~sum); assert(valid_rom()); kick_rom = nullptr;
    adf_file.resize(901119); assert(execute({"amiga", "/rom", "/disk"}) == 1);
    adf_file.resize(901120); read_error = -1; assert(execute({"amiga", "/rom"}) == 1); read_error = 0;
    assert(execute({"amiga", "/rom", "/disk"}) == 0);
    assert(execute({"amiga", "kick13.rom", "workbench.adf"}) == 0);
    assert(!strcmp(solar_os_native_app_manifest.file_extensions, ".rom"));
    assert(keyboard_mode && !mouse_mode);
    // Direct chipset checks: hires/lores conversion, input release/reset, Paula output.
    host = &api; app = &foreground; memory = &allocator;
    width = 480; height = 320; canvas_started = true; fit_source_width = 0;
    chip_ram = (uint8_t *)allocate(CHIP_SIZE); pixels = (uint8_t *)allocate(width * height);
    // A recognized beam wait stalls only for the current raster line, then
    // continues when the requested line arrives. Modified code and other
    // address-register mappings must never take the idle shortcut.
    const uint16_t wait_code[] = {0x202e,4,0x0280,1,0xff00,0x0c80,0,0xf800,0x66ee,0x5241,0x60fc};
    for (unsigned i = 0; i < sizeof(wait_code) / 2; ++i) put16(chip_ram + 0x1000 + 2 * i, wait_code[i]);
    ovl = 0; put32(chip_ram, 0x40000); put32(chip_ram + 4, 0x1000);
    custom_reset(); m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset();
    m68k_set_reg(M68K_REG_A6, 0xdff000); m68k_set_reg(M68K_REG_D1, 0);
    vpos = 100; m68k_execute(454);
    assert(m68k_get_reg(nullptr, M68K_REG_PC) == 0x1000 && m68k_get_reg(nullptr, M68K_REG_D1) == 0);
    vpos = 248; m68k_execute(454);
    assert(m68k_get_reg(nullptr, M68K_REG_D1) > 0);
    uint32_t address_regs[8] = {}; address_regs[6] = 0xdff000;
    assert(solar_os_amiga_beam_wait(0x1000, address_regs));
    address_regs[6] = 0xbfe000; assert(!solar_os_amiga_beam_wait(0x1000, address_regs));
    address_regs[6] = 0xdff000; put16(chip_ram + 0x1004, 0x0680);
    assert(!solar_os_amiga_beam_wait(0x1000, address_regs));
    assert(!solar_os_amiga_beam_wait(CHIP_SIZE - 16, address_regs));
    // These MOVEQ opcodes collide in the decoded dispatch cache.
    put16(chip_ram + 0x1000, 0x7001); put16(chip_ram + 0x1002, 0x7203);
    put16(chip_ram + 0x1004, 0xd081);
    m68k_set_reg(M68K_REG_PC, 0x1000); m68k_execute(16);
    assert(m68k_get_reg(nullptr, M68K_REG_D0) == 4 && m68k_get_reg(nullptr, M68K_REG_D1) == 3);
    put16(chip_ram + 0x1000, 0x7002); m68k_set_reg(M68K_REG_PC, 0x1000); m68k_execute(16);
    assert(m68k_get_reg(nullptr, M68K_REG_D0) == 5);
    memset(pixels, 0, width * height);
    uint16_t row[640]; for (auto &color : row) color = 0x0f0;
    render_line(44, row, 640); render_line(299, row, 640);
    assert(fit_width == 426 && fit_height == 320 && fit_source_height == 256);
    assert(pixels[27] == 0x1c && pixels[319 * width + 27] == 0x1c);
    assert(!pixels[0] && !pixels[319 * width + 479]);
    // Pulling down a screen changes DIW and bitmap occupancy, never the scale
    // or monitor origin. The same raster line must land on the same host row.
    custom_write(0x08e, 0x8c81); custom_write(0x090, 0xf4c1);
    for (auto &color : row) color = 0xf00;
    render_line(172, row, 640);
    assert(fit_start == 44 && fit_source_height == 256);
    assert(pixels[160 * width + 27] == 0xe0);
    render_line(172, row, 320);
    assert(pixels[160 * width + 27] == 0xe0 && fit_start == 44 && fit_source_height == 256);
    render_line(43, row, 640); render_line(300, row, 640);
    assert(!pixels[0] && !pixels[319 * width + 479]);
    // Recycled host buffers may contain arbitrary pixels. A partial redraw
    // must retain untouched rows and clear margins without copying over the
    // newly drawn rows, including vertical scaling that duplicates a row.
    {
        std::vector<uint8_t> previous_image(width * height, 0);
        for (unsigned y = 0; y < height; ++y)
            memset(previous_image.data() + y * width + fit_left, 0x55, fit_width);
        frame = previous_image.data();
        memset(pixels, 0xcd, width * height); canvas_started = false;
        render_line(172, row, 640); finish_canvas();
        for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
            uint8_t expected = x >= (unsigned)fit_left && x < fit_left + fit_width ?
                (y == 160 || y == 161 ? 0xe0 : 0x55) : 0;
            assert(pixels[y * width + x] == expected);
        }
        frame = nullptr;
    }
    // Exercise the actual Copper and bitplane renderer, then a bounded MFM read
    // from an original synthetic ADF, including the highest valid track.
    memset(chip_ram, 0, CHIP_SIZE); custom_reset();
    put16(chip_ram + 0x1000, 0x0180); put16(chip_ram + 0x1002, 0x000f);
    put16(chip_ram + 0x1004, 0xffff); put16(chip_ram + 0x1006, 0xfffe);
    custom_write(0x080, 0); custom_write(0x082, 0x1000); custom_write(0x096, 0x8380);
    vpos = 44; copper_vblank(); copper_run_line(); assert(custom_get(0x180) == 0xf);
    chip_ram[0x2000] = 0x80;
    custom_write(0x092, 0x38); custom_write(0x094, 0xd0);
    custom_write(0x182, 0x0f0); custom_write(0x08e, 0x2c81); custom_write(0x090, 0xf4c1);
    custom_write(0x100, 0x1200); custom_write(0x0e0, 0); custom_write(0x0e2, 0x2000);
    denise_line_cb = render_line; denise_render_line();
    assert(pixels[27] == 0x1c && pixels[28] == 0x1c && pixels[29] == 3);
    custom_write(0x100, 0x9200); custom_write(0x0e2, 0x2000);
    custom_write(0x092, 0x3c); custom_write(0x094, 0xd4);
    vpos = 45; denise_render_line(); assert(pixels[2 * width + 27] == 0x1c && pixels[2 * width + 28] == 3);
    // Hires DMA completes an eight-clock fetch block, even when DDFSTOP
    // lands halfway through it. Workbench uses 3c..d0 for an 80-byte row.
    custom_write(0x094, 0xd0); custom_write(0x0e2, 0x2000);
    denise_render_line(); assert(bpl_pt[0] == 0x2050);
    denise_draw = false; custom_write(0x0e2, 0x2000);
    denise_render_line(); assert(bpl_pt[0] == 0x2050);
    denise_draw = true; custom_write(0x094, 0xd4);
    // Skipping output still performs the same bitplane DMA/modulo update.
    denise_draw = false; custom_write(0x0e2, 0x2000); vpos = 44;
    denise_render_line(); assert(bpl_pt[0] == 0x2050);
    denise_draw = true;
    // Unchanged rows skip conversion while DMA still advances. Palette and
    // bitmap edits must invalidate the cached output independently.
    video_prepare();
    unsigned callbacks = 0;
    static unsigned *callback_count;
    callback_count = &callbacks;
    denise_line_cb = [](int, const uint16_t *, int) { ++*callback_count; };
    auto cached_line = [&]() { custom_write(0x0e2, 0x2000); denise_render_line(); assert(bpl_pt[0] == 0x2050); };
    cached_line(); cached_line(); assert(callbacks == 1);
    chip_ram[0x2000] ^= 0x80; cached_line(); assert(callbacks == 2);
    custom_write(0x182, 0xf00); cached_line(); assert(callbacks == 3);
    video_cache_invalidate(); cached_line(); assert(callbacks == 4);
    video_release(); denise_line_cb = render_line;
    // Kefrens' opening text uses a 68-byte hires fetch, placed 48 pixels
    // into the standard raster. A fixed 80-byte stride scrambles its rows.
    std::vector<uint16_t> raster;
    static std::vector<uint16_t> *captured;
    captured = &raster;
    denise_line_cb = [](int, const uint16_t *row, int count) { captured->assign(row, row + count); };
    memset(chip_ram + 0x2000, 0, 256);
    chip_ram[0x2000] = 0x80; chip_ram[0x2044] = 0x40;
    custom_write(0x180, 0); custom_write(0x182, 0xfff);
    custom_write(0x08e, 0x7c71); custom_write(0x090, 0xbcd1);
    custom_write(0x092, 0x48); custom_write(0x094, 0xc8);
    custom_write(0x0e2, 0x2000);
    vpos = 124; denise_render_line();
    assert(bpl_pt[0] == 0x2044 && raster.size() == 640);
    assert(raster[47] == 0 && raster[48] == 0xfff && raster[49] == 0);
    vpos = 125; denise_render_line();
    assert(bpl_pt[0] == 0x2088 && raster[48] == 0 && raster[49] == 0xfff);
    assert(raster[592] == 0 && raster[639] == 0);
    // Geometry changes invalidate the row cache even with identical bytes.
    video_prepare(); vpos = 124;
    custom_write(0x0e2, 0x2000); denise_render_line();
    custom_write(0x092, 0x4c); custom_write(0x094, 0xcc);
    custom_write(0x0e2, 0x2000); denise_render_line();
    assert(raster[48] == 0 && raster[64] == 0xfff);
    custom_write(0x08e, 0x7ca9);
    custom_write(0x0e2, 0x2000); denise_render_line();
    assert(raster[64] == 0); // Horizontal display window clips the bitplane.
    video_release();
    // Skipped frames use the same fetch length and signed modulo.
    denise_draw = false; custom_write(0x108, 0xfffe);
    custom_write(0x0e2, 0x2000); denise_render_line();
    assert(bpl_pt[0] == 0x2042);
    // Maximum OCS DMA width and chip-RAM wrap stay within row storage.
    denise_draw = true; custom_write(0x108, 0);
    custom_write(0x092, 0x18); custom_write(0x094, 0xd8);
    custom_write(0x0e0, 7); custom_write(0x0e2, 0xfff0);
    denise_render_line(); assert(bpl_pt[0] == 0x80052);
    custom_write(0x100, 0x1200); custom_write(0x0e0, 0); custom_write(0x0e2, 0x2000);
    denise_render_line(); assert(bpl_pt[0] == 0x2032);
    custom_write(0x092, 0xd0); custom_write(0x094, 0x38);
    custom_write(0x108, 8); custom_write(0x0e2, 0x2000);
    denise_render_line(); assert(bpl_pt[0] == 0x2000);
    denise_draw = false; denise_render_line(); assert(bpl_pt[0] == 0x2000);
    denise_draw = true;
    // Compare byte-expanded shifted rows against individual planar pixels,
    // including both scroll fields, clipping, EHB, and partial fetch windows.
    for (bool hires : {false, true}) for (unsigned planes = 1; planes <= (hires ? 4U : 6U); ++planes) {
        custom_write(0x100, (hires ? 0x8000 : 0) | (planes << 12) | 0x200);
        custom_write(0x096, 0x20); // Keep sprites out of the planar comparison.
        uint16_t colors[32];
        for (unsigned c = 0; c < 32; ++c) {
            colors[c] = (c * 199 + 47) & 0xfff;
            custom_write(0x180 + 2 * c, colors[c]);
        }
        for (unsigned p = 0; p < planes; ++p)
            for (unsigned b = 0; b < 100; ++b)
                chip_ram[0x10000 + p * 0x1000 + b] = (b * 73 + p * 117 + (b >> 1)) & 255;
        for (auto fetch : {std::pair<unsigned,unsigned>{0x18,0xd8}, {0x30,0xd0},
                           {0x38,0xd0}, {0x3c,0xd0}, {0x48,0xc8}})
            for (unsigned scroll : {0U, 1U, 7U, 15U}) for (bool clipped : {false, true}) {
                custom_write(0x08e, clipped ? 0x2c91 : 0x2c81);
                custom_write(0x090, clipped ? 0x2ca1 : 0x2cc1);
                custom_write(0x092, fetch.first); custom_write(0x094, fetch.second);
                custom_write(0x102, scroll | ((15 - scroll) << 4));
                custom_write(0x108, 0); custom_write(0x10a, 0);
                for (unsigned p = 0; p < planes; ++p) {
                    custom_write(0x0e0 + 4 * p, 1);
                    custom_write(0x0e2 + 4 * p, p * 0x1000);
                }
                vpos = 100; denise_render_line();
                unsigned fetched = bpl_pt[0] - 0x10000;
                int scale = hires ? 2 : 1, count = hires ? 640 : 320;
                int fetch_left = ((fetch.first & (hires ? 0xfc : 0xf8)) * 2 + (hires ? 9 : 17) - 0x81) * scale;
                assert(raster.size() == (unsigned)count);
                for (int x = 0; x < count; ++x) {
                    unsigned index = 0;
                    for (unsigned p = 0; p < planes; ++p) {
                        int sx = x - fetch_left - (int)((p & 1) ? 15 - scroll : scroll) * scale;
                        if (sx >= 0 && sx < (int)fetched * 8)
                            index |= ((chip_ram[0x10000 + p * 0x1000 + sx / 8] >> (7 - sx % 8)) & 1) << p;
                    }
                    uint16_t expected = index < 32 ? colors[index] : (colors[index & 31] & 0xeee) >> 1;
                    if (clipped && (x < 16 * scale || x >= 288 * scale)) expected = colors[0];
                    assert(raster[x] == expected);
                }
            }
    }
    denise_line_cb = render_line;
    // A late wait at line 255 must cross the vertical counter wrap before
    // testing the next wait. PAL screen data below row 255 must survive.
    put16(chip_ram + 0x1000, 0xffdf); put16(chip_ram + 0x1002, 0xfffe);
    put16(chip_ram + 0x1004, 0x0a01); put16(chip_ram + 0x1006, 0xfffe);
    put16(chip_ram + 0x1008, 0x0180); put16(chip_ram + 0x100a, 0x0abc);
    put16(chip_ram + 0x100c, 0xffff); put16(chip_ram + 0x100e, 0xfffe);
    copper_vblank(); custom_write(0x180, 0x0012);
    for (vpos = 254; vpos < 266; ++vpos) { copper_run_line(); assert(custom_get(0x180) == 0x0012); }
    copper_run_line(); assert(custom_get(0x180) == 0x0abc);
    solar_os_amiga_drive_reset();
    uint8_t *adf = drive_alloc_adf(); assert(adf); memset(adf, 0, 901120); drive_mount_ready();
    drive_prb_write(0xff); drive_prb_write(0x77); assert(drive_motor_on());
    assert(!(drive_pra_bits() & 8)); // write-protected
    for (unsigned i = 0; i < 90; ++i) { drive_prb_write(0x71); drive_prb_write(0x70); }
    assert(drive_track() == 79 && drive_side() == 1);
    custom_write(0x020, 1); custom_write(0x022, 0);
    disk_dsklen_write(0x9fff); disk_dsklen_write(0x9fff);
    assert(disk_irq_pending && chip_ram[0x10000] == 0xaa);
    bool sync = false;
    for (unsigned i = 0x10000; i < 0x13ff0; ++i)
        sync |= chip_ram[i] == 0x44 && chip_ram[i + 1] == 0x89 && chip_ram[i + 2] == 0x44 && chip_ram[i + 3] == 0x89;
    assert(sync);
    while (allocation_count) release(allocations[--allocation_count]);
    solar_os_amiga_drive_reset();
    reset_input(); mouse_mode = false; keyboard_mode = false;
    solar_os_native_foreground_key_v1_t key = {}; key.key = SOLAR_OS_KEY_RIGHT; key.usage = 0x4f;
    input(key); input_frame_tick(); assert(input_joy1dat() == 3);
    key.action = SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE; input(key); input_frame_tick(); assert(!input_joy1dat());
    key.action = 0; input(key); reset_input(); input_frame_tick(); assert(!input_joy1dat());
    key.usage = 0; input(key); input_frame_tick(); assert(input_joy1dat() == 3);
    time_now += 200000; input_frame_tick(); assert(!input_joy1dat());
    // Mouse motion and buttons coexist with keyboard joystick state.
    solar_os_native_foreground_pointer_v1_t pointer = {};
    pointer.mode = SOLAR_OS_NATIVE_POINTER_RELATIVE; pointer.delta_x = 17; pointer.delta_y = -9;
    pointer.buttons = 3; pointer.source = 1;
    uint16_t previous_mouse = input_joy0dat(); pointer_input(pointer); input_frame_tick();
    assert((uint8_t)input_joy0dat() == (uint8_t)(previous_mouse + 17));
    assert((uint8_t)(input_joy0dat() >> 8) == (uint8_t)((previous_mouse >> 8) - 9));
    assert(input_lmb() && input_rmb());
    pointer = {}; pointer.reset = true; pointer_input(pointer); input_frame_tick();
    assert(!input_lmb() && !input_rmb());
    pointer = {}; pointer.source = 2; pointer.buttons = 1;
    pointer.action = SOLAR_OS_NATIVE_POINTER_PRESS; pointer.x = 400; pointer.y = 200;
    previous_mouse = input_joy0dat(); pointer_input(pointer); input_frame_tick();
    assert(input_joy0dat() == previous_mouse && !input_lmb()); // touch moves freely
    pointer.action = SOLAR_OS_NATIVE_POINTER_MOVE; pointer.x += 13; pointer.y += 7;
    pointer_input(pointer); input_frame_tick();
    assert((uint8_t)input_joy0dat() == (uint8_t)(previous_mouse + 13));
    assert(!input_lmb());
    pointer.action = SOLAR_OS_NATIVE_POINTER_RELEASE; pointer.buttons = 0;
    pointer_input(pointer); input_frame_tick(); assert(!input_lmb());
    pointer.action = SOLAR_OS_NATIVE_POINTER_PRESS; pointer.buttons = 1; pointer.x = 12;
    previous_mouse = input_joy0dat(); pointer_input(pointer); input_frame_tick(); assert(input_joy0dat() == previous_mouse);
    // A short stationary tap produces a complete guest click. Taking a
    // snapshot cannot consume its minimum two-frame down interval.
    pointer.action = SOLAR_OS_NATIVE_POINTER_RELEASE;
    pointer_input(pointer); input_frame_tick(); assert(input_lmb());
    input_frame_tick(false); assert(input_lmb());
    input_frame_tick(); assert(input_lmb());
    input_frame_tick(); assert(!input_lmb());
    // A stationary hold starts a drag; lifting always ends it, even if a
    // source leaves its contact bit set in the release event.
    pointer.action = SOLAR_OS_NATIVE_POINTER_PRESS;
    pointer_input(pointer); time_now += 360000; input_frame_tick(); assert(input_lmb());
    pointer.action = SOLAR_OS_NATIVE_POINTER_MOVE; pointer.y += 30;
    pointer_input(pointer); input_frame_tick(); assert(input_lmb());
    pointer.action = SOLAR_OS_NATIVE_POINTER_RELEASE;
    pointer_input(pointer); input_frame_tick(); assert(!input_lmb() && !touch_clicks);
    reset_input(); input_frame_tick(); assert(!input_lmb() && !input_rmb());
    // Function keys select each mode directly and release the previous mode.
    key = {}; key.key = SOLAR_OS_KEY_F7; input(key); assert(mouse_mode && !keyboard_mode);
    key = {}; key.usage = 0x2c; input(key); input_frame_tick(); assert(input_lmb());
    key = {}; key.usage = 0x41; input(key); assert(!mouse_mode && !keyboard_mode && !input_lmb());
    key = {}; key.usage = 0x2c; input(key); input_frame_tick(); assert(input_joy_fire());
    key = {}; key.key = SOLAR_OS_KEY_F6; input(key); assert(keyboard_mode && !mouse_mode && !input_joy_fire());
    // Actual mapped clock nibble lanes, HOLD, midnight rollover, writable
    // controls and session-local date setting used by SetClock.
    solar_os_amiga_rtc_reset();
    assert(m68k_read_memory_8(0xdc0001) == 8 && m68k_read_memory_8(0xdc0003) == 8);
    assert(m68k_read_memory_8(0xdc0000) == 0 && m68k_read_memory_16(0xdc0002) == 8);
    assert(m68k_read_memory_8(0xdc002b) == 6 && m68k_read_memory_8(0xdc002f) == 2);
    m68k_write_memory_8(0xdc0037, 1); guest_clock_us += 3000000;
    assert(m68k_read_memory_8(0xdc0003) == 8); // HOLD
    m68k_write_memory_8(0xdc0037, 0); guest_clock_us += 3000000;
    assert(m68k_read_memory_8(0xdc0003) == 1 && m68k_read_memory_8(0xdc0007) == 0);
    assert(m68k_read_memory_8(0xdc001b) == 8 && m68k_read_memory_8(0xdc0033) == 0);
    m68k_write_memory_8(0xdc003f, 6); guest_clock_us += 3000000;
    assert(m68k_read_memory_8(0xdc0003) == 1); // STOP
    m68k_write_memory_8(0xdc0037, 1); m68k_write_memory_8(0xdc0003, 5);
    assert(m68k_read_memory_8(0xdc0003) == 5);
    solar_os_amiga_rtc_reset(); assert(m68k_read_memory_8(0xdc0003) == 8); // fresh host seed
    // A 75-tick detection delay must advance the seconds digit even when
    // a slow host takes ten real seconds to execute it.
    time_now += 10000000; guest_clock_us += 1500000;
    assert(m68k_read_memory_8(0xdc0003) != 8);
    solar_os_amiga_rtc_reset();
    solar_os_amiga_rtc_write(15, 0); // 12-hour mode retains 23:59:58 as 11 PM
    assert(solar_os_amiga_rtc_read(4) == 1 && solar_os_amiga_rtc_read(5) == 5);
    solar_os_amiga_rtc_write(15, 4);
    assert(solar_os_amiga_rtc_read(4) == 3 && solar_os_amiga_rtc_read(5) == 2);
    calendar_value = {2024,2,28,3,23,59,59}; solar_os_amiga_rtc_reset(); guest_clock_us += 2000000;
    assert(solar_os_amiga_rtc_read(6) == 9 && solar_os_amiga_rtc_read(7) == 2 && solar_os_amiga_rtc_read(8) == 2);
    calendar_value = {2026,12,31,4,23,59,59}; solar_os_amiga_rtc_reset(); guest_clock_us += 2000000;
    assert(solar_os_amiga_rtc_read(6) == 1 && solar_os_amiga_rtc_read(8) == 1 && solar_os_amiga_rtc_read(10) == 7);
    calendar_unavailable = true; solar_os_amiga_rtc_reset();
    assert(solar_os_amiga_rtc_read(10) == 8 && solar_os_amiga_rtc_read(11) == 8 && solar_os_amiga_rtc_read(6) == 1);
    calendar_unavailable = false;
    // Real CIA-A serial transport: power-up stream, guest handshake, HID
    // key releases, shifted characters, control chords and reset releases.
    cia_reset(); cia_write(&cia_a, 0x0d, 0x88);
    auto raw_key = []() {
        cia_tick(400);
        assert(cia_read(&cia_a, 0x0d) == 0x88);
        uint8_t serial = (uint8_t)~cia_read(&cia_a, 0x0c);
        uint8_t raw = (serial >> 1) | (serial << 7);
        cia_write(&cia_a, 0x0e, 0x40); cia_write(&cia_a, 0x0e, 0);
        return raw;
    };
    cia_tick(400); assert(cia_read(&cia_a, 0x0d) == 0x88);
    assert(cia_read(&cia_a, 0x0c) == 4); // rotated/inverted FD marker
    cia_tick(400); assert(cia_read(&cia_a, 0x0d) == 0); // waits for handshake
    cia_write(&cia_a, 0x0e, 0x40); cia_write(&cia_a, 0x0e, 0);
    assert(raw_key() == 0xfe);
    for (uint8_t keycode : {SOLAR_OS_KEY_F3, SOLAR_OS_KEY_F4, SOLAR_OS_KEY_F5}) {
        solar_os_native_foreground_key_v1_t function = {}; function.key = keycode;
        input(function);
        assert(raw_key() == 0x50 + keycode - SOLAR_OS_KEY_F1);
        assert(raw_key() == 0xd0 + keycode - SOLAR_OS_KEY_F1);
        assert(fit_start == 44 && fit_source_height == 256);
    }
    solar_os_native_foreground_key_v1_t typed = {}; typed.usage = 4;
    solar_os_amiga_keyboard_input(typed); assert(raw_key() == 0x20);
    typed.action = SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
    solar_os_amiga_keyboard_input(typed); assert(raw_key() == 0xa0);
    typed = {}; typed.key = 'A'; solar_os_amiga_keyboard_input(typed);
    for (uint8_t expected : {0x60,0x20,0xa0,0xe0}) assert(raw_key() == expected);
    typed.key = 4; solar_os_amiga_keyboard_input(typed);
    for (uint8_t expected : {0x63,0x22,0xa2,0xe3}) assert(raw_key() == expected);
    typed = {}; typed.usage = 0x4f; solar_os_amiga_keyboard_input(typed);
    assert(raw_key() == 0x4e); solar_os_amiga_keyboard_release(); assert(raw_key() == 0xce);
    typed = {}; typed.usage = 0x39; solar_os_amiga_keyboard_input(typed); assert(raw_key() == 0x62);
    typed.action = SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE; solar_os_amiga_keyboard_input(typed);
    cia_tick(400); assert(cia_read(&cia_a, 0x0d) == 0);
    typed.action = SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS; solar_os_amiga_keyboard_input(typed); assert(raw_key() == 0xe2);
    ring = (int16_t *)allocate(ring_size * 4); audio = true; ring_read = ring_write = 0;
    // Signed high/low bytes, stereo routing, and DMA interrupts. Changing RAM
    // after the high byte is fetched must not corrupt the latched low byte.
    solar_os_amiga_audio_rate(16000); reset_audio_queue(); paula_reset();
    chip_ram[0] = 0x7f; chip_ram[1] = 0x80;
    paula_write(0xa4, 1); paula_write(0xa6, 1000); paula_write(0xa8, 64); paula_set_dma(0x201);
    assert(intreq & 0x80); custom_write(0x09c, 0x80);
    paula_step(1000); assert(intreq & 0x80);
    chip_ram[1] = 0x7f; paula_step(1300);
    assert(ring_write == 10);
    bool positive = false, negative = false;
    for (unsigned i = 0; i < ring_write; ++i) {
        positive |= ring[i * 2] > 0; negative |= ring[i * 2] < 0;
        assert(!ring[i * 2 + 1]);
    }
    assert(positive && negative);
    // Zero LEN means 65536 words. Direct AUDxDAT output also advances and
    // signals completion with DMA disabled.
    reset_audio_queue(); paula_reset();
    paula_write(0xa6, 10); paula_set_dma(0x201); custom_write(0x09c, 0x80);
    paula_step(1000); assert(!(intreq & 0x80));
    reset_audio_queue(); paula_reset(); custom_write(0x09c, 0x80);
    paula_write(0xa6, 1000); paula_write(0xa8, 64); paula_write(0xaa, 0x7f80);
    paula_step(2500); assert(intreq & 0x80);
    positive = negative = false;
    for (unsigned i = 0; i < ring_write; ++i) { positive |= ring[i * 2] > 0; negative |= ring[i * 2] < 0; }
    assert(positive && negative);
    // Account for silent time at exactly the negotiated sample rate, including
    // rates whose clocks-per-sample ratio is fractional.
    reset_audio_queue(); paula_reset(); solar_os_amiga_audio_rate(11025);
    for (unsigned i = 0; i < 1000; ++i) paula_step(227);
    assert(ring_write == (uint64_t)227000 * 11025 / 3546895);
    for (unsigned i = 0; i < ring_write * 2; ++i) assert(!ring[i]);
    // A steady tone must remain continuous when the guest runs at 50, 40,
    // 25, or 20 fps, with bursty frame production and 16 ms host callbacks.
    // Filling missing fragments with zeros creates long gaps in these cases.
    for (unsigned frame_ms : {20, 22, 25, 40, 50}) {
        reset_audio_queue(); unsigned source = 0, zero_run = 0, longest_gap = 0, crossings = 0, count = 0;
        int previous = 0, largest_jump = 0; double energy = 0, last_crossing = -1;
        int16_t samples[512];
        for (unsigned ms = 0; ms < 4000; ++ms) {
            if (!(ms % frame_ms)) for (unsigned n = 0; n < 320; ++n) {
                int16_t v = 12000 * std::sin(source++ * 6.283185307179586 * 440 / 16000);
                solar_os_amiga_audio_sample(v, -v);
            }
            if (ms % 16) continue;
            render_audio(samples, 256, 16000, nullptr);
            if (ms < 1000) continue;
            for (unsigned n = 0; n < 256; ++n) {
                int v = samples[n * 2]; assert(abs(v + samples[n * 2 + 1]) <= 1);
                zero_run = v ? 0 : zero_run + 1;
                if (zero_run > longest_gap) longest_gap = zero_run;
                if (count && abs(v - previous) > largest_jump) largest_jump = abs(v - previous);
                if (previous <= 0 && v > 0) {
                    ++crossings;
                    // Measure individual output periods, not just average
                    // frequency: a tape-like pitch wobble can average out.
                    double position = (double)count - 1 + (double)-previous / (v - previous);
                    if (ms >= 2000 && last_crossing >= 0) {
                        double hz = 16000 / (position - last_crossing);
                        double expected = 440.0;
                        assert(hz > expected * .98 && hz < expected * 1.02);
                    }
                    if (ms >= 2000) last_crossing = position;
                }
                previous = v; energy += (double)v * v; ++count;
            }
        }
        double frequency = (double)crossings * 16000 / count;
        double expected = 440.0;
        assert(frequency > expected * .96 && frequency < expected * 1.04);
        assert(std::sqrt(energy / count) > 7500 && std::sqrt(energy / count) < 9000);
        assert(longest_gap < 8 && largest_jump < 4000);
        assert(!audio_dropped && !audio_underruns);
    }
    // Scene changes vary the guest speed. Follow production quickly enough
    // to avoid repeated empty/refill cycles while preserving the note pitch.
    {
        reset_audio_queue();
        unsigned source = 0, next_frame = 0, zero_run = 0, longest_gap = 0;
        unsigned count = 0, crossings = 0;
        int previous = 0, largest_jump = 0;
        for (unsigned ms = 0; ms < 60000; ++ms) {
            unsigned frame_ms = ms % 12000 < 4000 ? 20 : ms % 12000 < 8000 ? 50 : 22;
            if (ms >= next_frame) {
                next_frame = ms + frame_ms;
                for (unsigned n = 0; n < 320; ++n) {
                    int16_t v = 12000 * std::sin(source++ * 6.283185307179586 * 440 / 16000);
                    solar_os_amiga_audio_sample(v, -v);
                }
            }
            if (ms % 16) continue;
            int16_t samples[512]; render_audio(samples, 256, 16000, nullptr);
            if (ms < 1000) continue;
            for (unsigned n = 0; n < 256; ++n) {
                int v = samples[n * 2]; assert(abs(v + samples[n * 2 + 1]) <= 1);
                zero_run = v ? 0 : zero_run + 1;
                if (zero_run > longest_gap) longest_gap = zero_run;
                if (count && abs(v - previous) > largest_jump) largest_jump = abs(v - previous);
                if (count && previous <= 0 && v > 0) ++crossings;
                previous = v; ++count;
            }
        }
        double hz = (double)crossings * 16000 / count;
        assert(hz > 430 && hz < 450 && longest_gap < 8 && largest_jump < 4000);
        assert(!audio_dropped && !audio_underruns);
    }
    // A producer stall must rebuffer without learning the silent gap as a
    // slower clock, which otherwise causes prolonged detuning and overflow.
    reset_audio_queue();
    unsigned source = 0, output_count = 0;
    double last_crossing = -1;
    int previous = 0;
    for (unsigned ms = 0; ms < 8000; ++ms) {
        if ((ms < 3000 || ms >= 4000) && !(ms % 22))
            for (unsigned n = 0; n < 320; ++n) {
                int16_t v = 12000 * std::sin(source++ * 6.283185307179586 * 440 / 16000);
                solar_os_amiga_audio_sample(v, -v);
            }
        if (ms % 16) continue;
        int16_t samples[512]; render_audio(samples, 256, 16000, nullptr);
        if (ms < 5000) continue;
        for (unsigned n = 0; n < 256; ++n) {
            int v = samples[n * 2];
            if (output_count && previous <= 0 && v > 0) {
                double position = (double)output_count - 1 + (double)-previous / (v - previous);
                if (last_crossing >= 0) {
                    double hz = 16000 / (position - last_crossing);
                    assert(hz > 440 * .98 && hz < 440 * 1.02);
                }
                last_crossing = position;
            }
            previous = v; ++output_count;
        }
    }
    assert(audio_ready && audio_underruns == 1 && !audio_dropped);
    // Stretch musical time, not note pitch: at half guest speed, a change
    // after one guest second must reach the output after two host seconds.
    reset_audio_queue(); source = output_count = 0;
    unsigned late_crossings = 0; previous = 0;
    for (unsigned ms = 0; ms < 6000; ++ms) {
        if (!(ms % 40)) for (unsigned n = 0; n < 320; ++n) {
            unsigned hz = source < 16000 ? 440 : 660;
            int16_t v = 12000 * std::sin(source++ * 6.283185307179586 * hz / 16000);
            solar_os_amiga_audio_sample(v, -v);
        }
        if (ms % 16) continue;
        int16_t samples[512]; render_audio(samples, 256, 16000, nullptr);
        if (ms < 4000) continue;
        for (unsigned n = 0; n < 256; ++n) {
            int v = samples[n * 2];
            if (output_count && previous <= 0 && v > 0) ++late_crossings;
            previous = v; ++output_count;
        }
    }
    double late_hz = (double)late_crossings * 16000 / output_count;
    assert(late_hz > 650 && late_hz < 670 && !audio_dropped && !audio_underruns);
    audio = false; release(ring); release(pixels); release(chip_ram);
    ring = nullptr; pixels = chip_ram = nullptr; assert(live.empty());
    puts("AMIGA_MODULE_OK: CPU, chipset, media validation, input, audio, suspend/resume and cleanup");
}
