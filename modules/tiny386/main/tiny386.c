#include "port.h"
#include "vendor/pc.h"
#include "text.h"
#include "solar_os_key_codes.h"

#define WIDTH 640U
#define HEIGHT 400U
extern const uint8_t tiny_bios[], tiny_vgabios[];
extern const size_t tiny_bios_size, tiny_vgabios_size;
static PC *machine;
static uint16_t *rgb;
static uint8_t *indexed;
static uint16_t *text_scratch;
static unsigned present_width, present_height;
static bool changed;
static bool force_present;
static bool held[256];

static void fail_display(int result)
{
    switch (result) {
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: tiny_failure = "display rejected frame dimensions"; break;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: tiny_failure = "display setup ran out of memory"; break;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: tiny_failure = "display frame format unsupported"; break;
    case SOLAR_OS_NATIVE_ERROR_INVALID_STATE: tiny_failure = "display is busy or not ready"; break;
    default: tiny_failure = "display setup failed"; break;
    }
    tiny_abort();
}

SOLAR_OS_NATIVE_APP_MANIFEST = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
    .lifecycle_abi = SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_app_manifest_v2_t),
    .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI,
    .flags = SOLAR_OS_NATIVE_APP_RESUMABLE,
    .worker_stack_bytes = 16U * 1024U, .worker_priority = 1U,
    .minimum_internal_bytes = 8U * 1024U,
    .minimum_psram_bytes = 5U * 1024U * 1024U,
    .tick_interval_ms = 5U,
    .id = "tiny386", .summary = "386 PC emulator with SeaBIOS",
    .file_extensions = ".img",
};

int load_rom(void *memory, const char *name, uword address, int backward)
{
    bool vga = !tiny_strcmp(name, "vgabios.bin");
    size_t size = vga ? tiny_vgabios_size : tiny_bios_size;
    if (backward) address -= (uword)size;
    if (address > 2U * 1024U * 1024U || size > 2U * 1024U * 1024U - address) tiny_abort();
    memcpy((uint8_t *)memory + address, vga ? tiny_vgabios : tiny_bios, size);
    return (int)size;
}
static void redraw(void *opaque, int x, int y, int w, int h)
{ (void)opaque; (void)x; (void)y; (void)w; (void)h; changed = true; }

