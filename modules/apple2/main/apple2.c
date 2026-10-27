#include "solar_os_native_abi.h"
#include "machine.h"
#include "roms.h"
#include "vendor/puce6502.h"
#include <string.h>

static const solar_os_native_host_api_v1_t *host;

SOLAR_OS_NATIVE_APP_MANIFEST = {
    .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
    .lifecycle_abi = SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI,
    .struct_size = sizeof(solar_os_native_app_manifest_v2_t),
    .app_class = SOLAR_OS_NATIVE_APP_CLASS_GUI,
    .flags = SOLAR_OS_NATIVE_APP_RESUMABLE,
    .worker_stack_bytes = 16U * 1024U, .worker_priority = 1U,
    .minimum_internal_bytes = 8U * 1024U,
    .minimum_psram_bytes = sizeof(apple2_machine_t) + 128U * 1024U,
    .tick_interval_ms = 5U,
    .id = "apple2", .summary = "Apple II Plus emulator",
    .file_extensions = ".nib",
};

static void report(const char *message) { host->write_utf8(message, strlen(message)); }

static void report_display_error(int result)
{
    const char *message;
    switch (result) {
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: message = "apple2: display frame format unsupported"; break;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: message = "apple2: display setup ran out of memory"; break;
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: message = "apple2: display target not found"; break;
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: message = "apple2: display rejected frame configuration"; break;
    case SOLAR_OS_NATIVE_ERROR_INVALID_STATE: message = "apple2: display not ready or busy"; break;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: message = "apple2: display setup timed out"; break;
    default: message = "apple2: display setup failed"; break;
    }
    report(message);
}

static void append_text(char message[160], const char *text)
{
    size_t used = strlen(message), count = strlen(text);
    if (count > 159U - used) count = 159U - used;
    memcpy(message + used, text, count);
    message[used + count] = '\0';
}

static void append_number(char message[160], uint32_t value)
{
    char digits[11];
    unsigned count = 0;
    do { digits[count++] = (char)('0' + value % 10U); value /= 10U; } while (value);
    char number[11];
    for (unsigned i = 0; i < count; ++i) number[i] = digits[count - i - 1U];
    number[count] = '\0';
    append_text(message, number);
}

static void report_file_error(const char *path, const char *operation, int result)
{
    char message[160] = "apple2: ";
    append_text(message, operation);
    append_text(message, result == SOLAR_OS_NATIVE_ERROR_NOT_FOUND ?
        " (file not found): " : " (file access error): ");
    append_text(message, path);
    report(message);
}

static void report_size_error(const char *path, const char *operation,
    uint32_t actual, uint32_t expected)
{
    char message[160] = "apple2: ";
    append_text(message, operation);
    append_text(message, ": got "); append_number(message, actual);
    append_text(message, ", expected "); append_number(message, expected);
    append_text(message, " bytes: "); append_text(message, path);
    report(message);
}

static bool asset_path(char path[160], const char *directory, const char *name)
{
    if (!directory || directory[0] != '/' || !name) return false;
    size_t base = strlen(directory), tail = strlen(name);
    if (name[0] == '/') {
        if (tail >= 160U) return false;
        memcpy(path, name, tail + 1U);
        return true;
    }
    bool slash = base && directory[base - 1U] != '/';
    if (base + slash + tail >= 160U) return false;
    memcpy(path, directory, base);
    if (slash) path[base++] = '/';
    memcpy(path + base, name, tail + 1U);
    return true;
}

static bool nib_path(const char *path)
{
    size_t length = strlen(path);
    if (length < 4U) return false;
    const char *suffix = path + length - 4U;
    return suffix[0] == '.' && (suffix[1] == 'n' || suffix[1] == 'N') &&
        (suffix[2] == 'i' || suffix[2] == 'I') && (suffix[3] == 'b' || suffix[3] == 'B');
}

