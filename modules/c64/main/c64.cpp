// SPDX-License-Identifier: GPL-3.0-or-later
#include "solar_os_native_abi.h"
#include "input.h"
#include "roms.h"
#include "audio.h"
#include <atomic>
#include <cstring>
#include <new>

// Freestanding C++ runtime hook referenced by the abstract CPU base vtable.
extern "C" __attribute__((visibility("hidden"))) void __cxa_pure_virtual() { __builtin_trap(); }

static const solar_os_native_host_api_v1_t *host;
static const solar_os_native_foreground_api_v1_t *app;
static const solar_os_native_memory_api_v1_t *memory;
static void *allocations[12];
static unsigned allocation_count;
static C64Sys *machine;
static solar_os_c64_input input;
static bool audio_active, paused, exchanged;
static uint16_t width, height;
static uint8_t *canvas, *program;
static uint32_t program_size;
static int16_t *audio_ring;
static constexpr unsigned ring_size = solar_os_c64_audio::capacity;
static solar_os_c64_audio audio_queue;
static_assert(AUDIO_SAMPLE_RATE == solar_os_c64_audio::source_rate);
static char snapshot_path[160];
static uint32_t guest_frames;
static uint64_t started_at;
static bool snapshot_requested;

static const uint16_t palette[256] = {
    0x0000,0xffff,0x8000,0xa7fc,0xc218,0x064a,0x0014,0xe74e,
    0xd42a,0x6200,0xfbae,0x3186,0x73ae,0xa7ec,0x043f,0xb5d6
};

SOLAR_OS_NATIVE_APP_MANIFEST = {
    SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC, SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    sizeof(solar_os_native_app_manifest_v2_t), SOLAR_OS_NATIVE_APP_CLASS_GUI,
    SOLAR_OS_NATIVE_APP_RESUMABLE | SOLAR_OS_NATIVE_APP_AXIS_EVENTS,
    16U * 1024U, 1, 8U * 1024U, 2U * 1024U * 1024U, 5,
    "c64", "PAL Commodore 64 (64 KiB RAM, VIC-II, CIA, SID)", ".prg"
};

static void report(const char *message) { host->write_utf8(message, strlen(message)); }
static bool same(const char *left, const char *right)
{
    size_t length = strlen(left);
    return length == strlen(right) && !memcmp(left, right, length);
}
static void *allocate(size_t size, bool hot = false)
{
    if (allocation_count == sizeof(allocations) / sizeof(allocations[0])) return nullptr;
    void *p = nullptr;
    if (hot && memory->struct_size >= offsetof(solar_os_native_memory_api_v1_t, alloc_internal) + sizeof(memory->alloc_internal) &&
        memory->alloc_internal) p = memory->alloc_internal(size);
    if (!p) p = memory->alloc(size);
    if (p) { memset(p, 0, size); allocations[allocation_count++] = p; }
    return p;
}

// The worker produces PAL-frame PCM. The callback only consumes this queue;
// it never calls the emulator or any foreground method from the audio task.
static void render_audio(int16_t *samples, size_t frames, uint32_t rate, void *)
{
    audio_queue.render(samples, frames, rate);
}

static void close_audio()
{
    if (audio_active) app->synth_close(); // joins callbacks before queue reset/free
    audio_active = false;
}
static void open_audio()
{
    audio_queue.reset(audio_ring);
    audio_active = false;
    if (!audio_ring) return;
    uint32_t rate = 0; uint8_t channels = 0;
    if (app->audio_open(&rate, &channels)) return;
    app->audio_close();
    if (rate < 8000 || rate > 96000 || !channels || channels > 2) return;
    audio_active = !app->synth_open(render_audio, nullptr, 256, rate);
}
static void queue_audio()
{
    if (audio_active) audio_queue.push(machine->sid.pcm(), SID::pcm_frames);
}

