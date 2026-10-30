#include "solar_os_app_registry.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "solar_os_config.h"
#include "solar_os_task.h"
#include "solar_os_app_file_types.h"
#include "solar_os_storage.h"
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
#include "solar_os_native_foreground.h"
#include "solar_os_module_packages.h"
#include "solar_os_memory.h"
#include "esp_timer.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PLAYGROUND
#include "solar_os_playground.h"
#endif
#if SOLAR_OS_PACKAGE_APP_APLAY || SOLAR_OS_PACKAGE_APP_ARECORD
#include "solar_os_audio_apps.h"
#endif
#if SOLAR_OS_PACKAGE_APP_CHAT
#include "solar_os_chat_app.h"
#include "solar_os_messages_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_CONTACTS
#include "solar_os_contacts_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_AGENT
#include "solar_os_agent_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_CURL
#include "solar_os_curl.h"
#endif
#if SOLAR_OS_PACKAGE_APP_TELNET
#include "solar_os_telnet.h"
#endif
#if SOLAR_OS_PACKAGE_APP_SCP
#include "solar_os_scp_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_SFTPSYNC
#include "solar_os_sftpsync_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_SFTP
#include "solar_os_sftp_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_SSH
#include "solar_os_ssh_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_WEB
#include "solar_os_web.h"
#endif
#if SOLAR_OS_PACKAGE_APP_WEBRADIO
#include "solar_os_webradio.h"
#endif
#if SOLAR_OS_PACKAGE_APP_RTSP
#include "solar_os_rtsp_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYER
#include "solar_os_player.h"
#endif
#if SOLAR_OS_PACKAGE_APP_RECORDER
#include "solar_os_recorder.h"
#endif
#if SOLAR_OS_PACKAGE_APP_CLOCK
#include "solar_os_clock.h"
#endif
#if SOLAR_OS_PACKAGE_APP_DLP
#include "solar_os_dlp_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_SYNTH
#include "solar_os_synth_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_FUNCGEN
#include "solar_os_funcgen.h"
#endif
#if SOLAR_OS_PACKAGE_APP_CALC
#include "solar_os_calc.h"
#endif
#if SOLAR_OS_PACKAGE_APP_COM
#include "solar_os_com.h"
#endif
#if SOLAR_OS_PACKAGE_APP_EDIT
#include "solar_os_edit.h"
#endif
#if SOLAR_OS_PACKAGE_APP_DOCS
#include "solar_os_docs_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_EMAIL
#include "solar_os_email_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_FILES
#include "solar_os_files.h"
#endif
#if SOLAR_OS_PACKAGE_APP_LAUNCHER
#include "solar_os_launcher.h"
#endif
#if SOLAR_OS_PACKAGE_APP_FTP
#include "solar_os_ftp_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_FLASH
#include "solar_os_flash_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_IO
#include "solar_os_io.h"
#endif
#if SOLAR_OS_PACKAGE_APP_INBOX
#include "solar_os_inbox_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_LESS
#include "solar_os_less.h"
#endif
#if SOLAR_OS_PACKAGE_APP_LTOP
#include "solar_os_ltop.h"
#endif
#if SOLAR_OS_PACKAGE_APP_NOTES
#include "solar_os_notes.h"
#endif
#if SOLAR_OS_PACKAGE_APP_PLOT
#include "solar_os_plot.h"
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
#include "solar_os_playground_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_ZOO
#include "solar_os_zoo_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_LOGIC
#include "solar_os_logic_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_RFSCAN
#include "solar_os_rfscan.h"
#endif
#if SOLAR_OS_PACKAGE_APP_READER
#include "solar_os_reader.h"
#endif
#if SOLAR_OS_PACKAGE_APP_WRITER
#include "solar_os_writer.h"
#endif
#if SOLAR_OS_PACKAGE_APP_SHEET
#include "solar_os_sheet.h"
#endif
#if SOLAR_OS_PACKAGE_APP_INVADERS
#include "solar_os_invaders.h"
#endif
#if SOLAR_OS_PACKAGE_APP_PYTHON
#include "solar_os_python.h"
#endif
#if SOLAR_OS_PACKAGE_APP_LUA
#include "solar_os_lua.h"
#endif
#if SOLAR_OS_PACKAGE_APP_VIEW
#include "solar_os_view.h"
#endif
#if SOLAR_OS_PACKAGE_APP_VPLAY
#include "solar_os_vplay.h"
#endif
#if SOLAR_OS_PACKAGE_APP_SKETCH
#include "solar_os_sketch.h"
#endif

