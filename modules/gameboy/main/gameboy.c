#include "solar_os_gameboy_rom.h"
#include "solar_os_gameboy_video.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_app_abi.h"
#include <setjmp.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE 16000
#define MINIGB_APU_AUDIO_FORMAT_S16SYS 1
#include "vendor/peanut_gb/minigb_apu.h"
static uint8_t gameboy_audio_read(uint16_t address);
static void gameboy_audio_write(uint16_t address, uint8_t value);
#define audio_read gameboy_audio_read
#define audio_write gameboy_audio_write
#define ENABLE_SOUND 1
#define PEANUT_GB_12_COLOUR 0
#undef VERTICAL_SYNC
#include "vendor/peanut_gb/peanut_gb.h"
#undef audio_read
#undef audio_write

#define GAMEBOY_ROM_BANK_BYTES (16U * 1024U)
#define FRAME_PERIOD_US 16743ULL
#define INPUT_PULSE_US 140000ULL
#define SOLAR_OS_STORAGE_PATH_MAX 160U

typedef struct {
    struct gb_s *core;
    struct minigb_apu_ctx *apu;
    uint8_t *rom, *rom_fixed_cache, *rom_switch_cache, *cart_ram, *bitmap;
    size_t rom_size, rom_switch_cache_base, cart_ram_size;
    char rom_path[160], save_path[160];
    bool cart_ram_dirty, frame_rendered, paused, audio_ready, exit;
    struct {
        uint16_t physical, usage;
        uint8_t source, key, mask;
    } held[32];
    uint64_t release_at[8], next_frame_us, stats_started, emulation_us;
    uint32_t frames, timing_rebases;
    uint8_t buttons_source;
    jmp_buf fault;
} gameboy_state_t;
static gameboy_state_t *state;
static const solar_os_native_host_api_v1_t *host;
static const solar_os_native_memory_api_v1_t *memory;
static const solar_os_native_foreground_api_v1_t *app;

SOLAR_OS_NATIVE_APP_MANIFEST = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
    .lifecycle_abi = SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_app_manifest_v2_t),
    .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI,
    .flags = SOLAR_OS_NATIVE_APP_RESUMABLE,
    .worker_stack_bytes = 8192U,
    .worker_priority = 2U,
    .minimum_internal_bytes = 16U * 1024U,
    .minimum_psram_bytes = 256U * 1024U,
    .tick_interval_ms = 5U,
    .id = "gameboy",
    .summary = "original Game Boy emulator",
    .file_extensions = ".gb",
};