static unsigned usage_code(unsigned usage)
{
    static const uint8_t letters[26] = {30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
    static const uint8_t basic[] = {28,1,14,15,57,12,13,26,27,43,86,39,40,41,51,52,53,58};
    static const uint8_t nav[] = {99,70,119,110,102,104,111,107,109,106,105,108,103};
    static const uint8_t keypad[] = {69,98,55,74,78,96,79,80,81,75,76,77,71,72,73,82,83};
    static const uint8_t modifiers[8] = {29,42,56,125,97,54,100,126};
    if (usage >= 4 && usage <= 29) return letters[usage - 4];
    if (usage >= 30 && usage <= 39) return usage - 28;
    if (usage >= 40 && usage <= 57) return basic[usage - 40];
    if (usage >= 58 && usage <= 67) return usage + 1;
    if (usage == 68 || usage == 69) return usage + 19;
    if (usage >= 70 && usage <= 82) return nav[usage - 70];
    if (usage >= 83 && usage <= 99) return keypad[usage - 83];
    if (usage >= 224 && usage <= 231) return modifiers[usage - 224];
    return 0;
}
static void set_key(unsigned code, bool down)
{
    if (!code || code >= sizeof(held) || held[code] == down) return;
    held[code] = down; ps2_put_keycode(machine->kbd, down, (int)code);
}
static void reset_keys(void)
{
    for (unsigned code = 0; code < sizeof(held); ++code) if (held[code]) set_key(code, false);
}
static void keyboard(void)
{
    solar_os_native_foreground_key_v1_t k;
    bool available;
    for (unsigned i = 0; i < 32; ++i) {
        if (tiny_app->read_key(&k, &available) || !available) break;
        if (k.reset) { reset_keys(); continue; }
        static const uint8_t mods[8] = {29,42,56,125,97,54,100,126};
        for (unsigned m = 0; m < 8; ++m) set_key(mods[m], (k.modifiers & (1U << m)) != 0);
        unsigned code = usage_code(k.usage);
        if (code) {
            if (k.action != SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE && held[code])
                ps2_put_keycode(machine->kbd, true, (int)code);
            else set_key(code, k.action != SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE);
            continue;
        }
        if (k.action == SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE) continue;
        /* Logical events (including shell injection) have no physical release. */
        uint8_t ch = k.key;
        bool shift = false, control = false;
        if (ch >= SOLAR_OS_KEY_F1 && ch <= SOLAR_OS_KEY_F12)
            code = ch <= SOLAR_OS_KEY_F10 ? 59 + ch - SOLAR_OS_KEY_F1 : 87 + ch - SOLAR_OS_KEY_F11;
        else if (ch >= SOLAR_OS_KEY_UP && ch <= SOLAR_OS_KEY_PAGE_DOWN) {
            static const uint8_t codes[] = {103,108,105,106,104,109};
            code = codes[ch - SOLAR_OS_KEY_UP];
        } else if (ch == SOLAR_OS_KEY_HOME) code = 102;
        else if (ch == SOLAR_OS_KEY_END) code = 107;
        else if (ch == SOLAR_OS_KEY_DELETE) code = 111;
        else if (ch > 0 && ch < 27 && ch != '\t' && ch != '\b' && ch != '\n' && ch != '\r') {
            control = true; ch += 'a' - 1;
        }
        if (ch >= 'A' && ch <= 'Z') { shift = true; ch += 'a' - 'A'; }
        if (ch >= 'a' && ch <= 'z') code = usage_code(4 + ch - 'a');
        else if (ch >= '1' && ch <= '9') code = 2 + ch - '1';
        else if (ch == '0') code = 11;
        else {
            switch (ch) {
            case '\r': case '\n': code = 28; break;
            case ' ': code = 57; break;
            case '\t': code = 15; break;
            case '\b': case 127: code = 14; break;
            case 27: code = 1; break;
            case '.': code = 52; break;
            case '/': code = 53; break;
            case '-': code = 12; break;
            case '=': code = 13; break;
            case ':': code = 39; shift = true; break;
            case ';': code = 39; break;
            case ',': code = 51; break;
            case '<': code = 51; shift = true; break;
            case '>': code = 52; shift = true; break;
            case '?': code = 53; shift = true; break;
            case '_': code = 12; shift = true; break;
            case '+': code = 13; shift = true; break;
            case '[': code = 26; break;
            case ']': code = 27; break;
            case '{': code = 26; shift = true; break;
            case '}': code = 27; shift = true; break;
            case '\'': code = 40; break;
            case '"': code = 40; shift = true; break;
            case '`': code = 41; break;
            case '~': code = 41; shift = true; break;
            case '|': code = 43; shift = true; break;
            case '!': code = 2; shift = true; break;
            case '@': code = 3; shift = true; break;
            case '#': code = 4; shift = true; break;
            case '$': code = 5; shift = true; break;
            case '%': code = 6; shift = true; break;
            case '^': code = 7; shift = true; break;
            case '&': code = 8; shift = true; break;
            case '*': code = 9; shift = true; break;
            case '(': code = 10; shift = true; break;
            case ')': code = 11; shift = true; break;
            case '\\': code = 43; break;
            default: break;
            }
        }
        if (shift) set_key(42, true);
        if (control) set_key(29, true);
        set_key(code, true); set_key(code, false);
        if (control) set_key(29, false);
        if (shift) set_key(42, false);
    }
}
static void present(void)
{
    if (!changed) return;
    VGATextFrame text;
    if (!vga_get_text_frame(machine->vga, &text) ||
        !tiny_render_text(&text, indexed, present_width, present_height, text_scratch)) {
        tiny_text_reset(text_scratch);
        unsigned w = present_width, h = w * HEIGHT / WIDTH;
        if (h > present_height) { h = present_height; w = h * WIDTH / HEIGHT; }
        unsigned left = (present_width - w) / 2, top = (present_height - h) / 2;
        memset(indexed, 0, present_width * present_height);
        for (size_t y = 0; y < h; ++y)
            for (size_t x = 0; x < w; ++x) {
                uint16_t c = rgb[(y * HEIGHT / h) * WIDTH + x * WIDTH / w];
                indexed[(top + y) * present_width + left + x] =
                    (uint8_t)(((c >> 8) & 0xe0) | ((c >> 6) & 0x1c) | ((c >> 3) & 3));
            }
    } else if (!tiny_text_changed(text_scratch) && !force_present) {
        changed = false;
        return;
    }
    if (tiny_app->present(indexed, present_width * present_height)) {
        tiny_failure = "display submission failed"; tiny_abort();
    }
    changed = false;
    force_present = false;
}
int main(int argc, char **argv)
{
    tiny_host = solar_os_native_host_v1();
    if (!tiny_host || !tiny_host->get_service) return 1;
    tiny_app = tiny_host->get_service("foreground", 1,
        offsetof(solar_os_native_foreground_api_v1_t, disk_close) + sizeof(tiny_app->disk_close));
    tiny_memory = tiny_host->get_service("memory", 1, offsetof(solar_os_native_memory_api_v1_t, alloc_internal));
    if (!tiny_app || !tiny_memory) { tiny_report("tiny386: foreground services unavailable"); return 1; }
    const char *floppy_arg = NULL, *hard_arg = NULL;
    if (argc == 2) floppy_arg = argv[1];
    else if (argc == 3 && !strcmp(argv[1], "--hdd")) hard_arg = argv[2];
    else if (argc == 4 && !strcmp(argv[2], "--hdd")) { floppy_arg = argv[1]; hard_arg = argv[3]; }
    else { tiny_report("usage: tiny386 [floppy.img] [--hdd harddisk.img]"); tiny_app->set_exit_code(1); return 1; }
    char floppy_path[160], hard_path[160];
    uint32_t size;
    if (floppy_arg && (tiny_app->resolve_path(floppy_arg, floppy_path, sizeof(floppy_path)) ||
        tiny_app->file_size(floppy_path, &size) ||
        (size != 720U * 1024U && size != 1200U * 1024U && size != 1440U * 1024U && size != 2880U * 1024U))) {
        tiny_report("tiny386: expected a 720K, 1.2M, 1.44M or 2.88M floppy image"); tiny_app->set_exit_code(1); return 1;
    }
    if (hard_arg && (tiny_app->resolve_path(hard_arg, hard_path, sizeof(hard_path)) ||
        tiny_app->file_size(hard_path, &size) || !size || size > 0x7ffffe00U || size % 512)) {
        tiny_report("tiny386: hard disk must be a raw image with 512-byte sectors, smaller than 2 GiB");
        tiny_app->set_exit_code(1); return 1;
    }
    int result = 0;
    machine = NULL; rgb = NULL; indexed = NULL; changed = false; force_present = true; tiny_failure = NULL;
    memset(held, 0, sizeof(held));
    if (setjmp(tiny_trap)) { result = 1; goto done; }
    tiny_clock_reset();
    rgb = tiny_calloc(WIDTH * HEIGHT, sizeof(*rgb));
    PCConfig config = {0};
    config.mem_size = 2 * 1024 * 1024; config.vga_mem_size = 256 * 1024;
    config.cpu_gen = 3; config.width = WIDTH; config.height = HEIGHT;
    config.vga_force_8dm = 1; config.fdd[0] = floppy_arg ? floppy_path : NULL;
    config.disks[0] = hard_arg ? hard_path : NULL; config.fill_cmos = 1;
    machine = pc_new(redraw, NULL, (uint8_t *)rgb, &config);
    /* QEMU SeaBIOS reads this CMOS field for ATA geometry translation. LBA
     * translation also gives DOS a usable CHS view beyond 504 MiB. */
    if (hard_arg) cmos_set(machine->cmos, 0x39, 1);
    uint16_t palette[256];
    for (unsigned i = 0; i < 256; ++i) {
        unsigned r = (i >> 5) * 31 / 7, g = ((i >> 2) & 7) * 63 / 7, b = (i & 3) * 31 / 3;
        palette[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
    uint16_t display_width, display_height;
    int video_result = tiny_app->video_dimensions(&display_width, &display_height);
    if (video_result) fail_display(video_result);
    present_width = display_width; present_height = display_height;
    video_result = tiny_app->video_open_ex(display_width, display_height, palette, 15, 0);
    if (video_result) fail_display(video_result);
    indexed = tiny_malloc(present_width * present_height);
    text_scratch = tiny_malloc(TINY_TEXT_SCRATCH_BYTES);
    tiny_text_reset(text_scratch);
    if (tiny_app->enable_suspend()) { tiny_failure = "suspend setup failed"; tiny_abort(); }
    load_bios_and_reset(machine);
    tiny_report(hard_arg ? "tiny386: 386, 2 MiB RAM; IDE hard disk attached" :
        "tiny386: 386, 2 MiB RAM; booting floppy with SeaBIOS");
    uint64_t next_frame = 0;
    while (!tiny_app->stop_requested() && machine->shutdown_state != 8) {
        if (tiny_app->suspend_requested()) {
            reset_keys();
            uint64_t before = tiny_app->time_us();
            if (tiny_app->suspend()) break;
            tiny_clock_pause(tiny_app->time_us() - before);
            reset_keys(); machine->full_update = 2; changed = true; force_present = true; next_frame = 0;
            if (tiny_app->stop_requested()) break;
        }
        keyboard();
        uint64_t start = tiny_app->time_us();
        do { pc_step(machine); } while (tiny_app->time_us() - start < 8000);
        if (tiny_app->time_us() >= next_frame) {
            pc_vga_step(machine); present(); next_frame = tiny_app->time_us() + 66666;
        }
        solar_os_native_foreground_cooperate(tiny_app);
    }
done:
    if (tiny_failure) { tiny_report("tiny386: emulation stopped"); tiny_report(tiny_failure); }
    if (tiny_save_disks()) { tiny_report("tiny386: disk sync failed"); result = 1; }
    if (tiny_cleanup()) { tiny_report("tiny386: disk close failed"); result = 1; }
    machine = NULL; rgb = NULL; indexed = NULL;
    tiny_app->set_exit_code(result); return result;
}