#define APP_ENTRY(app_name, app_summary, app_ptr, app_caps, app_usage, app_min, app_max) \
    {.name = app_name, .summary = app_summary, .app = app_ptr, .capabilities = app_caps, \
     .usage = app_usage, .min_argc = app_min, .max_argc = app_max}
#define APP_FILE_ENTRY(app_name, app_summary, app_ptr, app_caps, app_usage, app_min, app_max, app_extensions) \
    {.name = app_name, .summary = app_summary, .app = app_ptr, .capabilities = app_caps, \
     .usage = app_usage, .file_extensions = app_extensions, \
     .min_argc = app_min, .max_argc = app_max}

static const solar_os_app_registry_entry_t registered_apps[] = {
#if SOLAR_OS_PACKAGE_APP_DLP
    APP_ENTRY("dlp", "DLP display receiver", &solar_os_dlp_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "dlp stream=<byte-stream>", 2, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_APLAY
    APP_ENTRY("aplay", "play WAV/MP3 audio", &solar_os_aplay_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "aplay [-v volume] <file.wav|file.mp3>", 2, 4),
#endif
#if SOLAR_OS_PACKAGE_APP_ARECORD
    APP_ENTRY("arecord", "record WAV audio", &solar_os_arecord_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "arecord [-d seconds] [-i capture-stream] <file.wav>", 2, 6),
#endif
#if SOLAR_OS_PACKAGE_APP_RECORDER
    APP_ENTRY("recorder", "interactive WAV recorder", &solar_os_recorder_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "recorder [--tui] [file.wav]", 1, 3),
#endif
#if SOLAR_OS_PACKAGE_APP_CHAT
    APP_ENTRY("chat", "provider-neutral conversation client", &solar_os_chat_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "chat [instance|class|conversation-id]", 1, 2),
    APP_ENTRY("messages", "manage messaging instances, connections, and history", &solar_os_messages_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "messages", 1, 1),
#endif
#if SOLAR_OS_PACKAGE_APP_CONTACTS
    APP_ENTRY("contacts", "provider-neutral address book", &solar_os_contacts_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "contacts", 1, 1),
#endif
#if SOLAR_OS_PACKAGE_APP_AGENT
    APP_ENTRY("agent", "native LLM agent", &solar_os_agent_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "agent [--tts] [new|resume <id>|ask <prompt...>] | agent script <file> [args...]", 1, 0),
#endif
#if SOLAR_OS_PACKAGE_APP_CURL
    APP_ENTRY("curl", "HTTP client", &solar_os_curl_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "curl [-L] [-o file] <URL>", 2, 0),
#endif
#if SOLAR_OS_PACKAGE_APP_TELNET
    APP_ENTRY("telnet", "Telnet client", &solar_os_telnet_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "telnet [-r] <host> [port]", 2, 4),
#endif
#if SOLAR_OS_PACKAGE_APP_SCP
    APP_ENTRY("scp", "SCP file copy", &solar_os_scp_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "scp [-P port] <source> <destination>", 3, 5),
#endif
#if SOLAR_OS_PACKAGE_APP_SFTPSYNC
    APP_ENTRY("sftpsync", "synchronize files over SSH", &solar_os_sftpsync_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "sftpsync [-arn] [-P port] <source> <destination>", 3, 8),
#endif
#if SOLAR_OS_PACKAGE_APP_SFTP
    APP_ENTRY("sftp", "two-pane SFTP file manager", &solar_os_sftp_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "sftp [[user@]HOST[:PATH] [PORT] [--user USER] [--password PASSWORD] [--remote PATH] [--local PATH]]", 1, 11),
#endif
#if SOLAR_OS_PACKAGE_APP_SSH
    APP_ENTRY("ssh", "SSH client", &solar_os_ssh_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "ssh [user@]host [port]", 2, 3),
#endif
#if SOLAR_OS_PACKAGE_APP_WEB
    APP_ENTRY("web", "simple web browser", &solar_os_web_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "web <URL>", 2, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_WEBRADIO
    APP_ENTRY("webradio", "streaming internet radio", &solar_os_webradio_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "webradio [--tui] [URL] | webradio [--tui] <list | add NAME URL | remove NAME | reset>", 1, 5),
#endif
#if SOLAR_OS_PACKAGE_APP_RTSP
    APP_ENTRY("rtsp", "RTSP JPEG/L16 viewer", &solar_os_rtsp_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "rtsp [--audio-only] [--stats] <[rtsp://][user[:pass]@]host[:port][/path]>", 2, 4),
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYER
    APP_FILE_ENTRY("player", "playlist audio player", &solar_os_player_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "player [--tui] [file.wav|file.mp3]", 1, 3, ".wav .mp3"),
#endif
#if SOLAR_OS_PACKAGE_APP_CLOCK
    APP_ENTRY("clock", "clock, countdown alarm, stopwatch", &solar_os_clock_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "clock [-s | -a MM:SS]", 1, 3),
#endif
#if SOLAR_OS_PACKAGE_APP_SYNTH
    APP_ENTRY("synth", "polyphonic synthesizer and sound designer", &solar_os_synth_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "synth [--headless]", 1, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_FUNCGEN
    APP_ENTRY("funcgen", "audio function generator", &solar_os_funcgen_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "funcgen [--tui]", 1, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_CALC
    APP_ENTRY("calc", "scientific calculator and function plotter", &solar_os_calc_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "calc [--tui | -e expression]", 1, 0),
#endif
#if SOLAR_OS_PACKAGE_APP_COM
    APP_ENTRY("com", "serial terminal", &solar_os_com_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "com [--autobaud] [--hex] [port]", 1, 4),
#endif
#if SOLAR_OS_PACKAGE_APP_EDIT
    APP_ENTRY("edit", "text editor", &solar_os_edit_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "edit <file>", 2, 2),
    APP_ENTRY("hexedit", "two-pane hex editor", &solar_os_hexedit_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "hexedit <file>", 2, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_DOCS
    APP_ENTRY("help", "browse the SolarOS manual", &solar_os_docs_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "help [TOPIC|status|update|reset]", 1, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_EMAIL
    APP_ENTRY("email", "IMAP email client", &solar_os_email_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "email", 1, 1),
#endif
#if SOLAR_OS_PACKAGE_APP_FILES
    APP_ENTRY("files", "two-pane file manager and launcher", &solar_os_files_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "files [--launcher] [path]", 1, 3),
#endif
#if SOLAR_OS_PACKAGE_APP_LAUNCHER
    APP_ENTRY("launcher", "configurable graphical launcher", &solar_os_launcher_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "launcher [config.json]", 1, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_FTP
    APP_ENTRY("ftp", "two-pane FTP file manager", &solar_os_ftp_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "ftp [HOST [PORT] [--user USER --password PASSWORD] [--remote PATH] [--local PATH]]", 1, 11),
#endif
#if SOLAR_OS_PACKAGE_APP_FLASH
    APP_ENTRY("flash", "download and flash SolarOS onto another ESP board", &solar_os_flash_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "flash [refresh | list | download BOARD FLAVOR [VERSION] | BOARD FLAVOR [version=VERSION] [port=uart0] [boot=PIN] [reset=PIN] [baud=RATE]]", 1, 8),
#endif
#if SOLAR_OS_PACKAGE_APP_IO
    APP_ENTRY("io", "expansion pin and bus manager", &solar_os_io_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "io", 1, 1),
#endif
#if SOLAR_OS_PACKAGE_APP_INBOX
    APP_ENTRY("inbox", "universal incoming-message browser", &solar_os_inbox_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "inbox", 1, 1),
#endif
#if SOLAR_OS_PACKAGE_APP_LESS
    APP_ENTRY("less", "text file pager", &solar_os_less_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "less <file>", 2, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_LTOP
    APP_ENTRY("ltop", "live task and per-core CPU monitor", &solar_os_ltop_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "ltop", 1, 1),
#endif
#if SOLAR_OS_PACKAGE_APP_NOTES
    APP_ENTRY("notes", "Markdown checklist notes", &solar_os_notes_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "notes [file.md]", 1, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_PLOT
    APP_ENTRY("plot", "plot DAQ CSV files or scalar streams", &solar_os_plot_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "plot <stream...> [--rate ms] | plot -f <file.csv> [column...]", 2, 0),
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
    APP_ENTRY("playground", "browse and run community scripts", &solar_os_playground_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "playground [search|install|run|delete|refresh|reload|source|storage] ...", 1, 4),
#endif
#if SOLAR_OS_PACKAGE_APP_ZOO
    APP_ENTRY("zoo", "browse and install model bundles", &solar_os_zoo_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "zoo [refresh|source [URL|reset]|storage [sd|flash]|install ID VERSION]", 1, 4),
#endif
#if SOLAR_OS_PACKAGE_APP_LOGIC
    APP_ENTRY("logic", "logic analyzer waveform viewer", &solar_os_logic_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "logic <pin[,pin...]> [rate] [samples] [trigger=pin]", 2, 5),
#endif
#if SOLAR_OS_PACKAGE_APP_RFSCAN
    APP_ENTRY("rfscan", "radio energy graph and swept waterfall", &solar_os_rfscan_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "rfscan [radio-name]", 1, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_READER
    APP_FILE_ENTRY("reader", "graphics text/Markdown/EPUB reader", &solar_os_reader_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "reader [--pager] <file.txt|file.md|file.epub|man:topic>", 2, 3, ".txt .text .md .markdown .epub"),
#endif
#if SOLAR_OS_PACKAGE_APP_WRITER
    APP_FILE_ENTRY("writer", "hybrid WYSIWYG Markdown editor", &solar_os_writer_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "writer [file.md]", 1, 2, ".md .markdown"),
#endif
#if SOLAR_OS_PACKAGE_APP_SHEET
    APP_FILE_ENTRY("sheet", "CSV sheet viewer", &solar_os_sheet_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "sheet <file.csv>", 2, 2, ".csv"),
#endif
#if SOLAR_OS_PACKAGE_APP_INVADERS
    APP_ENTRY("invaders", "arcade shooter", &solar_os_invaders_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "invaders", 1, 1),
#endif
#if SOLAR_OS_PACKAGE_APP_PYTHON
    APP_FILE_ENTRY("python", "MicroPython runtime", &solar_os_python_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "python [script.py [args...]]", 1, 0, ".py .pyw .mpy"),
#endif
#if SOLAR_OS_PACKAGE_APP_LUA
    APP_FILE_ENTRY("lua", "Lua runtime", &solar_os_lua_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "lua [script.lua [args...]]", 1, 0, ".lua"),
#endif
#if SOLAR_OS_PACKAGE_APP_VIEW
    APP_FILE_ENTRY("view", "image viewer", &solar_os_view_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "view [-fit|-actual] <image>", 2, 3, ".png .jpg .jpeg .gif .webp .bmp .pnm .pbm .pgm .ppm"),
#endif
#if SOLAR_OS_PACKAGE_APP_VPLAY
    APP_FILE_ENTRY("vplay", "MPEG-1 media player", &solar_os_vplay_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "vplay [-fit|-actual] <file.mpg>", 2, 3, ".mpg .mpeg"),
#endif
#if SOLAR_OS_PACKAGE_APP_SKETCH
    APP_FILE_ENTRY("sketch", "pointer-driven paint application", &solar_os_sketch_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "sketch [file.png]", 1, 2, ".png"),
#endif
    {0},
};

#undef APP_ENTRY
#undef APP_FILE_ENTRY

#define REGISTERED_APP_STORAGE_COUNT (sizeof(registered_apps) / sizeof(registered_apps[0]))

static const size_t registered_app_count = REGISTERED_APP_STORAGE_COUNT - 1U;
static EXT_RAM_BSS_ATTR char app_owners[sizeof(registered_apps) / sizeof(registered_apps[0])][SOLAR_OS_APP_OWNER_MAX];
static portMUX_TYPE app_owner_lock = portMUX_INITIALIZER_UNLOCKED;

static int app_registry_index_by_app(const solar_os_app_t *app)
{
    if (app == NULL) {
        return -1;
    }

    for (size_t i = 0; i < registered_app_count; i++) {
        if (registered_apps[i].app == app) {
            return (int)i;
        }
    }

    return -1;
}

#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
typedef struct {
    char id[SOLAR_OS_MODULE_PACKAGE_ID_MAX];
    const solar_os_app_registry_entry_t *entry;
    bool builtin;
} app_registry_module_t;

typedef struct {
    app_registry_module_t modules[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX];
    size_t count;
    size_t external_count;
    uint32_t revision;
    int64_t expires_us;
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    bool mounted, ready;
} app_registry_cache_t;

SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("shared app discovery cache in external BSS")
static EXT_RAM_BSS_ATTR app_registry_cache_t app_registry_cache;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("app discovery publication lock")
static portMUX_TYPE app_registry_cache_lock = portMUX_INITIALIZER_UNLOCKED;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("app discovery refresh serialization flag")
static bool app_registry_refreshing;

static bool app_registry_builtin_name(const char *name)
{
    for (size_t i = 0U; i < registered_app_count; ++i)
        if (!strcmp(name, registered_apps[i].name)) return true;
    return false;
}

static bool app_registry_external_visit(const char *id, void *user)
{
    app_registry_cache_t *cache = user;
    if (cache->count == SOLAR_OS_MODULE_PACKAGE_COUNT_MAX) return false;
    app_registry_module_t *module = &cache->modules[cache->count++];
    strlcpy(module->id, id, sizeof(module->id));
    module->entry = solar_os_native_foreground_find(id);
    module->builtin = app_registry_builtin_name(id);
    if (module->entry && !module->builtin) cache->external_count++;
    return true;
}

static void app_registry_cache_ensure(void)
{
    const uint32_t revision = solar_os_storage_revision();
    const int64_t now = esp_timer_get_time();
    const bool mounted = solar_os_storage_is_mounted();
    char root[SOLAR_OS_STORAGE_PATH_MAX];
    if (solar_os_storage_default_path("modules/apps", root, sizeof(root)) != ESP_OK)
        root[0] = '\0';
    portENTER_CRITICAL(&app_registry_cache_lock);
    const bool current = app_registry_cache.ready &&
        app_registry_cache.revision == revision && app_registry_cache.mounted == mounted &&
        !strcmp(app_registry_cache.root, root) && now < app_registry_cache.expires_us;
    if (current || app_registry_refreshing) {
        portEXIT_CRITICAL(&app_registry_cache_lock);
        return;
    }
    app_registry_refreshing = true;
    portEXIT_CRITICAL(&app_registry_cache_lock);

    /* Build outside the lock. Published descriptors remain immutable and can
     * still belong to a reserved or suspended session after a refresh. */
    app_registry_cache_t *next = solar_os_memory_calloc(1U, sizeof(*next),
        SOLAR_OS_MEMORY_TRANSIENT, "app.discovery");
    if (next) {
        next->revision = revision;
        next->mounted = mounted;
        strlcpy(next->root, root, sizeof(next->root));
        const esp_err_t err = mounted && root[0] ?
            solar_os_module_package_foreach_installed(SOLAR_OS_MODULE_TYPE_APP,
                app_registry_external_visit, next) : ESP_OK;
        if (err != ESP_OK) next->count = next->external_count = 0U;
        /* VFS users can bypass storage notifications. Periodically discover
         * their changes; ordinary completion does no filesystem I/O. */
        next->expires_us = esp_timer_get_time() +
            (err == ESP_OK ? 30000000LL : 1000000LL);
        next->ready = true;
    }
    const uint32_t after_revision = solar_os_storage_revision();
    portENTER_CRITICAL(&app_registry_cache_lock);
    if (next && revision == after_revision) app_registry_cache = *next;
    if (!next && revision == after_revision) {
        /* Avoid retrying a failed allocation on every item of a menu loop. */
        memset(&app_registry_cache, 0, sizeof(app_registry_cache));
        app_registry_cache.revision = revision;
        app_registry_cache.mounted = mounted;
        strlcpy(app_registry_cache.root, root, sizeof(app_registry_cache.root));
        app_registry_cache.ready = true;
        app_registry_cache.expires_us = now + 1000000LL;
    }
    app_registry_refreshing = false;
    portEXIT_CRITICAL(&app_registry_cache_lock);
    solar_os_memory_free(next);
}

static const solar_os_app_registry_entry_t *app_registry_cached_find(const char *name)
{
    const solar_os_app_registry_entry_t *entry = NULL;
    app_registry_cache_ensure();
    portENTER_CRITICAL(&app_registry_cache_lock);
    for (size_t i = 0U; i < app_registry_cache.count; ++i) {
        if (!strcmp(app_registry_cache.modules[i].id, name)) {
            entry = app_registry_cache.modules[i].entry;
            break;
        }
    }
    portEXIT_CRITICAL(&app_registry_cache_lock);
    return entry;
}

#endif

void solar_os_app_registry_foreach_module(bool (*visit)(const char *id, void *user),
                                          void *user)
{
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    if (!visit) return;
    app_registry_cache_ensure();
    char ids[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX][SOLAR_OS_MODULE_PACKAGE_ID_MAX];
    portENTER_CRITICAL(&app_registry_cache_lock);
    const size_t count = app_registry_cache.count;
    for (size_t i = 0U; i < count; ++i)
        memcpy(ids[i], app_registry_cache.modules[i].id, sizeof(ids[i]));
    portEXIT_CRITICAL(&app_registry_cache_lock);
    /* Call visitors outside the lock, with one coherent set of names. */
    for (size_t i = 0U; i < count; ++i)
        if (!visit(ids[i], user)) break;
#else
    (void)visit;
    (void)user;
#endif
}

size_t solar_os_app_registry_count(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    app_registry_cache_ensure();
    portENTER_CRITICAL(&app_registry_cache_lock);
    const size_t count = registered_app_count + app_registry_cache.external_count;
    portEXIT_CRITICAL(&app_registry_cache_lock);
    return count;
#else
    return registered_app_count;
#endif
}

const solar_os_app_registry_entry_t *solar_os_app_registry_get(size_t index)
{
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    if (index >= registered_app_count) {
        app_registry_cache_ensure();
        const solar_os_app_registry_entry_t *entry = NULL;
        size_t external_index = index - registered_app_count;
        portENTER_CRITICAL(&app_registry_cache_lock);
        for (size_t i = 0U; i < app_registry_cache.count; ++i) {
            const app_registry_module_t *module = &app_registry_cache.modules[i];
            if (module->entry && !module->builtin &&
                external_index-- == 0U) { entry = module->entry; break; }
        }
        portEXIT_CRITICAL(&app_registry_cache_lock);
        return entry;
    }
    const solar_os_app_registry_entry_t *external =
        app_registry_cached_find(registered_apps[index].name);
    if (external) return external;
#else
    if (index >= registered_app_count) return NULL;
#endif
    return &registered_apps[index];
}

const solar_os_app_registry_entry_t *solar_os_app_registry_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    const solar_os_app_registry_entry_t *external = app_registry_cached_find(name);
    if (external != NULL) return external;
#endif

    for (size_t i = 0; i < registered_app_count; i++) {
        if (registered_apps[i].name != NULL && strcmp(registered_apps[i].name, name) == 0) {
            return &registered_apps[i];
        }
    }

    return NULL;
}

const solar_os_app_registry_entry_t *solar_os_app_registry_find_by_app(const solar_os_app_t *app)
{
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
    const solar_os_app_registry_entry_t *external = solar_os_native_foreground_entry(app);
    if (external) return external;
#endif
    const int index = app_registry_index_by_app(app);
    return index >= 0 ? &registered_apps[index] : NULL;
}

const solar_os_app_registry_entry_t *solar_os_app_registry_find_opener(const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    const size_t count = solar_os_app_registry_count();
    for (size_t i = 0; i < count; i++) {
        const solar_os_app_registry_entry_t *entry = solar_os_app_registry_get(i);
        if (entry != NULL && entry->app != NULL &&
            solar_os_app_file_types_match(entry->file_extensions, path)) {
            return entry;
        }
    }
    return NULL;
}

static void app_discovery_native_info(const solar_os_app_registry_entry_t *entry,
                                      solar_os_app_discovery_info_t *info)
{
    memset(info, 0, sizeof(*info));
    strlcpy(info->name, entry->name, sizeof(info->name));
    strlcpy(info->id, entry->name, sizeof(info->id));
    strlcpy(info->title, entry->name, sizeof(info->title));
    strlcpy(info->summary, entry->summary, sizeof(info->summary));
    info->kind = SOLAR_OS_APP_DISCOVERY_NATIVE;
}

#if SOLAR_OS_PACKAGE_SERVICE_PLAYGROUND
static size_t app_discovery_playground_count(void)
{
    size_t installed = 0U;
    const size_t count = solar_os_playground_app_count();
    for (size_t i = 0U; i < count; i++) {
        char id[SOLAR_OS_PLAYGROUND_ID_MAX];
        if (solar_os_playground_get_installed_app_id(i, id, sizeof(id))) {
            installed++;
        }
    }
    return installed;
}

static bool app_discovery_playground_get(size_t index,
                                         solar_os_app_discovery_info_t *info)
{
    size_t installed = 0U;
    const size_t count = solar_os_playground_app_count();
    for (size_t i = 0U; i < count; i++) {
        char id[SOLAR_OS_PLAYGROUND_ID_MAX];
        if (!solar_os_playground_get_installed_app_id(i, id, sizeof(id))) {
            continue;
        }
        if (installed++ != index) {
            continue;
        }

        solar_os_playground_app_info_t app;
        if (!solar_os_playground_get_app(i, &app)) {
            return false;
        }
        memset(info, 0, sizeof(*info));
        const int written = snprintf(info->name,
                                     sizeof(info->name),
                                     "playground:%s",
                                     app.id);
        if (written < 0 || (size_t)written >= sizeof(info->name)) {
            return false;
        }
        strlcpy(info->id, app.id, sizeof(info->id));
        strlcpy(info->title, app.name, sizeof(info->title));
        strlcpy(info->summary, app.description, sizeof(info->summary));
        strlcpy(info->runtime,
                solar_os_playground_runtime_name(app.runtime),
                sizeof(info->runtime));
        info->kind = SOLAR_OS_APP_DISCOVERY_PLAYGROUND;
        return true;
    }
    return false;
}
#endif

size_t solar_os_app_discovery_count(bool include_playground)
{
    size_t count = solar_os_app_registry_count();
#if SOLAR_OS_PACKAGE_SERVICE_PLAYGROUND
    if (include_playground) {
        count += app_discovery_playground_count();
    }
#else
    (void)include_playground;
#endif
    return count;
}

bool solar_os_app_discovery_get(size_t index,
                                bool include_playground,
                                solar_os_app_discovery_info_t *info)
{
    if (info == NULL) {
        return false;
    }
    const size_t native_count = solar_os_app_registry_count();
    if (index < native_count) {
        const solar_os_app_registry_entry_t *entry =
            solar_os_app_registry_get(index);
        if (entry == NULL) {
            return false;
        }
        app_discovery_native_info(entry, info);
        return true;
    }
#if SOLAR_OS_PACKAGE_SERVICE_PLAYGROUND
    return include_playground &&
        app_discovery_playground_get(index - native_count, info);
#else
    (void)include_playground;
    return false;
#endif
}

static esp_err_t app_registry_request_native_launch(
    solar_os_context_t *ctx,
    const solar_os_app_registry_entry_t *entry,
    size_t arg_count,
    const char *const args[])
{
    if (ctx == NULL || entry == NULL || entry->app == NULL ||
        arg_count >= SOLAR_OS_APP_ARG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t argc = arg_count + 1U;
    if (argc < entry->min_argc ||
        (entry->max_argc != 0U && argc > entry->max_argc)) {
        return ESP_ERR_INVALID_ARG;
    }

    char storage[SOLAR_OS_APP_ARG_MAX][SOLAR_OS_APP_ARG_LEN];
    char *argv[SOLAR_OS_APP_ARG_MAX] = {0};
    if (strlcpy(storage[0], entry->name, sizeof(storage[0])) >= sizeof(storage[0])) {
        return ESP_ERR_INVALID_SIZE;
    }
    argv[0] = storage[0];
    for (size_t i = 0U; i < arg_count; i++) {
        if (args == NULL || args[i] == NULL ||
            strlcpy(storage[i + 1U], args[i], sizeof(storage[i + 1U])) >=
                sizeof(storage[i + 1U])) {
            return ESP_ERR_INVALID_SIZE;
        }
        argv[i + 1U] = storage[i + 1U];
    }
    return solar_os_context_request_launch_ex(ctx, entry->app, (int)argc, argv,
                                              SOLAR_OS_LAUNCH_CHILD_RETURN);
}

esp_err_t solar_os_app_registry_request_launch(solar_os_context_t *ctx,
                                               const char *name,
                                               size_t arg_count,
                                               const char *const args[])
{
    if (ctx == NULL || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    static const char playground_prefix[] = "playground:";
    if (strncmp(name, playground_prefix, sizeof(playground_prefix) - 1U) != 0) {
        const solar_os_app_registry_entry_t *entry =
            solar_os_app_registry_find(name);
        return entry != NULL ?
            app_registry_request_native_launch(ctx, entry, arg_count, args) :
            ESP_ERR_NOT_FOUND;
    }

#if SOLAR_OS_PACKAGE_SERVICE_PLAYGROUND
    const char *id = name + sizeof(playground_prefix) - 1U;
    solar_os_playground_app_info_t app;
    if (id[0] == '\0' || !solar_os_playground_find_installed_app(id, &app)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (arg_count > SOLAR_OS_APP_ARG_MAX - 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *runtime_name = solar_os_playground_runtime_name(app.runtime);
    const solar_os_app_registry_entry_t *runtime =
        solar_os_app_registry_find(runtime_name);
    if (runtime == NULL || runtime->app == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char path[SOLAR_OS_APP_ARG_LEN];
    esp_err_t err = solar_os_playground_entry_path(&app, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }

    const char *runtime_args[SOLAR_OS_APP_ARG_MAX - 1U] = {path};
    for (size_t i = 0U; i < arg_count; i++) {
        runtime_args[i + 1U] = args != NULL ? args[i] : NULL;
    }
    return app_registry_request_native_launch(ctx,
                                              runtime,
                                              arg_count + 1U,
                                              runtime_args);
#else
    (void)arg_count;
    (void)args;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static bool app_registry_is_web_url(const char *target)
{
    return target != NULL &&
        (strncmp(target, "http://", 7U) == 0 ||
         strncmp(target, "https://", 8U) == 0);
}

static const solar_os_app_registry_entry_t *app_registry_find_target_opener(
    const char *path_or_url,
    char *resolved,
    size_t resolved_len)
{
    if (path_or_url == NULL || path_or_url[0] == '\0' ||
        resolved == NULL || resolved_len == 0U) {
        return NULL;
    }
    if (app_registry_is_web_url(path_or_url)) {
        if (strlcpy(resolved, path_or_url, resolved_len) >= resolved_len) {
            return NULL;
        }
        return solar_os_app_registry_find("web");
    }
    if (solar_os_storage_resolve_path(path_or_url, resolved, resolved_len) != ESP_OK) {
        return NULL;
    }
    return solar_os_app_registry_find_opener(resolved);
}

bool solar_os_app_registry_can_open(const char *path_or_url)
{
    char resolved[SOLAR_OS_APP_ARG_LEN];
    const solar_os_app_registry_entry_t *entry =
        app_registry_find_target_opener(path_or_url, resolved, sizeof(resolved));
    return entry != NULL && entry->app != NULL;
}

esp_err_t solar_os_app_registry_request_open(solar_os_context_t *ctx,
                                             const char *path_or_url)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char resolved[SOLAR_OS_APP_ARG_LEN];
    const solar_os_app_registry_entry_t *entry =
        app_registry_find_target_opener(path_or_url, resolved, sizeof(resolved));
    if (entry == NULL || entry->app == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const char *args[] = {resolved};
    return app_registry_request_native_launch(ctx, entry, 1U, args);
}

bool solar_os_app_registry_owner(const solar_os_app_t *app, char *owner, size_t owner_len)
{
    const int index = app_registry_index_by_app(app);
    char current[SOLAR_OS_APP_OWNER_MAX] = "";

    if (owner != NULL && owner_len > 0) {
        owner[0] = '\0';
    }
    if (index < 0) {
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
        return solar_os_native_foreground_owner(app, owner, owner_len);
#else
        return false;
#endif
    }

    portENTER_CRITICAL(&app_owner_lock);
    const bool claimed = app_owners[index][0] != '\0';
    if (claimed) {
        strlcpy(current, app_owners[index], sizeof(current));
    }
    portEXIT_CRITICAL(&app_owner_lock);

    if (claimed && owner != NULL && owner_len > 0) {
        strlcpy(owner, current, owner_len);
    }
    return claimed;
}

esp_err_t solar_os_app_registry_claim(const solar_os_app_t *app,
                                      const char *owner,
                                      char *current_owner,
                                      size_t current_owner_len)
{
    const int index = app_registry_index_by_app(app);
    char busy_owner[SOLAR_OS_APP_OWNER_MAX] = "";

    if (current_owner != NULL && current_owner_len > 0) {
        current_owner[0] = '\0';
    }
    if (index < 0) {
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
        if (solar_os_native_foreground_entry(app))
            return solar_os_native_foreground_claim(app, owner, current_owner, current_owner_len);
#endif
        return ESP_OK;
    }
    if (owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (app->worker_stack_bytes > 0 &&
        !solar_os_task_admit(app->name,
                             app->worker_stack_bytes,
                             SOLAR_OS_TASK_ROLE_FOREGROUND,
                             app->worker_stack_external)) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&app_owner_lock);
    if (app_owners[index][0] == '\0' || strcmp(app_owners[index], owner) == 0) {
        strlcpy(app_owners[index], owner, sizeof(app_owners[index]));
        portEXIT_CRITICAL(&app_owner_lock);
        return ESP_OK;
    }
    strlcpy(busy_owner, app_owners[index], sizeof(busy_owner));
    portEXIT_CRITICAL(&app_owner_lock);

    if (current_owner != NULL && current_owner_len > 0) {
        strlcpy(current_owner, busy_owner, current_owner_len);
    }
    return ESP_ERR_INVALID_STATE;
}

void solar_os_app_registry_release(const solar_os_app_t *app, const char *owner)
{
    const int index = app_registry_index_by_app(app);

    if (index < 0 || owner == NULL || owner[0] == '\0') {
#if SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES
        if (index < 0) solar_os_native_foreground_release(app, owner);
#endif
        return;
    }

    portENTER_CRITICAL(&app_owner_lock);
    if (strcmp(app_owners[index], owner) == 0) {
        app_owners[index][0] = '\0';
    }
    portEXIT_CRITICAL(&app_owner_lock);
}