static bool read_exact(const char *path, uint32_t offset, void *buffer, size_t length)
{
    auto *out = (uint8_t *)buffer;
    while (length) {
        if (app->stop_requested()) return false;
        size_t count = length > 65536 ? 65536 : length, actual = 0;
        if (app->read_file(path, offset, out, count, &actual) || actual != count) return false;
        length -= count; offset += count; out += count;
    }
    return true;
}
static bool load_rom(const char *directory, const char *name, uint8_t *buffer, uint32_t length)
{
    char path[160];
    size_t base = strlen(directory), tail = strlen(name);
    bool slash = base && directory[base - 1] != '/';
    if (base + slash + tail >= sizeof(path)) return false;
    memcpy(path, directory, base);
    if (slash) path[base++] = '/';
    memcpy(path + base, name, tail + 1);
    uint32_t size;
    return !app->file_size(path, &size) && size == length && read_exact(path, 0, buffer, length);
}
static bool load_program(const char *name)
{
    char path[160];
    if (app->resolve_path(name, path, sizeof(path)) || app->file_size(path, &program_size) ||
        program_size < 3 || program_size > 65538) return false;
    program = (uint8_t *)allocate(program_size);
    if (!program || !read_exact(path, 0, program, program_size)) return false;
    uint32_t address = program[0] | (program[1] << 8);
    // CPU-port bytes are not program memory. BASIC also needs a representable
    // end pointer; reject malformed loads before any guest writes.
    return address >= 2 && program_size - 2 <= 65536U - address &&
        (address != 0x0801 || address + program_size - 2 < 65536);
}
static void install_program()
{
    uint16_t address = program[0] | (program[1] << 8);
    uint16_t end = address + program_size - 2;
    memcpy(machine->ram + address, program + 2, program_size - 2);
    if (address == 0x0801) {
        for (unsigned pointer = 0x2d; pointer <= 0x31; pointer += 2) {
            machine->ram[pointer] = end; machine->ram[pointer + 1] = end >> 8;
        }
        // Same BASIC CLR entry used by the upstream autostarter.
        machine->setPC(0xa52a);
        memcpy(machine->ram + 0x277, "RUN\r", 4);
        machine->ram[0xc6] = 4;
    } else machine->setPC(address);
}

static int present_frame()
{
    uint8_t border = machine->vic.vicreg[0x20] & 15;
    if (exchanged && width == 320 && height >= 200) {
        unsigned top = (height - 200) / 2;
        memset(canvas, border, top * width);
        memset(canvas + (top + 200) * width, border, (height - top - 200) * width);
        int result = app->video_exchange(&canvas);
        machine->vic.setBitmap(canvas + top * width);
        memset(machine->vic.getBitmap(), 0, 320 * 200);
        return result;
    }
    memset(canvas, border, (size_t)width * height);
    unsigned scale = width / 320, vertical = height / 200;
    if (scale > vertical) scale = vertical;
    unsigned w, h;
    if (scale) { w = 320 * scale; h = 200 * scale; }
    else if ((unsigned)width * 200 <= (unsigned)height * 320) { w = width; h = width * 200 / 320; }
    else { h = height; w = height * 320 / 200; }
    unsigned left = (width - w) / 2, top = (height - h) / 2;
    const uint8_t *source = machine->vic.getBitmap();
    if (w == 320 && h == 200) {
        // The native C64 picture needs no coordinate division on each pixel.
        for (unsigned y = 0; y < 200; ++y) {
            uint8_t *row = canvas + (y + top) * width + left;
            const uint8_t *guest = source + y * 320;
            for (unsigned x = 0; x < 320; ++x) row[x] = guest[x] & 15;
        }
    } else {
        for (unsigned y = 0; y < h; ++y) {
            uint8_t *row = canvas + (y + top) * width + left;
            const uint8_t *guest = source + (y * 200 / h) * 320;
            for (unsigned x = 0; x < w; ++x) row[x] = guest[x * 320 / w] & 15;
        }
    }
    return exchanged ? app->video_exchange(&canvas) : app->present(canvas, (size_t)width * height);
}