static void report(const char *text) { host->write_utf8(text, strlen(text)); }
static void number(char *text, size_t *used, uint64_t value) {
    char digits[24];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value);
    while (n)
        text[(*used)++] = digits[--n];
    text[*used] = 0;
}
static void field(char *text, size_t *used, const char *name, uint64_t value) {
    size_t n = strlen(name);
    memcpy(text + *used, name, n);
    *used += n;
    number(text, used, value);
}
static void stats(void) {
    if (state->frames < 120U)
        return;
    uint64_t elapsed = app->time_us() - state->stats_started;
    solar_os_native_foreground_video_stats_v1_t video = {0};
    app->video_stats(&video);
    char text[240] = "gameboy:";
    size_t used = strlen(text);
    field(text, &used, " frames=", state->frames);
    field(text, &used, " emu_fps_x100=", elapsed ? state->frames * 100000000ULL / elapsed : 0);
    field(text, &used, " present_fps_x100=", elapsed ? video.frames * 100000000ULL / elapsed : 0);
    field(text, &used, " avg_emu_us=", state->emulation_us / state->frames);
    field(text, &used, " dropped=", video.replaced_frames);
    field(text, &used, " timing_rebases=", state->timing_rebases);
    report(text);
    state->frames = 0;
    state->emulation_us = 0;
    state->timing_rebases = 0;
    state->stats_started = app->time_us();
}
static void render_audio(int16_t *samples, size_t frames, uint32_t rate, void *user) {
    gameboy_state_t *s = user;
    if (frames != AUDIO_SAMPLES || rate != AUDIO_SAMPLE_RATE)
        return;
    minigb_apu_audio_callback(s->apu, samples);
    for (size_t i = 0; i < frames * 2U; i++) {
        int32_t value = (int32_t)samples[i] * 4;
        if (value > INT16_MAX)
            value = INT16_MAX;
        if (value < INT16_MIN)
            value = INT16_MIN;
        samples[i] = (int16_t)value;
    }
}
static uint8_t gameboy_audio_read(uint16_t address) {
    if (!state || !state->apu || !app->synth_lock())
        return 0xff;
    uint8_t value = minigb_apu_audio_read(state->apu, address);
    app->synth_unlock();
    return value;
}
static void gameboy_audio_write(uint16_t address, uint8_t value) {
    if (state && state->apu && app->synth_lock()) {
        minigb_apu_audio_write(state->apu, address, value);
        app->synth_unlock();
    }
}
static void audio_resume(void) {
    state->audio_ready = app->synth_open(render_audio, state, AUDIO_SAMPLES, AUDIO_SAMPLE_RATE) ==
                         SOLAR_OS_NATIVE_OK;
}
static void gameboy_core_error(struct gb_s *core, enum gb_error_e error, uint16_t address) {
    (void)core;
    (void)error;
    (void)address;
    longjmp(state->fault, 1);
}
static uint8_t gameboy_rom_read(struct gb_s *core, const uint_fast32_t address) {
    gameboy_state_t *state = core->direct.priv;
    if (state == NULL || state->rom == NULL || address >= state->rom_size) {
        return 0xFFU;
    }

    if (address < GAMEBOY_ROM_BANK_BYTES && state->rom_fixed_cache != NULL) {
        return state->rom_fixed_cache[address];
    }
    if (address >= GAMEBOY_ROM_BANK_BYTES && state->rom_switch_cache != NULL) {
        const size_t bank_base = (size_t)address & ~(GAMEBOY_ROM_BANK_BYTES - 1U);
        if (bank_base != state->rom_switch_cache_base) {
            const size_t available = state->rom_size - bank_base;
            const size_t copy_len =
                available < GAMEBOY_ROM_BANK_BYTES ? available : GAMEBOY_ROM_BANK_BYTES;
            memcpy(state->rom_switch_cache, state->rom + bank_base, copy_len);
            if (copy_len < GAMEBOY_ROM_BANK_BYTES) {
                memset(state->rom_switch_cache + copy_len, 0xFF, GAMEBOY_ROM_BANK_BYTES - copy_len);
            }
            state->rom_switch_cache_base = bank_base;
        }
        return state->rom_switch_cache[(size_t)address - state->rom_switch_cache_base];
    }
    return state->rom[address];
}
static uint8_t gameboy_cart_ram_read(struct gb_s *core, const uint_fast32_t address) {
    gameboy_state_t *state = core->direct.priv;
    if (state == NULL || state->cart_ram == NULL || address >= state->cart_ram_size) {
        return 0xFFU;
    }
    return state->cart_ram[address];
}
static void gameboy_cart_ram_write(struct gb_s *core, const uint_fast32_t address,
                                   const uint8_t value) {
    gameboy_state_t *state = core->direct.priv;
    if (state == NULL || state->cart_ram == NULL || address >= state->cart_ram_size) {
        return;
    }
    if (state->cart_ram[address] != value) {
        state->cart_ram[address] = value;
        state->cart_ram_dirty = true;
    }
}
static void gameboy_draw_line(struct gb_s *core, const uint8_t *pixels, const uint_fast8_t line) {
    gameboy_state_t *state = core->direct.priv;
    if (state == NULL || state->bitmap == NULL) {
        return;
    }
    state->frame_rendered = true;
    (void)solar_os_gameboy_video_scanline(state->bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES, pixels,
                                          (size_t)line);
}
static const char *last_char(const char *text, char ch) {
    const char *last = NULL;
    for (; *text; text++)
        if (*text == ch)
            last = text;
    return last;
}
static bool gameboy_make_save_path(const char *rom_path, char *save_path, size_t save_path_len) {
    if (rom_path == NULL || save_path == NULL || save_path_len == 0) {
        return false;
    }
    const char *slash = last_char(rom_path, '/');
    const char *dot = last_char(rom_path, '.');
    size_t base_len = strlen(rom_path);
    if (dot != NULL && (slash == NULL || dot > slash)) {
        base_len = (size_t)(dot - rom_path);
    }
    if (base_len + sizeof(".sav") > save_path_len) {
        save_path[0] = '\0';
        return false;
    }
    memcpy(save_path, rom_path, base_len);
    memcpy(save_path + base_len, ".sav", sizeof(".sav"));
    return true;
}
static uint8_t gameboy_button_for_char(uint8_t ch) {
    if (ch == SOLAR_OS_KEY_RIGHT || ch == SOLAR_OS_KEY_CTRL_RIGHT) {
        return JOYPAD_RIGHT;
    }
    if (ch == SOLAR_OS_KEY_LEFT || ch == SOLAR_OS_KEY_CTRL_LEFT) {
        return JOYPAD_LEFT;
    }
    if (ch == SOLAR_OS_KEY_UP || ch == SOLAR_OS_KEY_CTRL_UP) {
        return JOYPAD_UP;
    }
    if (ch == SOLAR_OS_KEY_DOWN || ch == SOLAR_OS_KEY_CTRL_DOWN) {
        return JOYPAD_DOWN;
    }
    if (ch == 'z' || ch == 'Z') {
        return JOYPAD_A;
    }
    if (ch == 'x' || ch == 'X') {
        return JOYPAD_B;
    }
    if (ch == '\n' || ch == '\r') {
        return JOYPAD_START;
    }
    if (ch == '\b' || ch == SOLAR_OS_KEY_DELETE) {
        return JOYPAD_SELECT;
    }
    return 0;
}

