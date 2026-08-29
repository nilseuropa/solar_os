#include "solar_os_app_registry.h"

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "solar_os_config.h"
#include "solar_os_task.h"
#include "solar_os_app_file_types.h"
#if SOLAR_OS_PACKAGE_APP_APLAY || SOLAR_OS_PACKAGE_APP_ARECORD
#include "solar_os_audio_apps.h"
#endif
#if SOLAR_OS_PACKAGE_APP_CHAT
#include "solar_os_chat_app.h"
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
#if SOLAR_OS_PACKAGE_APP_SSH
#include "solar_os_ssh_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_WEB
#include "solar_os_web.h"
#endif
#if SOLAR_OS_PACKAGE_APP_WEBRADIO
#include "solar_os_webradio.h"
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
#if SOLAR_OS_PACKAGE_APP_NOTES
#include "solar_os_notes.h"
#endif
#if SOLAR_OS_PACKAGE_APP_PLOT
#include "solar_os_plot.h"
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
#include "solar_os_playground_app.h"
#endif
#if SOLAR_OS_PACKAGE_APP_LOGIC
#include "solar_os_logic_app.h"
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
#if SOLAR_OS_PACKAGE_APP_GAMEBOY
#include "solar_os_gameboy.h"
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

#define APP_ENTRY(app_name, app_summary, app_ptr, app_caps, app_usage, app_min, app_max) \
    {.name = app_name, .summary = app_summary, .app = app_ptr, .capabilities = app_caps, \
     .usage = app_usage, .min_argc = app_min, .max_argc = app_max}
#define APP_FILE_ENTRY(app_name, app_summary, app_ptr, app_caps, app_usage, app_min, app_max, app_extensions) \
    {.name = app_name, .summary = app_summary, .app = app_ptr, .capabilities = app_caps, \
     .usage = app_usage, .file_extensions = app_extensions, \
     .min_argc = app_min, .max_argc = app_max}

