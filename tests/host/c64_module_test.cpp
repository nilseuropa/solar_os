#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#define main solar_os_c64_main
#include "../../modules/c64/main/c64.cpp"
#undef main

static std::map<void *, size_t> live;
static std::map<std::string, std::vector<uint8_t>> files;
static solar_os_native_foreground_audio_render_v1_t callback;
static std::vector<uint8_t> host_buffers[2];
static unsigned attempts, fail_at, presents, suspend_count, synth_opens, buffer_index;
static unsigned stop_after = 10, input_stage;
static uint64_t clock_us;
static uint64_t audio_next_us;
static uint16_t display_w = 480, display_h = 320;
static int exit_code, video_error, synth_error, input_error, present_error, suspend_error;
static uint32_t output_rate = 16000;
static unsigned snapshots;
static bool stopping, short_read, did_suspend, request_suspend = true, saw_pcm, saw_counter, prg_executed, saw_ready;
static std::string messages;

static void *alloc(size_t n)
{
    if (++attempts == fail_at) return nullptr;
    void *p = malloc(n);
    if (p) live[p] = n;
    return p;
}
static void release(void *p) { if (p == audio_ring) assert(!callback); assert(live.erase(p) == 1); free(p); }
static int write_text(const char *text, size_t n) { messages.append(text, n); messages += '\n'; return 0; }
static uint64_t now() { return clock_us += 10; }
static bool stop() { return stopping; }
static void audio_tick()
{
    if (!callback) return;
    while (clock_us >= audio_next_us) {
        int16_t out[512];
        callback(out, 256, output_rate, nullptr);
        for (auto sample : out) if (sample) saw_pcm = true;
        audio_next_us += 256ULL * 1000000 / output_rate;
    }
}
static void sleep_ms(uint32_t ms) { clock_us += (uint64_t)ms * 1000; audio_tick(); }
static void cooperate() { clock_us += 100; audio_tick(); }
static int dimensions(uint16_t *w, uint16_t *h) { *w = display_w; *h = display_h; return 0; }
static int video(uint16_t w, uint16_t h, const uint16_t *palette, uint16_t fps, uint8_t border)
{
    assert(w == display_w && h == display_h && palette[1] == 0xffff && fps == 25 && border == 6);
    return video_error;
}
static int present(const uint8_t *data, size_t n)
{
    assert(n == (size_t)display_w * display_h);
    for (size_t i = 0; i < n; ++i) assert(data[i] < 16);
    ++presents;
    saw_counter |= machine->ram[0x400] != 0;
    unsigned pc = machine->getPC();
    prg_executed |= machine->ram[0xc000] != 0 &&
        ((pc >= 0x2000 && pc <= 0x2006) || (pc >= 0xa52a && pc <= 0xa530));
    const uint8_t ready[] = {18,5,1,4,25,46};
    for (unsigned i = 0x400; i < 0x7e8 - sizeof(ready); ++i)
        if (!memcmp(machine->ram + i, ready, sizeof(ready))) saw_ready = true;
    if (presents >= stop_after) stopping = true;
    return present_error;
}
static int acquire(uint8_t **p, size_t *n)
{
    for (auto &buffer : host_buffers) buffer.assign((size_t)display_w * display_h, 0xcd);
    buffer_index = 0; *p = host_buffers[0].data(); *n = host_buffers[0].size(); return 0;
}
static int exchange(uint8_t **p)
{
    assert(*p == host_buffers[buffer_index].data());
    int result = present(*p, host_buffers[buffer_index].size());
    buffer_index ^= 1;
    memset(host_buffers[buffer_index].data(), 0xcd, host_buffers[buffer_index].size());
    *p = host_buffers[buffer_index].data(); return result;
}
static int enable_suspend() { return 0; }
static bool suspend_requested() { return request_suspend && presents >= 3 && !did_suspend; }
static int suspend_guest()
{
    assert(!callback);
    did_suspend = true; ++suspend_count;
    uint8_t saved = machine->ram[0x400];
    for (auto row : machine->matrix) assert(row == 0xff);
    assert(machine->joystick[0] == 0xff && machine->joystick[1] == 0xff);
    clock_us += 5000000;
    assert(machine->ram[0x400] == saved);
    return suspend_error;
}
static int read_key(solar_os_native_foreground_key_v1_t *key, bool *available)
{
    *available = false;
    // Exercise pause, resume and reset while the app keeps servicing the host.
    if (input_stage && input_stage <= 4 && presents >= 2) {
        *key = {}; key->key = input_stage == 1 ? SOLAR_OS_KEY_F9 :
            input_stage == 4 ? SOLAR_OS_KEY_F11 : SOLAR_OS_KEY_F10;
        ++input_stage; *available = true;
    }
    return input_error;
}
static int snapshot_key(solar_os_native_foreground_key_v1_t *key, bool *available)
{
    *available = presents >= 2 && input_stage == 0;
    if (*available) { *key = {}; key->key = SOLAR_OS_KEY_F2; ++input_stage; }
    return 0;
}
static int resolve(const char *name, char *out, size_t size)
{
    if (!name) name = "/roms";
    if (strlen(name) >= size) return -2;
    memcpy(out, name, strlen(name) + 1); return 0;
}
static int file_size(const char *path, uint32_t *size)
{
    auto found = files.find(path);
    if (found == files.end()) return -5;
    *size = found->second.size(); return 0;
}
static int read_file(const char *path, uint32_t offset, void *out, size_t n, size_t *actual)
{
    const auto &data = files.at(path);
    assert(n <= 65536 && offset + n <= data.size());
    *actual = short_read ? n - 1 : n;
    memcpy(out, data.data() + offset, *actual); return 0;
}
static int synth_open(solar_os_native_foreground_audio_render_v1_t render, void *, size_t block, uint32_t rate)
{
    assert(!callback && block == 256 && rate == output_rate); ++synth_opens;
    if (synth_error) return synth_error;
    callback = render; audio_next_us = clock_us + 256ULL * 1000000 / output_rate; return 0;
}
static void synth_close() { assert(callback); callback = nullptr; }
static int audio_open(uint32_t *rate, uint8_t *channels) { *rate = output_rate; *channels = 2; return 0; }
static void audio_close() {}
static int video_stats(solar_os_native_foreground_video_stats_v1_t *stats)
{
    *stats = {}; stats->frames = presents; stats->present_us = 1234; return 0;
}
static int replace_file(const char *path, const void *data, size_t n)
{
    if (n == 80) {
        assert(!strcmp(path, "/diagnostic.ppm.state"));
        const auto *bytes = (const uint8_t *)data;
        assert(!memcmp(bytes, "C64S", 4) && bytes[4] == 1 && bytes[8] && bytes[48]);
    } else {
        assert(!strcmp(path, "/diagnostic.ppm"));
        assert(n == 320 * 200 * 3 + 15 && !memcmp(data, "P6\n320 200\n255\n", 15));
        ++snapshots;
    }
    return 0;
}
static uint8_t buttons_source() { return 7; }
static void set_exit(int code) { exit_code = code; }
static solar_os_native_memory_api_v1_t mem;
static solar_os_native_foreground_api_v1_t fg;
static solar_os_native_host_api_v1_t api;
static const void *get_service(const char *name, uint32_t version, uint32_t size)
{
    assert(version == 1);
    if (!strcmp(name, "memory")) return size <= mem.struct_size ? &mem : nullptr;
    if (!strcmp(name, "foreground")) return size <= fg.struct_size ? &fg : nullptr;
    return nullptr;
}
extern "C" const solar_os_native_host_api_v1_t *solar_os_native_host_v1() { return &api; }