static bool read_exact(const char *path, void *buffer, size_t size) {
    for (size_t at = 0; at < size;) {
        size_t count = size - at, read = 0;
        if (count > 65536U)
            count = 65536U;
        if (app->read_file(path, (uint32_t)at, (uint8_t *)buffer + at, count, &read) ||
            read != count)
            return false;
        at += read;
    }
    return true;
}
static void *hot_alloc(size_t size) {
    void *buffer = memory->alloc_internal(size);
    return buffer ? buffer : memory->alloc(size);
}
static uint8_t button(const solar_os_native_foreground_key_v1_t *key) {
    if (state->buttons_source && key->source == state->buttons_source) {
        if (key->key == '\n' || key->key == '\r')
            return JOYPAD_A;
        if (key->key == ' ')
            return JOYPAD_B;
        if (key->key == SOLAR_OS_KEY_APP_EXIT)
            return JOYPAD_START;
        if (key->key == SOLAR_OS_KEY_ESCAPE)
            return JOYPAD_SELECT;
    }
    if (key->usage == 0x1d)
        return JOYPAD_A;
    if (key->usage == 0x1b)
        return JOYPAD_B;
    if (key->usage >= 0x04 && key->usage <= 0x1d)
        return 0;
    return gameboy_button_for_char(key->key);
}
static void clear_inputs(void) {
    memset(state->held, 0, sizeof(state->held));
    memset(state->release_at, 0, sizeof(state->release_at));
    state->core->direct.joypad = 0xff;
}
static void held_key(const solar_os_native_foreground_key_v1_t *key, uint8_t mask) {
    size_t empty = SIZE_MAX;
    for (size_t i = 0; i < 32U; ++i) {
        if (!state->held[i].mask) {
            if (empty == SIZE_MAX)
                empty = i;
            continue;
        }
        if (state->held[i].source == key->source && state->held[i].physical == key->physical_key &&
            state->held[i].usage == key->usage &&
            (key->physical_key || key->usage || state->held[i].key == key->key)) {
            state->held[i].mask = mask;
            return;
        }
    }
    if (!mask)
        return;
    if (empty == SIZE_MAX) {
        clear_inputs();
        empty = 0U;
    }
    state->held[empty].source = key->source;
    state->held[empty].physical = key->physical_key;
    state->held[empty].usage = key->usage;
    state->held[empty].key = key->key;
    state->held[empty].mask = mask;
}
static void input(void) {
    solar_os_native_foreground_key_v1_t key;
    bool available;
    while (app->read_key(&key, &available) == SOLAR_OS_NATIVE_OK && available) {
        if (key.reset) {
            clear_inputs();
            continue;
        }
        bool board = state->buttons_source && key.source == state->buttons_source;
        bool release = key.action == SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
        if (!release && !board) {
            if (key.key == 'q' || key.key == SOLAR_OS_KEY_ESCAPE ||
                key.key == SOLAR_OS_KEY_APP_EXIT) {
                state->exit = true;
                return;
            }
            if (key.key == 'p') {
                if (key.action != SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS)
                    continue;
                state->paused = !state->paused;
                clear_inputs();
                if (state->paused)
                    app->synth_close();
                else
                    audio_resume();
                continue;
            }
            if (key.key == 'r') {
                if (key.action != SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS)
                    continue;
                gb_reset(state->core);
                if (app->synth_lock()) {
                    minigb_apu_audio_init(state->apu);
                    app->synth_unlock();
                }
                clear_inputs();
                solar_os_gameboy_video_clear(state->bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
                app->present(state->bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
                state->next_frame_us = app->time_us();
                continue;
            }
        }
        uint8_t mask = button(&key);
        if (key.physical_key || key.usage || board) {
            held_key(&key, release ? 0 : mask);
        } else if (!release) {
            for (unsigned bit = 0; bit < 8; bit++)
                if (mask & (1U << bit))
                    state->release_at[bit] = app->time_us() + INPUT_PULSE_US;
        }
        if (board) {
            uint8_t pressed = 0;
            for (unsigned i = 0; i < 32U; i++)
                pressed |= state->held[i].mask;
            if ((pressed & (JOYPAD_START | JOYPAD_SELECT)) == (JOYPAD_START | JOYPAD_SELECT))
                state->exit = true;
        }
    }
}
static bool run_frame(void) {
    if (setjmp(state->fault)) {
        report("gameboy: emulator core error");
        return false;
    }
    state->frame_rendered = false;
    uint64_t started = app->time_us();
    gb_run_frame(state->core);
    state->emulation_us += app->time_us() - started;
    state->frames++;
    if (state->frame_rendered)
        app->present(state->bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
    state->next_frame_us += FRAME_PERIOD_US;
    stats();
    return true;
}
static bool load(const char *path) {
    uint32_t size = 0;
    if (app->resolve_path(path, state->rom_path, sizeof(state->rom_path)) ||
        app->file_size(state->rom_path, &size) || size > SOLAR_OS_GAMEBOY_ROM_MAX_BYTES ||
        size < SOLAR_OS_GAMEBOY_ROM_HEADER_BYTES) {
        report("gameboy: invalid ROM path or size");
        return false;
    }
    state->rom_size = size;
    state->rom = memory->alloc(size);
    if (!state->rom || !read_exact(state->rom_path, state->rom, size)) {
        report("gameboy: ROM read failed");
        return false;
    }
    solar_os_gameboy_rom_status_t status = solar_os_gameboy_rom_validate(state->rom, size);
    if (status != SOLAR_OS_GAMEBOY_ROM_OK) {
        report(solar_os_gameboy_rom_status_name(status));
        return false;
    }
    state->core = hot_alloc(sizeof(*state->core));
    state->apu = hot_alloc(sizeof(*state->apu));
    state->bitmap = memory->alloc(SOLAR_OS_GAMEBOY_BITMAP_BYTES);
    if (!state->core || !state->apu || !state->bitmap) {
        report("gameboy: allocation failed");
        return false;
    }
    memset(state->core, 0, sizeof(*state->core));
    memset(state->apu, 0, sizeof(*state->apu));
    minigb_apu_audio_init(state->apu);
    state->rom_fixed_cache = memory->alloc_internal(GAMEBOY_ROM_BANK_BYTES);
    state->rom_switch_cache = memory->alloc_internal(GAMEBOY_ROM_BANK_BYTES);
    state->rom_switch_cache_base = SIZE_MAX;
    if (state->rom_fixed_cache)
        memcpy(state->rom_fixed_cache, state->rom, GAMEBOY_ROM_BANK_BYTES);
    if (gb_init(state->core, gameboy_rom_read, gameboy_cart_ram_read, gameboy_cart_ram_write,
                gameboy_core_error, state) != GB_INIT_NO_ERROR) {
        report("gameboy: unsupported cartridge controller");
        return false;
    }
    if (gb_get_save_size_s(state->core, &state->cart_ram_size))
        return false;
    if (state->cart_ram_size) {
        state->cart_ram = memory->alloc(state->cart_ram_size);
        if (!state->cart_ram ||
            !gameboy_make_save_path(state->rom_path, state->save_path, sizeof(state->save_path)))
            return false;
        memset(state->cart_ram, 0xff, state->cart_ram_size);
        uint32_t save_size = 0;
        if (app->file_size(state->save_path, &save_size) == SOLAR_OS_NATIVE_OK) {
            if (save_size != state->cart_ram_size ||
                !read_exact(state->save_path, state->cart_ram, save_size))
                report("gameboy: save size mismatch or read failed");
        }
    }
    gb_init_lcd(state->core, gameboy_draw_line);
    state->core->direct.frame_skip = true;
    uint8_t rtc[5];
    if (app->rtc_read(rtc) == SOLAR_OS_NATIVE_OK)
        memcpy(state->core->rtc_real.bytes, rtc, 5);
    solar_os_gameboy_video_clear(state->bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
    if (app->video_open_index2(160, 144, 25) != SOLAR_OS_NATIVE_OK) {
        report("gameboy: display setup failed");
        return false;
    }
    state->buttons_source = app->buttons_source();
    audio_resume();
    report(state->audio_ready ? "gameboy: loaded with audio" : "gameboy: loaded without audio");
    return true;
}
static void cleanup(void) {
    if (!state)
        return;
    app->synth_close();
    memory->free(state->rom_switch_cache);
    memory->free(state->rom_fixed_cache);
    memory->free(state->cart_ram);
    memory->free(state->bitmap);
    memory->free(state->apu);
    memory->free(state->core);
    memory->free(state->rom);
    memory->free(state);
    state = NULL;
}
int main(int argc, char **argv) {
    host = solar_os_native_host_v1();
    if (!host || host->struct_size < sizeof(*host) || !host->get_service)
        return 1;
    app = host->get_service(SOLAR_OS_NATIVE_FOREGROUND_SERVICE, 1,
        offsetof(solar_os_native_foreground_api_v1_t, video_stats) + sizeof(app->video_stats));
    memory = host->get_service(SOLAR_OS_NATIVE_MEMORY_SERVICE, 1, sizeof(*memory));
    if (!app || !memory)
        return 1;
    app->set_exit_code(1);
    if (argc != 2) {
        report("usage: gameboy <rom.gb>");
        return 1;
    }
    state = memory->alloc(sizeof(*state));
    if (!state)
        return 1;
    memset(state, 0, sizeof(*state));
    if (!load(argv[1])) {
        cleanup();
        return 1;
    }
    int result = 0;
    state->next_frame_us = state->stats_started = app->time_us();
    while (!state->exit && !app->stop_requested()) {
        if (app->suspend_requested()) {
            clear_inputs();
            if (app->suspend() != SOLAR_OS_NATIVE_OK) {
                result = 1;
                break;
            }
            if (app->stop_requested())
                break;
            clear_inputs();
            state->next_frame_us = state->stats_started = app->time_us();
            state->frames = 0;
            state->emulation_us = 0;
            if (!state->paused)
                audio_resume();
            app->present(state->bitmap, SOLAR_OS_GAMEBOY_BITMAP_BYTES);
        }
        input();
        if (state->exit)
            break;
        uint64_t now = app->time_us();
        if (state->paused) {
            state->next_frame_us = now;
            app->sleep_ms(5);
            continue;
        }
        uint8_t pressed = 0;
        for (unsigned i = 0; i < 32U; i++)
            pressed |= state->held[i].mask;
        for (unsigned bit = 0; bit < 8; bit++)
            if (now < state->release_at[bit])
                pressed |= 1U << bit;
        state->core->direct.joypad = (uint8_t)~pressed;
        if (now < state->next_frame_us) {
            app->sleep_ms((uint32_t)((state->next_frame_us - now + 999) / 1000));
            continue;
        }
        uint32_t due = (uint32_t)((now - state->next_frame_us) / FRAME_PERIOD_US) + 1;
        if (due > 3)
            due = 3;
        for (uint32_t i = 0; i < due; i++)
            if (!run_frame()) {
                result = 1;
                state->exit = true;
                break;
            }
        now = app->time_us();
        if (now > state->next_frame_us && now - state->next_frame_us > FRAME_PERIOD_US * 6) {
            state->next_frame_us = now;
            state->timing_rebases++;
        }
    }
    app->synth_close();
    if (state->cart_ram_dirty && app->replace_file(state->save_path, state->cart_ram,
                                                   state->cart_ram_size) != SOLAR_OS_NATIVE_OK) {
        report("gameboy: save failed");
        result = 1;
    }
    cleanup();
    app->set_exit_code(result);
    return result;
}
