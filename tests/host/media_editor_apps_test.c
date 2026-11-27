#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_native_client_ui.h"
#include "solar_os_native_ui_abi.h"
#include "solar_os_webradio_catalog.h"
#include "media_dialog_types.inc"

static struct {
    webradio_dialog_t dialog;
    bool redraw;
    char dialog_input[SOLAR_OS_WEBRADIO_URL_MAX];
    char edit_name[SOLAR_OS_WEBRADIO_STATION_NAME_MAX], edit_url[SOLAR_OS_WEBRADIO_URL_MAX];
    solar_os_ui_input_state_t dialog_input_state;
    const solar_os_native_ui_api_v1_t *ui_api;
} webradio;
static struct {
    void *task;
    bool redraw, editing_filename;
    char filename[RECORDER_FILENAME_MAX], edit_filename[RECORDER_FILENAME_MAX];
    solar_os_ui_input_state_t edit_filename_state;
    const solar_os_native_ui_api_v1_t *ui_api;
} recorder;
static unsigned saves;
static void webradio_finish_dialog(void) { saves++; }
static void webradio_set_ui_message(const char *message) { (void)message; }
size_t strlcpy(char *out, const char *in, size_t capacity)
{
    const size_t length = strlen(in);
    if (capacity) { const size_t n = length < capacity ? length : capacity - 1; memcpy(out, in, n); out[n] = 0; }
    return length;
}
#include "media_dialog_handlers.inc"

int main(void)
{
    const solar_os_native_ui_api_v1_t ui = {.input_key = solar_os_ui_input_key, .input_text = solar_os_ui_input_text};
    webradio.ui_api = recorder.ui_api = &ui;
    webradio.dialog = WEBRADIO_DIALOG_ADD_NAME;
    webradio_dialog_set_input("T\xc3\xb6ne");
    assert(webradio_handle_dialog_key(SOLAR_OS_KEY_LEFT));
    webradio.redraw = false;
    assert(webradio_handle_dialog_key(SOLAR_OS_KEY_LEFT) && webradio.redraw);
    assert(webradio_handle_dialog_text("\xc3\xa4") && !strcmp(webradio.dialog_input, "T\xc3\xb6\xc3\xa4ne"));
    assert(webradio_handle_dialog_key(SOLAR_OS_KEY_DELETE) && !strcmp(webradio.dialog_input, "T\xc3\xb6\xc3\xa4" "e"));
    assert(webradio_handle_dialog_key('\b') && !strcmp(webradio.dialog_input, "T\xc3\xb6" "e"));
    assert(!webradio_handle_dialog_text("\xe2\x82"));
    assert(webradio_handle_dialog_key('\r') && webradio.dialog == WEBRADIO_DIALOG_ADD_URL);
    assert(!webradio.dialog_input[0] && !webradio.dialog_input_state.cursor);
    assert(webradio_handle_dialog_text("https://example.test/live"));
    assert(webradio_handle_dialog_key('\n') && saves == 1);
    webradio.dialog = WEBRADIO_DIALOG_EDIT_NAME;
    memset(webradio.dialog_input, 'a', SOLAR_OS_WEBRADIO_STATION_NAME_MAX - 1);
    webradio.dialog_input[SOLAR_OS_WEBRADIO_STATION_NAME_MAX - 1] = 0;
    webradio.dialog_input_state.cursor = strlen(webradio.dialog_input);
    assert(!webradio_handle_dialog_text("\xc3\xb6"));
    assert(webradio_handle_dialog_key(SOLAR_OS_KEY_ESCAPE) && !webradio.dialog && !webradio.dialog_input[0]);

    strcpy(recorder.filename, "T\xc3\xb6ne.wav");
    recorder_begin_filename_edit();
    assert(recorder.editing_filename && recorder.edit_filename_state.cursor == strlen(recorder.filename));
    assert(recorder_handle_filename_key(SOLAR_OS_KEY_HOME));
    assert(recorder_handle_filename_text("\xc3\x84"));
    assert(!recorder_handle_filename_text("/") && !recorder_handle_filename_text("\\"));
    assert(!recorder_handle_filename_text("x/y") && !recorder_handle_filename_text("\xed\xa0\x80"));
    assert(recorder_handle_filename_key(SOLAR_OS_KEY_DELETE));
    assert(!strcmp(recorder.edit_filename, "\xc3\x84\xc3\xb6ne.wav"));
    assert(recorder_handle_filename_key('\n') && !recorder.editing_filename);
    assert(!strcmp(recorder.filename, "\xc3\x84\xc3\xb6ne.wav"));
    recorder_begin_filename_edit();
    assert(recorder_handle_filename_text("x"));
    assert(recorder_handle_filename_key(SOLAR_OS_KEY_ESCAPE) && !recorder.editing_filename);
    assert(!strcmp(recorder.filename, "\xc3\x84\xc3\xb6ne.wav"));
    puts("Actual radio/recorder dialogs: shared cursor, UTF-8 insertion/deletion, redraw, capacity, filename rules, submit and cancel passed");
}
