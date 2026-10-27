#undef main
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_abi.h"
#include "machine.h"
#include "roms.h"
#include "vendor/puce6502.h"

extern int apple2_app_entry(int argc, char **argv);
static unsigned allocations, frees, reads, frames, closes;
static int failure, exit_code;
static uint64_t time_us;
static bool key_sent;
static bool stats_sent;
static char last_report[256];
static const char *assets;
static bool booted;
static uint32_t sleep_quantum_us = 1000U;
static uint64_t observed_ticks, last_poll_us, longest_poll_gap_us;
static unsigned asset_frames;
static bool model_cpu_cost;
static unsigned stop_case, audio_writes;
static bool audio_live;
static uint16_t text_foreground, text_background;
static const char *expected_directory_arg, *resolved_directory, *expected_disk;
static bool fail_resolve;
static bool resumable, suspend_pending, suspend_exit;
static unsigned suspends, audio_opens;
static int resume_failure;
static unsigned suspension_case;
static uint64_t suspended_wall, suspend_cycles;
static bool suspend_boot;
static bool basic_boot, basic_ready, basic_done;
static unsigned basic_character;
static const char *const *direct_inputs, *const *direct_outputs;
static unsigned direct_resolves;
static unsigned direct_fail_at = 2;
static int resolve_path(const char *path, char *resolved, size_t size)
{
    if (direct_inputs) {
        assert(direct_resolves < 2 && !strcmp(path, direct_inputs[direct_resolves]));
        if (direct_resolves == direct_fail_at) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        assert(strlen(direct_outputs[direct_resolves]) < size);
        strcpy(resolved, direct_outputs[direct_resolves++]);
        return 0;
    }
    if (expected_directory_arg) assert(!strcmp(path, expected_directory_arg));
    if (fail_resolve || failure == 1) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    const char *result = resolved_directory ? resolved_directory : path;
    assert(result[0] == '/' && strlen(result) < size);
    strcpy(resolved, result); return 0;
}
static FILE *asset_file(const char *path)
{
    char resolved[1024];
    const char *name = strstr(path, "appleII+.rom") ? "rom/appleII+.rom" :
        (strstr(path, "diskII.rom") ? "rom/diskII.rom" : "DOS 3.3.nib");
    assert(snprintf(resolved, sizeof(resolved), "%s/%s", assets, name) < (int)sizeof(resolved));
    return fopen(resolved, "rb");
}
static void *allocate(size_t bytes)
{
    if (failure == 2) return NULL;
    allocations++; return malloc(bytes);
}
static void release(void *data) { assert(!audio_live); frees++; free(data); }
static int report(const char *text, size_t length)
{
    assert(text && length && length < sizeof(last_report));
    memcpy(last_report, text, length); last_report[length] = '\0'; return 0;
}
static uint64_t now(void)
{
    if (model_cpu_cost) {
        /* Model an emulator processing ten guest cycles per microsecond. */
        time_us += (ticks - observed_ticks) / 10U;
        observed_ticks = ticks;
    }
    return time_us;
}
static void sleep_ms(uint32_t ms)
{
    assert(ms);
    time_us += ((ms * 1000U + sleep_quantum_us - 1U) / sleep_quantum_us) * sleep_quantum_us;
}
static void set_exit(int code) { exit_code = code; }
static int file_size(const char *path, uint32_t *size)
{
    assert(!strstr(path, ".rom"));
    if (direct_outputs) assert(!strcmp(path, direct_outputs[0]) ||
        (direct_resolves == 2 && !strcmp(path, direct_outputs[1])));
    if (resolved_directory) {
        if (expected_disk && !strstr(path, ".rom")) assert(!strcmp(path, expected_disk));
        else {
            assert(!strncmp(path, resolved_directory, strlen(resolved_directory)));
            assert(path[strlen(resolved_directory)] == '/');
        }
    }
    if (assets) {
        FILE *file = asset_file(path); assert(file && !fseek(file, 0, SEEK_END));
        *size = (uint32_t)ftell(file); fclose(file); return 0;
    }
    if (failure == 5) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *size = failure == 3 ? 1 : APPLE2_NIB_SIZE;
    return 0;
}
static int read_file(const char *path, uint32_t offset, void *buffer,
    size_t length, size_t *actual)
{
    assert(length <= 65536U); reads++;
    assert(!strstr(path, ".rom"));
    if (assets) {
        FILE *file = asset_file(path); assert(file && !fseek(file, offset, SEEK_SET));
        *actual = fread(buffer, 1, length, file); assert(!ferror(file));
        fclose(file); return 0;
    }
    if (strstr(path, "disk.nib") && failure == 6) return SOLAR_OS_NATIVE_ERROR_FAILED;
    memset(buffer, 0, length); *actual = length;
    if (strstr(path, "disk.nib") && failure == 7 && offset == 65536U) *actual = length - 1U;
    return 0;
}
static void synthetic_rom(void)
{
    if (!assets && !basic_boot) {
        uint8_t *buffer = apple2_machine->rom;
        /* ROM program echoes the keyboard latch to RAM and clears its strobe. */
        static const uint8_t rom[] = {
            0xad, 0x00, 0xc0, 0x8d, 0x00, 0x03,
            0xad, 0x10, 0xc0, 0x4c, 0x00, 0xd0,
        };
        if (failure == 8 || failure == 9 || stop_case == 3) {
            static const uint8_t motor_on[] = {0xa9, 0x00, 0x8d, 0xe9, 0xc0};
            memcpy(buffer, motor_on, sizeof(motor_on));
            memcpy((uint8_t *)buffer + sizeof(motor_on), rom, sizeof(rom));
        } else memcpy(buffer, rom, sizeof(rom));
        ((uint8_t *)buffer)[0x2ffc] = 0;
        ((uint8_t *)buffer)[0x2ffd] = 0xd0;
    }
}
static int video_open(uint16_t width, uint16_t height, const uint16_t *palette, uint16_t fps)
{
    assert(!memcmp(apple2_machine->rom, solar_os_apple2_rom, sizeof(apple2_machine->rom)));
    assert(!memcmp(apple2_machine->sl6, solar_os_apple2_disk_rom, sizeof(apple2_machine->sl6)));
    synthetic_rom();
    assert(width == 280 && height == 192 && palette[15] == 0xffff && fps == 25);
    assert(palette[0] == 0 && palette[1] == 0x984cU);
    assert(palette[APPLE2_TEXT_FOREGROUND] == text_foreground);
    assert(palette[APPLE2_TEXT_BACKGROUND] == text_background);
    return failure == 4 ? SOLAR_OS_NATIVE_ERROR_NO_MEMORY : 0;
}
static int text_colors(uint16_t *foreground, uint16_t *background)
{
    text_foreground = stop_case > 1 ? 0x07e0U : 0x0000U;
    text_background = stop_case > 1 ? 0x0000U : 0xffffU;
    *foreground = text_foreground; *background = text_background; return 0;
}
static int video_open_ex(uint16_t width, uint16_t height, const uint16_t *palette,
    uint16_t fps, uint8_t background_index)
{
    assert(background_index == 0U && palette[background_index] == 0x0000U);
    return video_open(width, height, palette, fps);
}
static int present(const uint8_t *pixels, size_t length)
{
    assert(pixels && length == 280U * 192U);
    if (basic_boot) {
        assert(!apple2_machine->disk[0].mounted && !apple2_machine->disk[1].mounted);
        assert(time_us < 5000000U);
        char screen[24 * 41 + 1]; size_t n = 0;
        for (unsigned row = 0; row < 24; ++row) {
            unsigned offset = (row & 7U) * 128U + (row >> 3) * 40U;
            for (unsigned col = 0; col < 40; ++col) {
                uint8_t ch = apple2_machine->ram[0x400 + offset + col] & 0x7fU;
                screen[n++] = (char)(ch < 32 ? ch + 64 : ch);
            }
            screen[n++] = '\n';
        }
        screen[n] = '\0';
        if (strstr(screen, "\n]")) basic_ready = true;
        if (strstr(screen, "579")) basic_done = true;
        return 0;
    }
    if (stop_case) {
        frames++;
        if (stop_case == 2) assert(apple2_machine->paused);
        if (stop_case == 4) assert(apple2_machine->show_help);
        if (stop_case == 5 && frames >= 3) assert(!apple2_machine->show_help);
        if (suspension_case && frames == 2) {
            suspend_pending = true;
            apple2_machine->PB0 = 0x80;
            apple2_machine->paddle[0] = 0;
            apple2_machine->char_count = 4;
            apple2_machine->disk[0].dirty = true;
        }
        if (suspension_case && suspends && frames == 3) {
            assert(!apple2_machine->PB0 && apple2_machine->paddle[0] == 127);
            assert(!apple2_machine->char_count && apple2_machine->disk[0].dirty);
            assert(ticks - suspend_cycles < 101000U); /* One turbo slice with whole-instruction rounding. */
        }
        return 0;
    }
    if (assets) {
        asset_frames++;
        char screen[24 * 41 + 1]; size_t n = 0;
        for (unsigned row = 0; row < 24; ++row) {
            unsigned offset = (row & 7U) * 128U + (row >> 3) * 40U;
            for (unsigned col = 0; col < 40; ++col) {
                uint8_t ch = apple2_machine->ram[0x400 + offset + col] & 0x7fU;
                screen[n++] = (char)(ch < 32 ? ch + 64 : ch);
            }
            screen[n++] = '\n';
        }
        screen[n] = '\0';
        if (strstr(screen, "DOS VERSION") && strstr(screen, "\n]")) {
            booted = true;
            printf("Module booted DOS 3.3 to BASIC: %llu guest cycles, %.2f simulated seconds (%u us tick, %u frames)\n",
                ticks, (double)time_us / 1000000U, sleep_quantum_us, asset_frames);
            assert(asset_frames < time_us / 40000U / 2U);
            return SOLAR_OS_NATIVE_ERROR_FAILED;
        }
        assert(time_us - suspended_wall < (sleep_quantum_us == 1000U ? 25000000U : 45000000U));
        return 0;
    }
    if (++frames == 4) {
        assert(apple2_machine->ram[0x300] == 'A');
        if (failure == 8) assert(apple2_machine->show_stats);
        if (failure == 9) assert(!apple2_machine->fast_disk);
        return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
    return 0;
}
static int read_key(solar_os_native_foreground_key_v1_t *key, bool *available)
{
    memset(key, 0, sizeof(*key));
    if (basic_boot) {
        const char *command = "PRINT 123+456\r";
        *available = basic_ready && !(apple2_machine->KBD & 0x80U) && command[basic_character];
        if (*available) key->key = (uint8_t)command[basic_character++];
        return 0;
    }
    if (assets) {
        if (suspend_boot && !suspends && time_us > 1000000U) suspend_pending = true;
        if (model_cpu_cost && time_us > 1100000U) {
            assert(apple2_machine->cpu_khz >= 9900U && apple2_machine->cpu_khz < 10200U);
            assert(apple2_machine->guest_khz > 0U &&
                apple2_machine->guest_khz <= apple2_machine->cpu_khz);
        }
        uint64_t gap = time_us - last_poll_us;
        if (gap > longest_poll_gap_us) longest_poll_gap_us = gap;
        last_poll_us = time_us;
        *available = false; return 0;
    }
    /* Change visible RAM independently of the keyboard echo program. */
    apple2_machine->ram[0x401] = (uint8_t)(0x80U | (time_us / 40000U & 0x3fU));
    if (stop_case && !key_sent) {
        *available = true;
        key->key = stop_case == 2 ? 0x8f : (stop_case >= 4 ? 0x91 : 'a');
        key_sent = true;
        return 0;
    }
    if (stop_case == 5 && time_us >= 80000U && !stats_sent) {
        *available = true; key->usage = 0x45; stats_sent = true; return 0;
    }
    if ((failure == 8 || failure == 9) && key_sent && !stats_sent) {
        *available = true; key->key = failure == 8 ? 0x87 : 0x88;
        stats_sent = true; return 0;
    }
    *available = !key_sent; key->key = 'a'; key_sent = true;
    return 0;
}
static int audio_open(uint32_t *rate, uint8_t *channels)
{
    audio_opens++;
    if (stop_case) { *rate = 48000; *channels = 1; audio_live = true; return 0; }
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
}
static int audio_write(const int16_t *samples, size_t count)
{
    assert(audio_live && samples && count); audio_writes++; return 0;
}
static void audio_close(void) { audio_live = false; closes++; }
static bool stop_requested(void) { return basic_done || suspend_exit || (stop_case && time_us - suspended_wall >= 120000U); }
static int enable_suspend(void) { resumable = true; return 0; }
static bool suspend_requested(void) { return suspend_pending; }
static int suspend_vm(void)
{
    assert(resumable && suspend_pending && allocations > frees);
    uint64_t cycles = ticks;
    apple2_machine_t *m = apple2_machine;
    uint8_t memory_byte = m->ram[0x401];
    unsigned before_reads = reads;
    /* Model release/reacquisition by the host, with a long time in the shell. */
    audio_close(); time_us += 30000000U; suspended_wall += 30000000U;
    last_poll_us = time_us;
    suspend_cycles = ticks;
    assert(apple2_machine == m && ticks == cycles && reads == before_reads);
    assert(m->ram[0x401] == memory_byte);
    suspends++; suspend_pending = false;
    if (suspension_case == 4) suspend_exit = true;
    return resume_failure;
}
static void restart(void) { assert(!"module must not restart the device"); }
static const solar_os_native_foreground_api_v1_t vm_api = {
    .abi_version = 1, .struct_size = sizeof(vm_api),
    .time_us = now, .sleep_ms = sleep_ms, .video_open = video_open,
    .present = present, .read_key = read_key, .file_size = file_size,
    .read_file = read_file, .audio_open = audio_open, .audio_close = audio_close,
    .audio_write = audio_write, .set_exit_code = set_exit,
    .stop_requested = stop_requested, .restart = restart,
    .text_colors = text_colors, .video_open_ex = video_open_ex,
    .resolve_path = resolve_path,
    .enable_suspend = enable_suspend, .suspend_requested = suspend_requested,
    .suspend = suspend_vm,
};
static const solar_os_native_memory_api_v1_t memory_api = {
    .abi_version = 1, .struct_size = sizeof(memory_api), .alloc = allocate, .free = release,
};
static const void *get_service(const char *name, uint32_t abi, uint32_t size)
{
    assert(abi == 1);
    if (!strcmp(name, "foreground")) {
        assert(size == offsetof(solar_os_native_foreground_api_v1_t, suspend) + sizeof(vm_api.suspend));
        return &vm_api;
    }
    assert(!strcmp(name, "memory") && size == sizeof(memory_api)); return &memory_api;
}
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void)
{
    static const solar_os_native_host_api_v1_t host = {
        .abi_version = 1, .struct_size = sizeof(host), .write_utf8 = report,
        .get_service = get_service,
    };
    return &host;
}
int main(int argc, char **test_argv)
{
    char *argv[] = {"apple2", "/assets", "disk.nib"};
    for (failure = 1; failure <= 7; ++failure) {
        exit_code = 0;
        assert(apple2_app_entry(3, argv) == 1 && exit_code == 1);
        assert(allocations == frees && apple2_machine == NULL);
        if (failure == 3) assert(strstr(last_report, "got 1, expected 232960") &&
            strstr(last_report, "/assets/disk.nib"));
        if (failure == 4) assert(strstr(last_report, "display setup ran out of memory"));
        if (failure == 5) assert(strstr(last_report, "file not found") &&
            strstr(last_report, "/assets/disk.nib"));
        if (failure == 6) assert(strstr(last_report, "read failed") &&
            strstr(last_report, "/assets/disk.nib"));
        if (failure == 7) assert(strstr(last_report, "short read") &&
            strstr(last_report, "got 65535, expected 65536"));
    }
    failure = 0; reads = 0;
    assert(apple2_app_entry(3, argv) == 1 && exit_code == 1);
    assert(reads == 4 && frames == 4 && key_sent && time_us >= 120000);
    assert(allocations == frees && closes == 6 && apple2_machine == NULL);
    failure = 8; frames = 0; time_us = 0; key_sent = false;
    assert(apple2_app_entry(3, argv) == 1 && ticks > 500000U);
    assert(allocations == frees && closes == 7 && apple2_machine == NULL);
    failure = 9; frames = 0; time_us = 0; key_sent = stats_sent = false;
    sleep_quantum_us = 10000U;
    assert(apple2_app_entry(3, argv) == 1 && ticks > 100000U && ticks < 140000U);
    assert(allocations == frees && closes == 8 && apple2_machine == NULL);
    sleep_quantum_us = 1000U;
    /* Repeat real module entry/cleanup with active audio, pause, disk turbo,
     * help open and help dismissed. Every stop must return successfully. */
    failure = 0;
    for (unsigned run = 0; run < 20; ++run) {
        stop_case = run % 5U + 1U;
        frames = 0; time_us = 0; key_sent = stats_sent = false; audio_writes = 0;
        unsigned before_closes = closes;
        exit_code = 1;
        assert(apple2_app_entry(3, argv) == 0 && exit_code == 0);
        assert(allocations == frees && apple2_machine == NULL && !audio_live);
        assert(closes == before_closes + 1U && frames >= 2U);
        assert(time_us >= 120000U && time_us < 130000U);
        if (stop_case == 1 || stop_case >= 4) assert(audio_writes);
        assert(!strcmp(last_report, "apple2: stopped"));
    }
    stop_case = 0;
    for (suspension_case = 1; suspension_case <= 5; ++suspension_case) {
        stop_case = suspension_case <= 3 ? suspension_case : 1;
        frames = 0; time_us = 0; suspended_wall = 0;
        key_sent = stats_sent = false; suspend_exit = suspend_pending = resumable = false;
        suspends = 0; resume_failure = suspension_case == 5 ? SOLAR_OS_NATIVE_ERROR_NO_MEMORY : 0;
        unsigned before_reads = reads, before_opens = audio_opens;
        int expected = resume_failure ? 1 : 0;
        assert(apple2_app_entry(3, argv) == expected && exit_code == expected);
        assert(suspends == 1 && resumable && !audio_live && allocations == frees);
        assert(reads == before_reads + 4); /* Disk is loaded only once; ROMs are embedded. */
        assert(audio_opens == before_opens + (suspension_case >= 4 ? 1U : 2U));
        if (suspension_case < 4) assert(frames >= 3);
        else assert(frames == 2);
    }
    suspension_case = stop_case = 0; suspend_exit = false;
    resume_failure = 0; suspended_wall = 0;
    /* The legacy form delegates its directory to the host. Disk names remain
     * relative to that resolved directory, including when launched with '.'. */
    static const char *const directories[] = {"apple2", ".", "../apple2", "/apple2", "/sdcard/apple2"};
    for (unsigned i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        expected_directory_arg = directories[i]; resolved_directory = "/sdcard/apple2";
        argv[1] = (char *)directories[i];
        stop_case = 1; frames = 0; time_us = 0; key_sent = stats_sent = false;
        assert(apple2_app_entry(3, argv) == 0 && exit_code == 0);
        assert(allocations == frees && apple2_machine == NULL);
    }
    argv[1] = "apple2"; expected_directory_arg = argv[1];
    argv[2] = "/flash/disks/disk.nib"; expected_disk = argv[2];
    frames = 0; time_us = 0; key_sent = stats_sent = false;
    assert(apple2_app_entry(3, argv) == 0 && exit_code == 0);
    unsigned before_allocations = allocations;
    fail_resolve = true;
    assert(apple2_app_entry(3, argv) == 1 && exit_code == 1);
    assert(allocations == before_allocations && !strcmp(last_report, "apple2: invalid disk directory"));
    fail_resolve = false;
    expected_directory_arg = resolved_directory = expected_disk = NULL;
    /* The short form resolves each disk against the launching shell, including
     * absolute paths and uppercase suffixes. No external ROM file is opened. */
    static const char *const short_inputs[] = {"../disks/disk.NIB", "/flash/disks/data.nib"};
    static const char *const short_outputs[] = {"/sdcard/disks/disk.NIB", "/flash/disks/data.nib"};
    char *short_argv[] = {"apple2", (char *)short_inputs[0], (char *)short_inputs[1], "extra.nib"};
    direct_inputs = short_inputs; direct_outputs = short_outputs;
    for (int count = 2; count <= 3; ++count) {
        direct_resolves = 0; frames = 0; time_us = 0; key_sent = stats_sent = false;
        unsigned before_reads = reads;
        assert(apple2_app_entry(count, short_argv) == 0 && exit_code == 0);
        assert(direct_resolves == (unsigned)(count - 1));
        assert(reads == before_reads + 4U * (unsigned)(count - 1));
        assert(allocations == frees && apple2_machine == NULL);
    }
    before_allocations = allocations;
    assert(apple2_app_entry(4, short_argv) == 1 && exit_code == 1);
    assert(allocations == before_allocations);
    unsigned before_reads = reads;
    for (direct_fail_at = 0; direct_fail_at < 2; ++direct_fail_at) {
        direct_resolves = 0; before_reads = reads;
        assert(apple2_app_entry(3, short_argv) == 1 && exit_code == 1);
        assert(reads == before_reads + 4U * direct_fail_at);
        assert(allocations == frees && apple2_machine == NULL);
        assert(!strcmp(last_report, "apple2: invalid disk path"));
    }
    direct_fail_at = 2;
    direct_inputs = direct_outputs = NULL;
    /* Plain apple2 boots the embedded ROM to BASIC and executes a command. */
    char *bare_argv[] = {"apple2"};
    basic_boot = true; stop_case = 0; time_us = 0;
    before_reads = reads;
    assert(apple2_app_entry(1, bare_argv) == 0 && exit_code == 0);
    assert(basic_ready && basic_done && reads == before_reads);
    assert(allocations == frees && apple2_machine == NULL);
    basic_boot = basic_ready = basic_done = false;
    stop_case = 0; argv[1] = "/assets"; argv[2] = "disk.nib";
    if (argc == 2) {
        assets = test_argv[1]; failure = 0; time_us = 0;
        char *disk_argv[] = {"apple2", "/assets/disk.nib"};
        asset_frames = 0; booted = false;
        assert(apple2_app_entry(2, disk_argv) == 1 && booted && exit_code == 1);
        assert(allocations == frees && apple2_machine == NULL);
        time_us = 0; booted = false;
        asset_frames = 0;
        assert(apple2_app_entry(3, argv) == 1 && booted && exit_code == 1);
        assert(allocations == frees && apple2_machine == NULL);
        assert(time_us < 8000000U);
        for (sleep_quantum_us = 1000U; sleep_quantum_us <= 10000U; sleep_quantum_us *= 10U) {
            time_us = observed_ticks = last_poll_us = longest_poll_gap_us = 0;
            booted = false; model_cpu_cost = true;
            asset_frames = 0;
            assert(apple2_app_entry(3, argv) == 1 && booted && exit_code == 1);
            assert(time_us < (sleep_quantum_us == 1000U ? 25000000U : 45000000U));
            assert(longest_poll_gap_us < 25000U);
            assert(allocations == frees && apple2_machine == NULL);
        }
        sleep_quantum_us = 1000U; suspend_boot = true;
        time_us = observed_ticks = last_poll_us = longest_poll_gap_us = suspended_wall = 0;
        booted = false; asset_frames = suspends = 0;
        assert(apple2_app_entry(3, argv) == 1 && booted && exit_code == 1);
        assert(suspends == 1 && time_us > 30000000U && longest_poll_gap_us < 25000U);
        assert(allocations == frees && apple2_machine == NULL);
    }
    puts("Apple II app entry, chunked assets, input and cleanup tests passed");
    return 0;
}
