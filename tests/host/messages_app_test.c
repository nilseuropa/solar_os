/* Exercise the real browser, Messaging archive, and widgets with fake transport
 * and terminal endpoints. */
#define main messaging_existing_tests_main
#include "messaging_service_test.c"
#undef main
#include "../../src/apps/solar_os_messages.c"

#define TEST_ROWS 32U
#define TEST_COLS 100U
static char screen[TEST_ROWS][TEST_COLS + 1U];
static size_t rows = 10, cols = 32;
static bool cursor_visible;
static size_t cursor_row, cursor_col;
static messages_app_state_t app_state;
static solar_os_context_t context;
const solar_os_app_t solar_os_chat_app = {.name = "chat"};
static unsigned launches;
static char launch_selector[48];
static solar_os_launch_policy_t launch_policy;
void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *message)
{ (void)message; assert(!code); ctx->exit_requested = true; }
esp_err_t solar_os_context_request_launch_ex(solar_os_context_t *ctx, const solar_os_app_t *app,
    int argc, char **argv, solar_os_launch_policy_t policy)
{
    (void)ctx; assert(app == &solar_os_chat_app && argc >= 1 && !strcmp(argv[0], "chat"));
    launches++; launch_policy = policy;
    strlcpy(launch_selector, argc == 2 ? argv[1] : "", sizeof(launch_selector)); return ESP_OK;
}
bool solar_os_jobs_get_by_name(const char *name, solar_os_job_status_t *status)
{
    assert(!strcmp(name, "test-worker"));
    *status = (solar_os_job_status_t){.name = "test-worker", .state = SOLAR_OS_JOB_STOPPED};
    return true;
}
size_t solar_os_jobs_count(void) { return 0; }
bool solar_os_jobs_get(size_t index, solar_os_job_status_t *status)
{ (void)index; (void)status; return false; }
const char *solar_os_job_state_name(solar_os_job_state_t state)
{ (void)state; return "stopped"; }

typedef struct { bool enabled; char endpoint[48], secret[48]; } form_config_t;
static form_config_t configs[256];
static const solar_os_messaging_setting_t fields[] = {
    {"Endpoint", "Server hostname", 48, false, true},
    {"Secret", "Empty clears", 48, true, false},
};
static esp_err_t fake_read(void *ctx, unsigned form, solar_os_messaging_settings_t *out)
{
    if (form) return ESP_ERR_NOT_FOUND;
    out->label = "Test provider"; out->fields = fields; out->count = 2;
    strlcpy(out->worker, "test-worker", sizeof(out->worker));
    if (ctx) {
        form_config_t *config = ctx; out->enabled = config->enabled;
        solar_os_messaging_settings_value(out, 0, config->endpoint);
        solar_os_messaging_settings_value(out, 1, config->secret);
    }
    return ESP_OK;
}
static esp_err_t fake_create(const char *name, int argc, const char *const *argv,
    solar_os_messaging_provider_id_t *id)
{
    assert(argc == 1);
    form_config_t config = {0}; strlcpy(config.endpoint, argv[0], sizeof(config.endpoint));
    esp_err_t error = solar_os_messaging_instance_create("test", name, &config, sizeof(config), id);
    if (error != ESP_OK) return error;
    configs[*id] = config;
    return solar_os_messaging_instance_register(*id, "test", name, &configs[*id]);
}
static esp_err_t fake_write(void *ctx, unsigned form, const char *name,
    const solar_os_messaging_settings_t *in, solar_os_messaging_provider_id_t *id)
{
    (void)form;
    if (!ctx) { const char *args[] = {in->values[0]}; return fake_create(name, 1, args, id); }
    form_config_t *config = ctx;
    *id = (solar_os_messaging_provider_id_t)(config - configs);
    if (in->changed[0]) strlcpy(config->endpoint, in->values[0], sizeof(config->endpoint));
    if (in->changed[1]) strlcpy(config->secret, in->values[1], sizeof(config->secret));
    return ESP_OK;
}
static esp_err_t fake_connect(void *ctx, bool enabled)
{ ((form_config_t *)ctx)->enabled = enabled; return ESP_OK; }
static esp_err_t fake_remove(void *ctx) { (void)ctx; return ESP_OK; }
static void fake_removed(void *ctx, bool removed) { (void)ctx; (void)removed; }
static const solar_os_messaging_provider_class_t fake_class = {
    .name = "test", .create = fake_create, .settings_read = fake_read, .settings_write = fake_write,
    .connect = fake_connect, .remove_prepare = fake_remove, .remove_finish = fake_removed,
};