static void reset_host()
{
    assert(live.empty() && !callback);
    attempts = fail_at = presents = suspend_count = synth_opens = input_stage = snapshots = 0;
    output_rate = 16000;
    clock_us = 0; stop_after = 10; display_w = 480; display_h = 320;
    exit_code = video_error = synth_error = input_error = present_error = suspend_error = 0;
    stopping = short_read = did_suspend = saw_pcm = saw_counter = prg_executed = saw_ready = false;
    request_suspend = true; messages.clear(); files.clear();
    mem = {}; mem.abi_version = 1; mem.struct_size = sizeof(mem); mem.alloc = alloc; mem.free = release;
    fg = {}; fg.abi_version = 1; fg.struct_size = sizeof(fg);
    fg.time_us = now; fg.sleep_ms = sleep_ms; fg.cooperate = cooperate;
    fg.video_dimensions = dimensions; fg.video_open_ex = video; fg.present = present;
    fg.read_key = read_key; fg.resolve_path = resolve; fg.file_size = file_size; fg.read_file = read_file;
    fg.enable_suspend = enable_suspend; fg.suspend_requested = suspend_requested; fg.suspend = suspend_guest;
    fg.synth_open = synth_open; fg.synth_close = synth_close; fg.buttons_source = buttons_source;
    fg.audio_open = audio_open; fg.audio_close = audio_close;
    fg.video_stats = video_stats; fg.replace_file = replace_file;
    fg.stop_requested = stop; fg.set_exit_code = set_exit;
    api = {1, sizeof(api), "host", "test", write_text, get_service};
}
static int launch(std::initializer_list<const char *> args = {"c64", "--self-test"})
{
    std::vector<char *> argv;
    for (const char *arg : args) argv.push_back(const_cast<char *>(arg));
    int result = solar_os_c64_main(argv.size(), argv.data());
    assert(live.empty() && !callback && result == exit_code);
    return result;
}
static void synthetic_roms()
{
    files["/roms/basic.rom"].assign(8192, 0xea);
    files["/roms/kernal.rom"].assign(8192, 0xea);
    files["/roms/chargen.rom"].assign(4096, 0xff);
    auto &kernal = files["/roms/kernal.rom"];
    diagnostic_rom(kernal.data());
    // Original substitute for the standard KERNAL's input loop, for PRG tests.
    kernal[0] = 0x4c; kernal[1] = 0xcd; kernal[2] = 0xe5;
    kernal[0x5cd] = 0x4c; kernal[0x5ce] = 0xcd; kernal[0x5cf] = 0xe5;
    const uint8_t loop[] = {0xee,0x00,0xc0,0x4c,0x2a,0xa5};
    memcpy(files["/roms/basic.rom"].data() + 0x52a, loop, sizeof(loop));
}