static int load_exact(const char *path, void *buffer, size_t length)
{
    uint32_t size = 0;
    int result = apple2_app->file_size(path, &size);
    if (result) { report_file_error(path, "file lookup failed", result); return result; }
    if (size != length) {
        report_size_error(path, "wrong file size", size, (uint32_t)length);
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    for (size_t offset = 0; offset < length;) {
        size_t count = length - offset, actual = 0;
        if (count > 65536U) count = 65536U;
        result = apple2_app->read_file(path, (uint32_t)offset, (uint8_t *)buffer + offset, count, &actual);
        if (result) { report_file_error(path, "read failed", result); return result; }
        if (actual != count) {
            report_size_error(path, "short read", (uint32_t)actual, (uint32_t)count);
            return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        }
        offset += actual;
    }
    return SOLAR_OS_NATIVE_OK;
}

static int save_drive(unsigned index)
{
    apple2_drive_t *drive = &apple2_machine->disk[index];
    if (!drive->mounted || !drive->dirty) return SOLAR_OS_NATIVE_OK;
    int result = apple2_app->replace_file(drive->filename, drive->data, sizeof(drive->data));
    if (!result) drive->dirty = false;
    report(result ? "apple2: disk save failed" : "apple2: disk saved");
    return result;
}

static void handle_key(const solar_os_native_foreground_key_v1_t *key)
{
    apple2_machine_t *m = apple2_machine;
    if (key->reset) {
        m->PB0 = m->PB1 = m->PB2 = 0;
        m->paddle[0] = m->paddle[1] = 127;
        m->char_count = 0;
        return;
    }
    m->PB0 = (key->modifiers & 0x11U) ? 0x80 : 0;
    m->PB1 = (key->modifiers & 0x44U) ? 0x80 : 0;
    m->PB2 = (key->modifiers & 0x22U) ? 0x80 : 0;
    bool release = key->action == SOLAR_OS_NATIVE_FOREGROUND_KEY_RELEASE;
    /* USB HID keypad 1/3/5/2 control paddles without sending guest characters. */
    if (key->usage == 0x59 || key->usage == 0x5b) {
        m->paddle[0] = release ? 127 : (key->usage == 0x59 ? 0 : 255); return;
    }
    if (key->usage == 0x5d || key->usage == 0x5a) {
        m->paddle[1] = release ? 127 : (key->usage == 0x5d ? 0 : 255); return;
    }
    if (release) return;
    uint8_t ch = key->key;
    if (ch == 0x86 || key->usage == 0x3a) {
        (void)save_drive((key->modifiers & 0x44U) ? 1U : 0U); return;
    }
    if (ch == 0x87 || key->usage == 0x3b) { m->show_stats = !m->show_stats; return; }
    if (ch == 0x88 || key->usage == 0x3c) { m->fast_disk = !m->fast_disk; return; }
    if (ch == 0x89 || key->usage == 0x3d) { m->muted = !m->muted; return; }
    if (ch == 0x8f || key->usage == 0x43) { m->paused = !m->paused; return; }
    if (ch == 0x90 || key->usage == 0x44) { puce6502RST(); return; }
    if (ch == 0x91 || key->usage == 0x45) { m->show_help = !m->show_help; return; }
    switch (ch) {
    case 0x80: ch = 11; break;
    case 0x81: ch = 10; break;
    case 0x82: ch = 8; break;
    case 0x83: ch = 21; break;
    case 127: ch = 8; break;
    case '\n': ch = '\r'; break;
    default: break;
    }
    if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
    if ((key->modifiers & 0x11U) && ch >= '@' && ch <= '_') ch &= 31U;
    if (ch && ch < 0x80 && m->char_count < sizeof(m->chars)) {
        m->chars[(m->char_read + m->char_count) % sizeof(m->chars)] = ch;
        m->char_count++;
    }
}

static void poll_keyboard(void)
{
    solar_os_native_foreground_key_v1_t key;
    bool available;
    /* Bound work per slice even if another task continually injects keys. */
    for (unsigned i = 0; i < 64U; ++i) {
        if (apple2_app->read_key(&key, &available) || !available) break;
        handle_key(&key);
    }
    apple2_machine_t *m = apple2_machine;
    if (!(m->KBD & 0x80U) && m->char_count) {
        m->KBD = m->chars[m->char_read] | 0x80U;
        m->char_read = (uint8_t)((m->char_read + 1U) % sizeof(m->chars));
        m->char_count--;
    }
}

static void open_audio(apple2_machine_t *m)
{
    m->audio_active = false;
    if (!apple2_app->audio_open(&m->sample_rate, &m->channels)) {
        if (m->sample_rate > APPLE2_CLOCK_HZ || !m->sample_rate || !m->channels || m->channels > 2U)
            apple2_app->audio_close();
        else {
            m->sample_step = APPLE2_CLOCK_HZ / m->sample_rate;
            m->sample_remainder = APPLE2_CLOCK_HZ % m->sample_rate;
            m->audio_active = true;
        }
    }
    m->next_sample = ticks;
    m->pcm_frames = m->sample_fraction = 0;
    m->audio_error = 0;
    m->audio_suspended = false;
    m->speaker_input = m->speaker ? 6000 : 0;
    m->speaker_filtered = 0;
}

int main(int argc, char **argv)
{
    host = solar_os_native_host_v1();
    if (!host || host->abi_version != 1U || host->struct_size < sizeof(*host) || !host->get_service) return 1;
    apple2_app = host->get_service(SOLAR_OS_NATIVE_FOREGROUND_SERVICE, 1U,
        offsetof(solar_os_native_foreground_api_v1_t, suspend) + sizeof(apple2_app->suspend));
    const solar_os_native_memory_api_v1_t *memory = host->get_service(
        SOLAR_OS_NATIVE_MEMORY_SERVICE, 1U, sizeof(*memory));
    if (!apple2_app || !memory) { report("apple2: requires the foreground app host on compatible firmware"); return 1; }
    bool disk_arguments = argc > 1 && nib_path(argv[1]);
    if (argc < 1 || argc > (disk_arguments ? 3 : 4)) {
        report("apple2: apple2 [drive1.nib [drive2.nib]] or apple2 <disk-directory> [drive1.nib] [drive2.nib]");
        apple2_app->set_exit_code(1); return 1;
    }
    char directory[160];
    if (argc > 1 && !disk_arguments) {
        int path_result = apple2_app->resolve_path(argv[1], directory, sizeof(directory));
        if (path_result) {
            report("apple2: invalid disk directory");
            apple2_app->set_exit_code(1); return 1;
        }
    }
    apple2_machine_t *m = memory->alloc(sizeof(*m));
    if (!m) { report("apple2: insufficient PSRAM"); apple2_app->set_exit_code(1); return 1; }
    apple2_machine_init(m);
    memcpy(m->rom, solar_os_apple2_rom, sizeof(m->rom));
    memcpy(m->sl6, solar_os_apple2_disk_rom, sizeof(m->sl6));
    int result = SOLAR_OS_NATIVE_OK;
    int first_disk = disk_arguments ? 1 : 2;
    for (int i = first_disk; i < argc; ++i) {
        apple2_drive_t *drive = &m->disk[i - first_disk];
        result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if (disk_arguments) {
            result = apple2_app->resolve_path(argv[i], drive->filename, sizeof(drive->filename));
        } else if (asset_path(drive->filename, directory, argv[i])) {
            result = SOLAR_OS_NATIVE_OK;
        }
        if (result) {
            report("apple2: invalid disk path"); goto done;
        }
        result = load_exact(drive->filename, drive->data, sizeof(drive->data));
        if (result) goto done;
        drive->mounted = true;
    }
    uint16_t palette[256];
    apple2_palette(palette);
    result = apple2_app->text_colors(&palette[APPLE2_TEXT_FOREGROUND],
        &palette[APPLE2_TEXT_BACKGROUND]);
    if (result) { report("apple2: display text colors unavailable"); goto done; }
    result = apple2_app->video_open_ex(APPLE2_WIDTH, APPLE2_HEIGHT, palette, 25U,
        0U); /* Literal black for the aspect-ratio bars. */
    if (result) { report_display_error(result); goto done; }
    open_audio(m);
    result = apple2_app->enable_suspend();
    if (result) { report("apple2: foreground switching unavailable"); goto done; }
    puce6502RST();
    m->next_sample = ticks;
    uint64_t wall = apple2_app->time_us(), epoch_cycles = ticks;
    uint64_t next_frame = wall;
    uint64_t stats_wall = wall, stats_cycles = ticks;
    uint64_t cpu_us = 0, video_us = 0;
    bool was_paused = false;
    report("apple2: F1 save; F2 status; F3 fast disk; F4 mute; F10 pause; F11 reset; F12 help; Ctrl+Alt+Del exit");
    for (;;) {
        if (apple2_app->stop_requested()) {
            result = SOLAR_OS_NATIVE_OK;
            report("apple2: stopped");
            break;
        }
        if (apple2_app->suspend_requested()) {
            m->audio_active = false;
            result = apple2_app->suspend();
            if (apple2_app->stop_requested()) {
                result = SOLAR_OS_NATIVE_OK;
                report("apple2: stopped");
                break;
            }
            if (result) { report("apple2: display resume failed"); break; }
            open_audio(m);
            m->PB0 = m->PB1 = m->PB2 = 0;
            m->char_read = m->char_count = 0;
            m->paddle[0] = m->paddle[1] = 127;
            m->video_valid = false;
            wall = apple2_app->time_us(); epoch_cycles = ticks;
            next_frame = stats_wall = wall; stats_cycles = ticks;
            cpu_us = video_us = 0;
            was_paused = m->paused;
        }
        poll_keyboard();
        uint64_t now = apple2_app->time_us();
        if (m->paused || was_paused) {
            wall = now; epoch_cycles = ticks; m->next_sample = ticks;
            m->pcm_frames = 0; was_paused = m->paused;
        }
        if (!m->paused) {
            if (apple2_disk_accelerated()) {
                /* Run short CPU batches for up to 4 ms before yielding. Bound
                 * cycles too, so coarse clocks cannot delay keyboard polling. */
                unsigned batches = 0;
                do {
                    puce6502Exec(5000U);
                } while (apple2_disk_accelerated() && ++batches < 20U &&
                    apple2_app->time_us() - now < 4000U);
                wall = apple2_app->time_us(); epoch_cycles = ticks;
            } else {
                uint64_t target = epoch_cycles + ((now - wall) * APPLE2_CLOCK_HZ) / 1000000U;
                /* Limit catch-up work; keep input/driver tasks schedulable. */
                if (target > ticks + APPLE2_CLOCK_HZ / 10U) {
                    wall = now; epoch_cycles = ticks; target = ticks;
                }
                if (target > ticks) {
                    uint64_t budget = target - ticks;
                    if (budget > APPLE2_CLOCK_HZ / 50U) budget = APPLE2_CLOCK_HZ / 50U;
                    puce6502Exec(budget);
                }
            }
            cpu_us += apple2_app->time_us() - now;
            apple2_audio_until(ticks);
        }
        if (m->audio_error) {
            apple2_app->audio_close(); m->audio_active = false; m->audio_error = 0;
            report("apple2: audio stopped after output failure");
        }
        now = apple2_app->time_us();
        if (now >= next_frame) {
            if (apple2_video_refresh(ticks, now)) {
                result = apple2_app->present(m->pixels, sizeof(m->pixels));
                if (result) { report("apple2: frame presentation failed"); break; }
            }
            video_us += apple2_app->time_us() - now;
            next_frame = now + 40000U;
        }
        now = apple2_app->time_us();
        if (now - stats_wall >= 1000000U) {
            uint64_t cycles = ticks - stats_cycles, duration = now - stats_wall;
            m->guest_khz = (uint32_t)(cycles * 1000U / duration);
            m->cpu_khz = cpu_us ? (uint32_t)(cycles * 1000U / cpu_us) : 0U;
            uint64_t percent = video_us * 100U / duration;
            m->video_percent = (uint8_t)(percent > 100U ? 100U : percent);
            stats_wall = now; stats_cycles = ticks;
            cpu_us = video_us = 0;
        }
        if (!m->paused && apple2_disk_accelerated())
            solar_os_native_foreground_cooperate(apple2_app);
        else
            apple2_app->sleep_ms(1U);
    }
done:
    apple2_app->audio_close();
    memory->free(m);
    apple2_machine = NULL;
    apple2_app->set_exit_code(result ? 1 : 0);
    return result ? 1 : 0;
}