size_t solar_os_tui_rows(const solar_os_tui_t *tui) { (void)tui; return rows; }
size_t solar_os_tui_cols(const solar_os_tui_t *tui) { (void)tui; return cols; }
esp_err_t solar_os_tui_begin(solar_os_tui_t *tui, solar_os_context_t *ctx)
{ (void)tui; (void)ctx; return ESP_OK; }
void solar_os_tui_end(solar_os_tui_t *tui) { (void)tui; }
void solar_os_tui_attach_session(solar_os_tui_t *tui) { (void)tui; }
esp_err_t solar_os_tui_enable_diff(solar_os_tui_t *tui, bool enabled)
{ tui->diff_enabled = enabled; return ESP_OK; }
bool solar_os_terminal_status_bar_visible(const solar_os_terminal_t *terminal)
{ (void)terminal; return true; }
esp_err_t solar_os_terminal_set_status_bar_visible_transient(solar_os_terminal_t *terminal, bool visible)
{ (void)terminal; (void)visible; return ESP_OK; }
void solar_os_tui_clear(solar_os_tui_t *tui)
{
    (void)tui;
    cursor_row = cursor_col = 0U;
    for (size_t row = 0; row < TEST_ROWS; row++) {
        memset(screen[row], ' ', cols); screen[row][cols] = '\0';
    }
}
void solar_os_tui_refresh(solar_os_tui_t *tui) { (void)tui; }
esp_err_t solar_os_tui_set_cursor_visible(solar_os_tui_t *tui, bool visible)
{ (void)tui; cursor_visible = visible; return ESP_OK; }
esp_err_t solar_os_tui_fill(solar_os_tui_t *tui, size_t row, size_t col,
    size_t height, size_t width, uint32_t ch, uint8_t attr)
{
    (void)tui; (void)attr; assert(row + height <= rows && col + width <= cols);
    for (size_t y = row; y < row + height; y++)
        for (size_t x = col; x < col + width; x++)
            solar_os_tui_putch(tui, y, x, ch, attr);
    return ESP_OK;
}
esp_err_t solar_os_tui_putch(solar_os_tui_t *tui, size_t row, size_t col, uint32_t ch, uint8_t attr)
{
    (void)tui; (void)attr; assert(row < rows && col < cols);
    screen[row][col] = ch < 128 ? (char)ch : '?';
    cursor_row = row;
    cursor_col = col + 1U;
    if (cursor_col == cols) {
        cursor_col = 0U;
        if (cursor_row + 1U < rows) cursor_row++;
    }
    return ESP_OK;
}
esp_err_t solar_os_tui_move(solar_os_tui_t *tui, size_t row, size_t col)
{ (void)tui; assert(row < rows && col < cols); cursor_row = row; cursor_col = col; return ESP_OK; }


static void key(uint8_t ch)
{
    solar_os_event_t event = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = (char)ch};
    assert(messages_event(&context, &event));
}
static void type(const char *text) { while (*text) key((uint8_t)*text++); }
static bool screen_has(const char *text)
{
    for (size_t row = 0; row < rows; row++) if (strstr(screen[row], text)) return true;
    return false;
}