static void test_bus_input_and_chipsets()
{
    SID sound;
    auto &voice = sound.sidVoice[0];
    voice.updVarEnvelopeAD(0); voice.updVarEnvelopeSR(0xf0); voice.updVarControl(0x21);
    for (unsigned i = 0; i < 1000; ++i) voice.updateEnvelope();
    assert(voice.envelope == 1.0f); // sustain is the high nibble, release the low
    voice.updVarControl(0x20);
    for (unsigned i = 0; i < 1000; ++i) voice.updateEnvelope();
    assert(voice.envelope == 0.0f);
    voice.init(); voice.updVarControl(0x21); voice.updVarFrequency(0x7fff);
    unsigned periods = 0; float previous = 0;
    for (unsigned i = 0; i < AUDIO_SAMPLE_RATE; ++i) {
        float value = voice.generateSample();
        if (previous > .5f && value < -.5f) ++periods;
        previous = value;
    }
    unsigned frequency = (uint64_t)0x7fff * 985248 / 16777216;
    assert(periods + 1 >= frequency && periods <= frequency + 1);
    C64Sys m{};
    std::vector<uint8_t> ram(65536), basic(8192, 0xba), kernal(8192), chars(4096, 0xcc), pixels(64000), colors(1024);
    m.ram = ram.data(); m.basic_rom = basic.data(); m.kernal_rom = kernal.data(); m.charrom = chars.data();
    diagnostic_rom(kernal.data()); m.vic.init(ram.data(), chars.data(), pixels.data(), colors.data()); m.reset();
    assert(m.getMem(0xa000) == 0xba);
    m.setMem(0xa000, 0x17); m.setMem(1, 0x30); assert(m.getMem(0xa000) == 0x17);
    m.setMem(1, 0x33); assert(m.getMem(0xd000) == 0xcc);
    m.setMem(1, 0x37); m.setMem(0xd800, 0xff); assert(m.vic.colormap[0] == 15);
    m.setMem(0xdc02, 0xff); m.setMem(0xdc03, 0);
    solar_os_c64_input keys;
    solar_os_native_foreground_key_v1_t key{};
    key.source = 1; key.usage = 4; key.physical_key = 4; key.key = 'a'; keys.key(key);
    key.usage = key.physical_key = 0x1a; key.key = 'w'; keys.key(key); keys.apply(m);
    m.setMem(0xdc00, 0xfd); assert((m.getMem(0xdc01) & 6) == 0); // A + W
    key.action = 1; key.key = 'W'; keys.key(key); keys.apply(m); assert((m.getMem(0xdc01) & 6) == 2);
    m.setMem(0xdc02, 0); m.setMem(0xdc03, 0xff); m.setMem(0xdc01, 0xfb);
    assert((m.getMem(0xdc00) & 2) == 0); // reverse CIA scanning
    key = {}; key.reset = true; keys.key(key); keys.apply(m);
    for (auto row : m.matrix) assert(row == 0xff);
    key = {}; key.key = '@'; key.modifiers = 2; keys.key(key); keys.apply(m);
    assert(!(m.matrix[5] & 0x40) && (m.matrix[1] & 0x80));
    keys.clear();
    key = {}; key.key = '"'; key.modifiers = 2; keys.key(key); keys.apply(m);
    assert(!(m.matrix[7] & 0x08) && !(m.matrix[1] & 0x80));
    keys.clear();
    keys.keyboard_port = 2; key = {}; key.key = SOLAR_OS_KEY_UP; keys.key(key); keys.apply(m);
    assert(m.joystick[1] == 0xfe && m.joystick[0] == 0xff);
    keys.clear(); keys.joystick_source = 9;
    solar_os_native_foreground_axis_v1_t axis{}; axis.source = 9; axis.value = -20000;
    keys.axis(axis); keys.apply(m); assert(m.joystick[1] == 0xfb);
    axis.value = -9000; keys.axis(axis); keys.apply(m); assert(m.joystick[1] == 0xfb);
    axis.value = 0; keys.axis(axis); keys.apply(m); assert(m.joystick[1] == 0xff);
    m.reset();
    bool count = false, tone = false;
    for (unsigned frame = 0; frame < 10; ++frame) {
        for (unsigned line = 0; line < 312; ++line) m.stepRaster();
        count |= ram[0x400] != 0;
        for (unsigned i = 0; i < SID::pcm_frames; ++i) tone |= m.sid.pcm()[i] != 0;
        m.sid.playAudio();
    }
    assert(count && tone && !m.cpuhalted);
    m.cia1.setCommonCIAReg(4, 2); m.cia1.setCommonCIAReg(5, 0);
    m.cia1.setCommonCIAReg(0x0d, 0x81); m.cia1.setCommonCIAReg(0x0e, 0x11);
    m.cia1.checkTimerA(3); assert(m.cia1.latchdc0d & 1);
    m.cia1.setCommonCIAReg(0x0b, 1); m.cia1.setCommonCIAReg(8, 0);
    m.cia1.updateTOD(); assert(m.cia1.latchrundc08.load() == 1);
    // Exercise all legal VIC display modes, fine scroll and sprite clipping.
    for (unsigned mode = 0; mode < 8; ++mode) {
        m.reset();
        for (unsigned i = 0; i < ram.size(); ++i) ram[i] = i * 37;
        m.setMem(1, 0x37);
        m.setMem(0xd011, 0x1f | ((mode & 1) ? 0x20 : 0) | ((mode & 2) ? 0x40 : 0));
        m.setMem(0xd016, 0x0f | ((mode & 4) ? 0x10 : 0));
        m.setMem(0xd015, 0xff); m.setMem(0xd01d, 0xff); m.setMem(0xd017, 0xff);
        for (unsigned i = 0; i < 16; ++i) m.setMem(0xd000 + i, i & 1 ? 50 : 255);
        for (unsigned line = 0; line < 312; ++line) { m.vic.nextRasterline(); m.vic.drawRasterline(); }
    }
    // A doubled multicolor sprite straddling the bottom-right framebuffer edge.
    // Clip each output pixel: callers may revisit the right edge as they expand
    // a two-bit group, and must never advance past the final scanline.
    for (unsigned mode = 0; mode < 4; ++mode) {
        m.reset();
        ram[1024 + 1016] = 0x20;
        memset(ram.data() + 0x800, 0xff, 64);
        m.setMem(0xd000, 87); m.setMem(0xd001, 230); m.setMem(0xd010, 1);
        m.setMem(0xd015, 1); m.setMem(0xd017, 1);
        m.setMem(0xd01d, mode & 1); m.setMem(0xd01c, (mode >> 1) & 1);
        m.setMem(0xd025, 1); m.setMem(0xd026, 1); m.setMem(0xd027, 1);
        for (unsigned line = 0; line < 312; ++line) { m.vic.nextRasterline(); m.vic.drawRasterline(); }
        assert(pixels.back() == 1);
    }
}