static void save_snapshot()
{
    // F2 is reserved only when the caller explicitly enables snapshots.
    if (!snapshot_path[0]) return;
    uint8_t state[80] = {};
    memcpy(state, "C64S", 4);
    auto put = [&](unsigned offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) state[offset + i] = value >> (i * 8);
    };
    put(4, 1); put(8, guest_frames); put(12, machine->getPC());
    put(16, machine->ram[0x400]); put(20, paused); put(24, audio_active);
    put(28, input.keyboard_port);
    memcpy(state + 32, machine->matrix, 8);
    state[40] = machine->joystick[0]; state[41] = machine->joystick[1];
    state[42] = machine->ram[0x401]; state[43] = machine->ram[0x402];
    put(44, (app->time_us() - started_at) / 1000);
    solar_os_native_foreground_video_stats_v1_t stats = {};
    if (!app->video_stats(&stats)) {
        put(48, stats.frames); put(52, stats.replaced_frames);
        put(56, stats.present_us); put(60, stats.present_us >> 32);
    }
    put(64, audio_queue.write.load()); put(68, audio_queue.read.load());
    put(72, audio_queue.underruns.load()); put(76, audio_queue.output_frames.load());
    static const char header[] = "P6\n320 200\n255\n";
    constexpr size_t prefix = sizeof(header) - 1, size = prefix + 320 * 200 * 3;
    auto *picture = (uint8_t *)memory->alloc(size);
    if (!picture) { report("c64: snapshot allocation failed"); return; }
    memcpy(picture, header, prefix);
    const auto *bitmap = machine->vic.getBitmap();
    for (unsigned i = 0; i < 320 * 200; ++i) {
        uint16_t color = palette[bitmap[i] & 15];
        picture[prefix + i * 3] = (color >> 11) * 255 / 31;
        picture[prefix + i * 3 + 1] = ((color >> 5) & 63) * 255 / 63;
        picture[prefix + i * 3 + 2] = (color & 31) * 255 / 31;
    }
    int result = app->replace_file(snapshot_path, picture, size);
    memory->free(picture);
    char path[sizeof(snapshot_path) + 6];
    size_t length = strlen(snapshot_path);
    memcpy(path, snapshot_path, length); memcpy(path + length, ".state", 7);
    if (!result) result = app->replace_file(path, state, sizeof(state));
    report(result ? "c64: snapshot write failed" : "c64: snapshot saved");
}

static int poll_input()
{
    if (app->struct_size >= offsetof(solar_os_native_foreground_api_v1_t, joystick_source) + sizeof(app->joystick_source) &&
        app->joystick_source && app->read_axis) {
        uint8_t source = app->joystick_source();
        if (source != input.joystick_source) { input.clear(); input.joystick_source = source; }
        for (unsigned i = 0; i < 32; ++i) {
            solar_os_native_foreground_axis_v1_t event; bool available = false;
            if (app->read_axis(&event, &available)) return -1;
            if (!available) break;
            input.axis(event);
        }
    }
    for (unsigned i = 0; i < 64; ++i) {
        solar_os_native_foreground_key_v1_t key; bool available = false;
        if (app->read_key(&key, &available)) return -1;
        if (!available) break;
        bool press = !key.reset && key.action == SOLAR_OS_NATIVE_FOREGROUND_KEY_PRESS;
        if (!key.reset && snapshot_path[0] && key.key == SOLAR_OS_KEY_F2) {
            if (press) snapshot_requested = true;
        } else if (press && key.key == SOLAR_OS_KEY_F9) {
            input.clear();
            input.keyboard_port = input.keyboard_port == 0 ? 2 : input.keyboard_port == 2 ? 1 : 0;
            report(input.keyboard_port == 2 ? "c64: keyboard joystick port 2" :
                input.keyboard_port == 1 ? "c64: keyboard joystick port 1" : "c64: keyboard mode");
        } else if (press && key.key == SOLAR_OS_KEY_F10) {
            paused = !paused;
            close_audio(); if (!paused) open_audio();
        } else if (press && key.key == SOLAR_OS_KEY_F11) {
            close_audio(); input.clear(); machine->reset(); if (!paused) open_audio();
        } else if (press && key.key == SOLAR_OS_KEY_F12) machine->restorenmi = true;
        else input.key(key);
    }
    input.apply(*machine);
    return 0;
}

// Original diagnostic firmware, independent of Commodore ROMs. The 6510
// increments screen RAM and cycles the VIC background while SID plays a tone.
static void diagnostic_rom(uint8_t *kernal)
{
    memset(kernal, 0xea, 8192);
    static const uint8_t code[] = {
        0x78, 0xa9,0x37, 0x85,0x01,
        0xa9,0x0f, 0x8d,0x18,0xd4, 0xa9,0x11, 0x8d,0x01,0xd4,
        0xa9,0x00, 0x8d,0x05,0xd4, 0xa9,0xf0, 0x8d,0x06,0xd4,
        0xa9,0x21, 0x8d,0x04,0xd4,
        0xee,0x00,0x04, 0xad,0x00,0x04, 0x29,0x0f, 0x8d,0x21,0xd0,
        0xa9,0xff, 0x8d,0x02,0xdc, 0xa9,0xfd, 0x8d,0x00,0xdc,
        0xad,0x01,0xdc, 0x8d,0x01,0x04, 0xad,0x00,0xdc, 0x8d,0x02,0x04,
        0x4c,0x1e,0xe0
    };
    memcpy(kernal, code, sizeof(code));
    for (unsigned vector = 0x1ffa; vector < 0x2000; vector += 2) {
        kernal[vector] = 0; kernal[vector + 1] = 0xe0;
    }
}