int main(void)
{
    assert(mkdtemp(test_root));
    assert(solar_os_messaging_class_register(&fake_class) == ESP_OK);
    const char *args[] = {"server"};
    solar_os_messaging_provider_id_t first, second;
    assert(fake_create("first", 1, args, &first) == ESP_OK);
    assert(fake_create("second", 1, args, &second) == ESP_OK);
    strlcpy(configs[first].secret, "synthetic-secret", sizeof(configs[first].secret));
    messages_app_state = &app_state;
    assert(messages_start(&context) == ESP_OK);
    assert(screen_has("a Actions") && screen_has("? Help"));
    app_state.selected = first; messages_refresh(); messages_render();

    /* Form editing keeps secrets out of both readback and terminal output. */
    key('e'); assert(app_state.mode == MESSAGES_FORM);
    assert(app_state.settings.present[1] && !app_state.settings.values[1][0]);
    assert(!screen_has("synthetic-secret"));
    key('\r'); assert(app_state.mode == MESSAGES_EDIT_FIELD && cursor_visible);
    assert(cursor_row == 3 && cursor_col == strlen("Endpoint: server"));
    assert(strstr(screen[3], "Endpoint: server") && strstr(screen[4], "Secret: [saved]"));
    key(SOLAR_OS_KEY_HOME); assert(cursor_row == 3 && cursor_col == strlen("Endpoint: "));
    key(SOLAR_OS_KEY_END); type("-edited"); key('\r'); key('s');
    assert(!strcmp(configs[first].endpoint, "server-edited"));
    assert(!strcmp(configs[first].secret, "synthetic-secret"));
    key('e'); key(SOLAR_OS_KEY_DOWN); key('\r'); type("q");
    assert(cursor_visible && cursor_row == 4 && cursor_col == strlen("Secret: ") + 1);
    assert(strstr(screen[4], "Secret: *") && !strstr(screen[4], "Secret: q"));
    key(SOLAR_OS_KEY_ESCAPE); key(SOLAR_OS_KEY_ESCAPE);
    assert(!context.exit_requested && !strcmp(configs[first].secret, "synthetic-secret"));

    /* Enabled and worker-stopped are separate from transport connection state. */
    key('\t'); key('c'); assert(configs[first].enabled);
    assert(screen_has("worker stopped"));
    key('\r'); assert(app_state.mode == MESSAGES_CONNECTION_DETAIL);
    key(SOLAR_OS_KEY_END); key(SOLAR_OS_KEY_ESCAPE);
    key('x'); assert(!configs[first].enabled);

    solar_os_messaging_inbound_t inbound = {.provider = first, .conversation_key = "room",
        .conversation_kind = SOLAR_OS_CONVERSATION_ROOM, .conversation_title = "Room",
        .sender = "Sender", .body = "first body\nsecond line"};
    bool inserted; solar_os_message_key_t message, neighbor;
    inbound.provider_message_key = 111;
    assert(solar_os_messaging_publish_inbound(&inbound, &inserted, &message) == ESP_OK && inserted);
    inbound.provider_message_key = 112; inbound.body = "keep this message";
    assert(solar_os_messaging_publish_inbound(&inbound, &inserted, &neighbor) == ESP_OK && inserted);
    key('\t'); assert(app_state.tab == MESSAGES_BROWSER && app_state.count == 1);
    key('\r'); assert(app_state.view == MESSAGES_HISTORY && app_state.count == 2);
    key('c'); assert(launches == 1 && launch_policy == SOLAR_OS_LAUNCH_CHILD_RETURN && launch_selector[0]);
    size_t cursor_before = app_state.browser.cursor;
    messages_suspend(&context); messages_resume(&context);
    assert(app_state.tab == MESSAGES_BROWSER && app_state.view == MESSAGES_HISTORY && app_state.browser.cursor == cursor_before);
    key('\r'); assert(app_state.mode == MESSAGES_DETAIL && app_state.detail.key == message);
    key('d'); key('n');
    assert(solar_os_messaging_message_get(message, &app_state.detail) == ESP_OK);
    key('d'); key('y');
    assert(solar_os_messaging_message_get(message, &app_state.detail) == ESP_ERR_NOT_FOUND);
    assert(solar_os_messaging_message_get(neighbor, &app_state.detail) == ESP_OK);

    /* Pending message deletion fails without hiding it; explicit cancellation
     * removes it from the outbox and allows individual history deletion. */
    solar_os_conversation_id_t conversation = app_state.detail.conversation_id;
    solar_os_message_key_t pending;
    assert(solar_os_messaging_send(conversation, "queued", false, &pending) == ESP_OK);
    messages_refresh();
    for (size_t i = 0; i < app_state.count; i++) if (app_state.page.messages[i].key == pending) app_state.browser.cursor = i;
    key('d'); key('y');
    assert(strstr(app_state.feedback, "cancel") && solar_os_messaging_message_get(pending, &app_state.detail) == ESP_OK);
    key('x'); key('y'); key('d'); key('y');
    assert(solar_os_messaging_message_get(pending, &app_state.detail) == ESP_ERR_NOT_FOUND);

    key(SOLAR_OS_KEY_ESCAPE); key('v'); assert(app_state.view == MESSAGES_UNREAD);
    assert(app_state.count == 1 && app_state.page.messages[0].key == neighbor);
    key('r'); assert(app_state.count == 0);
    key('v'); assert(app_state.view == MESSAGES_OUTBOX && app_state.count == 0);
    key('f'); key('\r'); assert(app_state.selected == 0);
    key('\t'); assert(app_state.tab == MESSAGES_INSTANCES);
    app_state.selected = first; messages_refresh();
    key(SOLAR_OS_KEY_DOWN); assert(app_state.selected == second);
    key('d'); key(SOLAR_OS_KEY_ESCAPE);
    assert(solar_os_messaging_provider_find("second") == second);

    /* Creation uses provider metadata, including on a compact terminal. */
    key('n'); assert(app_state.mode == MESSAGES_CLASSES);
    key('\r'); assert(app_state.mode == MESSAGES_FORM);
    key('\r'); assert(cursor_visible && cursor_row == 3 && cursor_col == strlen("Instance name: "));
    type("third"); key('\r'); key(SOLAR_OS_KEY_DOWN);
    key('\r'); type("new-server"); key('\r'); key('s');
    assert(solar_os_messaging_provider_find("third") && app_state.mode == MESSAGES_BROWSE);
    key('d'); key('y');
    assert(!solar_os_messaging_provider_find("third") && solar_os_messaging_provider_find("first") == first);
    assert(solar_os_messaging_message_get(neighbor, &app_state.detail) == ESP_OK);

    inbound.provider = first; inbound.body = "paged unread";
    for (unsigned i = 0; i < 20; i++) {
        inbound.provider_message_key = 200 + i;
        assert(solar_os_messaging_publish_inbound(&inbound, &inserted, NULL) == ESP_OK && inserted);
    }
    app_state.tab = MESSAGES_BROWSER; app_state.view = MESSAGES_UNREAD;
    app_state.selected = first; messages_reset_browser();
    assert(app_state.count == 16 && app_state.more);
    key(SOLAR_OS_KEY_PAGE_UP); assert(app_state.offset == 16 && app_state.count == 4 && !app_state.more);
    key(SOLAR_OS_KEY_PAGE_DOWN); assert(app_state.offset == 0 && app_state.count == 16);
    for (cols = 20; cols <= 80; cols += 12) {
        for (rows = 7; rows <= 24; rows += 5) {
            messages_render(); key('?'); key(SOLAR_OS_KEY_END); key(SOLAR_OS_KEY_ESCAPE);
        }
    }
    rows = 10; cols = 32;
    messages_stop(&context);
    puts("messages browser, deletion, forms and child-return tests: ok");
    return 0;
}