static void test_audio_burst_buffering()
{
  for (unsigned speed : {100, 70}) {
    reset_host();
    app = &fg;
    std::vector<int16_t> queue(ring_size, 5000);
    audio_ring = queue.data();
    open_audio();
    bool heard = false;
    unsigned produced = 0, fade = 0;
    for (unsigned block = 0; block < 300; ++block) {
        // A PAL producer publishes whole 20 ms bursts, with up to 3 ms jitter.
        uint64_t time = (uint64_t)block * 16000;
        while ((uint64_t)(produced + 1) * 20000 * 100 / speed + (produced % 3) * 1500 <= time) {
            assert(audio_queue.write.load() - audio_queue.read.load() + SID::pcm_frames <= ring_size);
            audio_queue.write.store(audio_queue.write.load() + SID::pcm_frames);
            ++produced;
        }
        int16_t output[512];
        render_audio(output, 256, 16000, nullptr);
        for (auto sample : output) {
            if (sample) heard = true;
            if (heard && fade++ >= 64) assert(sample == 5000);
        }
    }
    assert(heard && audio_queue.underruns.load() == 0);
    close_audio(); audio_ring = nullptr;
  }
}

int main()
{
    test_bus_input_and_chipsets();
    test_audio_burst_buffering();
    reset_host(); assert(launch() == 0);
    assert(presents == 10 && saw_counter && saw_pcm && suspend_count == 1 && synth_opens == 2);
    reset_host(); mem.alloc_internal = alloc; assert(launch() == 0);
    reset_host(); input_stage = 1; assert(launch() == 0); assert(input_stage == 5);
    reset_host(); output_rate = 48000; assert(launch() == 0 && saw_pcm);
    reset_host(); fg.read_key = snapshot_key;
    assert(launch({"c64", "--self-test", "/diagnostic.ppm"}) == 0 && snapshots == 1);
    reset_host(); synthetic_roms(); fg.read_key = snapshot_key;
    assert(launch({"c64", "--roms", "/roms", "--snapshot", "/diagnostic.ppm"}) == 0 && snapshots == 1);
    reset_host(); fg.video_acquire = acquire; fg.video_exchange = exchange; assert(launch() == 0); assert(saw_counter);
    for (auto dimensions : {std::pair{128,128}, std::pair{320,200}, std::pair{640,480}, std::pair{1024,768}}) {
        reset_host(); display_w = dimensions.first; display_h = dimensions.second; assert(launch() == 0);
    }
    for (unsigned height : {200, 240, 320}) {
        reset_host(); display_w = 320; display_h = height;
        fg.video_acquire = acquire; fg.video_exchange = exchange;
        assert(launch() == 0 && saw_counter && suspend_count == 1);
    }
    for (unsigned failure = 1; failure <= 9; ++failure) {
        reset_host(); fail_at = failure; assert(launch() == (failure == 8 ? 0 : 1));
    }
    reset_host(); video_error = -4; assert(launch() == 1);
    reset_host(); input_error = -1; assert(launch() == 1);
    reset_host(); present_error = -1; assert(launch() == 1);
    reset_host(); suspend_error = -1; assert(launch() == 1);
    reset_host(); synth_error = -6; assert(launch() == 0); assert(!saw_pcm);
    reset_host(); display_w = 32; assert(launch() == 1);
    reset_host(); stop_after = 150; assert(launch({"c64"}) == 0 && saw_ready);
    reset_host(); assert(launch({"c64", "--roms", "/missing"}) == 1);
    reset_host(); synthetic_roms(); short_read = true; assert(launch({"c64", "--roms", "/roms"}) == 1);
    reset_host(); synthetic_roms(); files["/roms/chargen.rom"].resize(4095); assert(launch({"c64", "--roms", "/roms"}) == 1);
    reset_host(); synthetic_roms(); files["/roms/kernal.rom"][0x1ffd] = 0; assert(launch({"c64", "--roms", "/roms"}) == 1);
    for (auto prg : {std::vector<uint8_t>{0}, {0,0,0}, {0xff,0xff,1,2}}) {
        reset_host(); synthetic_roms(); files["/test.prg"] = prg;
        assert(launch({"c64", "--roms", "/roms", "/test.prg"}) == 1);
    }
    for (bool basic : {false, true}) {
        reset_host(); synthetic_roms(); stop_after = 60;
        files["/test.prg"] = basic ? std::vector<uint8_t>{1,8,0,0} :
            std::vector<uint8_t>{0,0x20,0xee,0,0xc0,0x4c,0,0x20};
        assert(launch({"c64", "--roms", "/roms", "/test.prg"}) == 0); assert(prg_executed);
    }
    reset_host(); assert(launch({"c64", "--self-test", "--roms", "/roms"}) == 1);
    if (const char *directory = getenv("SOLAROS_C64_TEST_ROM_DIR")) {
        reset_host(); stop_after = 150;
        for (const char *name : {"basic.rom", "kernal.rom", "chargen.rom"}) {
            std::ifstream stream(std::string(directory) + "/" + name, std::ios::binary);
            assert(stream.good());
            files[std::string("/roms/") + name] = std::vector<uint8_t>(
                std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        }
        // Only inspect the guest while the foreground presenter owns it.
        // A BASIC PRG uses SYS 8192 to run the original counter loop.
        std::vector<uint8_t> prg(0x2000 - 0x0801 + 8, 0);
        const uint8_t loader[] = {1,8,0x0b,8,10,0,0x9e,'8','1','9','2',0,0,0};
        memcpy(prg.data(), loader, sizeof(loader));
        const uint8_t code[] = {0xee,0,0xc0,0x4c,0,0x20};
        memcpy(prg.data() + 2 + 0x2000 - 0x0801, code, sizeof(code));
        files["/test.prg"] = prg;
        assert(launch({"c64", "--roms", "/roms", "/test.prg"}) == 0);
        assert(prg_executed);
        puts("User-supplied ROM boot and BASIC PRG autostart passed on host");
    }
    if (const char *path = getenv("SOLAROS_C64_TEST_PRG")) {
        reset_host(); stop_after = 1500;
        std::ifstream stream(path, std::ios::binary);
        assert(stream.good());
        files["/external.prg"] = std::vector<uint8_t>(
            std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        int result = launch({"c64", "/external.prg"});
        if (result) fprintf(stderr, "%s", messages.c_str());
        assert(result == 0);
        puts("External PRG completed 3000 guest frames under ASan/UBSan");
    }
    reset_host(); fg.struct_size = offsetof(solar_os_native_foreground_api_v1_t, video_dimensions);
    assert(solar_os_c64_main(0, nullptr) == 1 && live.empty());
    puts("C64 core, display, media, input, audio, suspend/resume and cleanup checks passed");
}