static const char *run(int argc, char **argv)
{
    const char *roms = nullptr, *prg = nullptr;
    bool diagnostic = false;
    for (int i = 1; i < argc; ++i) {
        if (same(argv[i], "--roms") && i + 1 < argc) roms = argv[++i];
        else if (same(argv[i], "--snapshot") && i + 1 < argc) {
            if (app->resolve_path(argv[++i], snapshot_path, sizeof(snapshot_path) - 6))
                return "c64: invalid snapshot path";
        }
        else if (same(argv[i], "--self-test")) diagnostic = true;
        else if (same(argv[i], "--joystick") && i + 1 < argc) {
            const char *port = argv[++i];
            if (same(port, "1")) input.keyboard_port = 1;
            else if (same(port, "2")) input.keyboard_port = 2;
            else return "c64: joystick port must be 1 or 2";
        } else if (argv[i][0] == '-' || prg) return "usage: c64 [--roms directory] [--joystick 1|2] [program.prg]";
        else prg = argv[i];
    }
    if (diagnostic && roms) return "c64: --self-test does not take ROM files";
    if (diagnostic && prg) {
        if (snapshot_path[0]) return "c64: specify only one snapshot path";
        // Leave room for the sidecar suffix within the host's path limit.
        if (app->resolve_path(prg, snapshot_path, sizeof(snapshot_path) - 6))
            return "c64: invalid diagnostic snapshot path";
        prg = nullptr;
    }
    if (app->video_dimensions(&width, &height) || width < 64 || height < 64 || width > 1024 || height > 1024)
        return "c64: display dimensions must be 64..1024 pixels";
    // Registers, opcode dispatch and chipset state are touched on every guest
    // instruction. Keep this small hot object in SRAM when the host permits it.
    auto *object = allocate(sizeof(C64Sys), true);
    if (!object) return "c64: not enough PSRAM for the machine";
    machine = new (object) C64Sys();
    // alloc_internal preserves the OS reserve and falls back to PSRAM.
    auto *ram = (uint8_t *)allocate(65536, true);
    const uint8_t *basic = solar_os_c64_basic_rom;
    const uint8_t *kernal = solar_os_c64_kernal_rom;
    const uint8_t *chargen = solar_os_c64_chargen_rom;
    if (diagnostic || roms) {
        basic = (uint8_t *)allocate(8192);
        kernal = (uint8_t *)allocate(8192);
        chargen = (uint8_t *)allocate(4096);
    }
    auto *pixels = (uint8_t *)allocate(320 * 200);
    auto *colors = (uint8_t *)allocate(1024);
    if (!ram || !basic || !kernal || !chargen || !pixels || !colors) return "c64: not enough PSRAM for guest memory";
    machine->ram = ram; machine->basic_rom = basic; machine->kernal_rom = kernal; machine->charrom = chargen;
    if (diagnostic) diagnostic_rom((uint8_t *)kernal);
    else if (roms) {
        char directory[160];
        if (app->resolve_path(roms, directory, sizeof(directory))) return "c64: invalid ROM directory";
        if (!load_rom(directory, "basic.rom", (uint8_t *)basic, 8192)) return "c64: need an 8192-byte basic.rom in the ROM directory";
        if (!load_rom(directory, "kernal.rom", (uint8_t *)kernal, 8192)) return "c64: need an 8192-byte kernal.rom in the ROM directory";
        if (!load_rom(directory, "chargen.rom", (uint8_t *)chargen, 4096)) return "c64: need a 4096-byte chargen.rom in the ROM directory";
        unsigned reset = kernal[0x1ffc] | (kernal[0x1ffd] << 8);
        if (reset < 0xe000) return "c64: invalid KERNAL reset vector";
    }
    if (prg && !load_program(prg)) return "c64: invalid or unreadable PRG (load address and data must fit in 64 KiB RAM)";
    machine->vic.init(ram, chargen, pixels, colors);
    machine->reset();
    audio_ring = (int16_t *)allocate(ring_size * sizeof(int16_t)); // audio is optional
    if (app->video_open_ex(width, height, palette, 25, 6)) return "c64: foreground video unavailable";
    exchanged = solar_os_native_foreground_has_exchange(app);
    if (exchanged) {
        size_t length = 0;
        if (app->video_acquire(&canvas, &length) || length != (size_t)width * height)
            return "c64: foreground frame allocation failed";
    } else canvas = (uint8_t *)allocate((size_t)width * height);
    if (!canvas) return "c64: not enough PSRAM for the display";
    if (exchanged && width == 320 && height >= 200) {
        memset(canvas, 0, (size_t)width * height);
        machine->vic.setBitmap(canvas + ((height - 200) / 2) * width);
    }
    if (app->enable_suspend()) return "c64: foreground switching unavailable";
    input.buttons_source = app->buttons_source();
    open_audio();
    report(diagnostic ? "c64: diagnostic 6510 guest running" :
        "c64: PAL guest running; F9 joystick mode; F10 pause; F11 reset; F12 Restore; Ctrl+] exit");
    if (!audio_active) report("c64: audio unavailable; continuing silently");
    uint64_t deadline = app->time_us();
    started_at = deadline;
    unsigned frames = 0;
    bool loaded = !prg;
    while (!app->stop_requested()) {
        if (app->suspend_requested()) {
            close_audio(); input.clear(); input.apply(*machine);
            if (app->suspend()) return app->stop_requested() ? nullptr : "c64: resume failed";
            if (app->stop_requested()) break;
            input.clear(); input.apply(*machine);
            if (!paused) open_audio();
            deadline = app->time_us();
            if (present_frame()) return "c64: resume presentation failed";
        }
        if (poll_input()) return "c64: input failed";
        if (snapshot_requested && paused) {
            snapshot_requested = false;
            save_snapshot();
            deadline = app->time_us();
        }
        if (paused) { app->sleep_ms(5); deadline = app->time_us(); continue; }
        for (unsigned slice = 0; slice < 32 && !app->stop_requested() && !app->suspend_requested(); ++slice) {
            machine->stepRaster();
            if (machine->cpuhalted) return "c64: CPU halted on an unsupported instruction; close and relaunch";
            if (machine->vic.rasterline != 311) continue;
            ++frames;
            guest_frames = frames;
            if (frames % 5 == 0) { machine->cia1.updateTOD(); machine->cia2.updateTOD(); }
            queue_audio(); machine->sid.playAudio();
            // Wait for the standard KERNAL's keyboard-input loop after cold
            // start. Never overwrite a still-initializing BASIC workspace.
            if (!loaded && frames >= 50 && machine->getPC() >= 0xe5cd && machine->getPC() <= 0xe5d4) {
                install_program(); loaded = true;
            }
            if (!loaded && frames == 500) return "c64: PRG autostart timed out waiting for the standard KERNAL input loop";
            if ((frames & 1) == 0 && present_frame()) return "c64: display submission failed";
            deadline += 20000;
            uint64_t now = app->time_us();
            if (deadline > now) app->sleep_ms((deadline - now) / 1000);
            else if (now - deadline > 100000) deadline = now;
            break;
        }
        if (snapshot_requested) {
            // Let the diagnostic guest read the newly applied CIA inputs
            // before capturing, while the host's logical tap is still held.
            snapshot_requested = false;
            save_snapshot();
            deadline = app->time_us();
        }
        solar_os_native_foreground_cooperate(app);
    }
    return nullptr;
}