static const solar_os_app_registry_entry_t registered_apps[] = {
#if SOLAR_OS_PACKAGE_APP_APLAY
    APP_ENTRY("aplay", "play WAV/MP3 audio", &solar_os_aplay_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "aplay [-v volume] <file.wav|file.mp3>", 2, 4),
#endif
#if SOLAR_OS_PACKAGE_APP_ARECORD
    APP_ENTRY("arecord", "record WAV audio", &solar_os_arecord_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "arecord [-d seconds] <file.wav>", 2, 4),
#endif
#if SOLAR_OS_PACKAGE_APP_RECORDER
    APP_ENTRY("recorder", "interactive WAV recorder", &solar_os_recorder_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "recorder [--tui] [file.wav]", 1, 3),
#endif
#if SOLAR_OS_PACKAGE_APP_CHAT
    APP_ENTRY("chat", "provider-neutral conversation client", &solar_os_chat_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "chat [gateway|meshcore|link|conversation-id]", 1, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_CONTACTS
    APP_ENTRY("contacts", "provider-neutral contact browser", &solar_os_contacts_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "contacts", 1, 1),
#endif
#if SOLAR_OS_PACKAGE_APP_AGENT
    APP_ENTRY("agent", "native LLM agent", &solar_os_agent_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "agent [new|resume <id>|ask <prompt...>|script <file> [args...]]", 1, 0),
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
#if SOLAR_OS_PACKAGE_APP_SSH
    APP_ENTRY("ssh", "SSH client", &solar_os_ssh_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "ssh [user@]host [port]", 2, 3),
#endif
#if SOLAR_OS_PACKAGE_APP_WEB
    APP_ENTRY("web", "simple web browser", &solar_os_web_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "web <URL>", 2, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_WEBRADIO
    APP_ENTRY("webradio", "streaming internet radio", &solar_os_webradio_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "webradio [--tui] [URL] | webradio [--tui] <list | add NAME URL | remove NAME | reset>", 1, 5),
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
    APP_ENTRY("help", "browse the SolarOS manual", &solar_os_docs_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "help [topic]", 1, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_EMAIL
    APP_ENTRY("email", "IMAP email client", &solar_os_email_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "email", 1, 1),
#endif
#if SOLAR_OS_PACKAGE_APP_FILES
    APP_ENTRY("files", "two-pane file manager and launcher", &solar_os_files_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "files [--launcher] [path]", 1, 3),
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
#if SOLAR_OS_PACKAGE_APP_NOTES
    APP_ENTRY("notes", "Markdown checklist notes", &solar_os_notes_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "notes [file.md]", 1, 2),
#endif
#if SOLAR_OS_PACKAGE_APP_PLOT
    APP_ENTRY("plot", "plot DAQ CSV files or scalar streams", &solar_os_plot_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "plot <stream...> [--rate ms] | plot -f <file.csv> [column...]", 2, 0),
#endif
#if SOLAR_OS_PACKAGE_APP_PLAYGROUND
    APP_ENTRY("playground", "browse and run community scripts", &solar_os_playground_app, SOLAR_OS_APP_CAP_TEXT | SOLAR_OS_APP_CAP_DISPLAY | SOLAR_OS_APP_CAP_PORT, "playground [source|storage|reload|install|delete] ...", 1, 4),
#endif
#if SOLAR_OS_PACKAGE_APP_LOGIC
    APP_ENTRY("logic", "logic analyzer waveform viewer", &solar_os_logic_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "logic <pin[,pin...]> [rate] [samples] [trigger=pin]", 2, 5),
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
#if SOLAR_OS_PACKAGE_APP_GAMEBOY
    APP_FILE_ENTRY("gameboy", "original Game Boy emulator", &solar_os_gameboy_app, SOLAR_OS_APP_CAP_GRAPHICS | SOLAR_OS_APP_CAP_DISPLAY, "gameboy <file.gb>", 2, 2, ".gb"),
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
    {0},
};

#undef APP_ENTRY
#undef APP_FILE_ENTRY

#define REGISTERED_APP_STORAGE_COUNT (sizeof(registered_apps) / sizeof(registered_apps[0]))

static const size_t registered_app_count = REGISTERED_APP_STORAGE_COUNT - 1U;
static char app_owners[sizeof(registered_apps) / sizeof(registered_apps[0])][SOLAR_OS_APP_OWNER_MAX];
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

size_t solar_os_app_registry_count(void)
{
    return registered_app_count;
}

const solar_os_app_registry_entry_t *solar_os_app_registry_get(size_t index)
{
    if (index >= registered_app_count) {
        return NULL;
    }

    return &registered_apps[index];
}

const solar_os_app_registry_entry_t *solar_os_app_registry_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < registered_app_count; i++) {
        if (registered_apps[i].name != NULL && strcmp(registered_apps[i].name, name) == 0) {
            return &registered_apps[i];
        }
    }

    return NULL;
}

const solar_os_app_registry_entry_t *solar_os_app_registry_find_by_app(const solar_os_app_t *app)
{
    const int index = app_registry_index_by_app(app);
    return index >= 0 ? &registered_apps[index] : NULL;
}

const solar_os_app_registry_entry_t *solar_os_app_registry_find_opener(const char *path)
{
    if (path == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < registered_app_count; i++) {
        const solar_os_app_registry_entry_t *entry = &registered_apps[i];
        if (entry->app != NULL &&
            solar_os_app_file_types_match(entry->file_extensions, path)) {
            return entry;
        }
    }
    return NULL;
}

bool solar_os_app_registry_owner(const solar_os_app_t *app, char *owner, size_t owner_len)
{
    const int index = app_registry_index_by_app(app);
    char current[SOLAR_OS_APP_OWNER_MAX] = "";

    if (owner != NULL && owner_len > 0) {
        owner[0] = '\0';
    }
    if (index < 0) {
        return false;
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
        return;
    }

    portENTER_CRITICAL(&app_owner_lock);
    if (strcmp(app_owners[index], owner) == 0) {
        app_owners[index][0] = '\0';
    }
    portEXIT_CRITICAL(&app_owner_lock);
}