extern "C" int main(int argc, char **argv)
{
    host = solar_os_native_host_v1();
    if (!host || host->abi_version != 1 || host->struct_size < sizeof(*host) || !host->get_service) return 1;
    app = (const solar_os_native_foreground_api_v1_t *)host->get_service("foreground", 1,
        offsetof(solar_os_native_foreground_api_v1_t, video_dimensions) + sizeof(app->video_dimensions));
    memory = (const solar_os_native_memory_api_v1_t *)host->get_service("memory", 1,
        offsetof(solar_os_native_memory_api_v1_t, alloc_internal));
    if (!app || !memory) { report("c64: compatible foreground and memory services required"); return 1; }
    machine = nullptr; canvas = program = nullptr; audio_ring = nullptr;
    allocation_count = program_size = 0; audio_active = paused = exchanged = false;
    snapshot_path[0] = 0; snapshot_requested = false; guest_frames = 0;
    input = {};
    const char *error = run(argc, argv);
    close_audio();
    if (machine) machine->~C64Sys();
    while (allocation_count) memory->free(allocations[--allocation_count]);
    machine = nullptr; canvas = program = nullptr; audio_ring = nullptr;
    if (error) report(error);
    app->set_exit_code(error ? 1 : 0);
    return error ? 1 : 0;
}
