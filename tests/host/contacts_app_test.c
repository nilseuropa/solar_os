#include <assert.h>
#include "../../src/apps/solar_os_contacts.c"

#define TEST_ROWS 24U
#define TEST_COLS 100U
static char screen[TEST_ROWS][TEST_COLS + 1U];
static size_t rows = 4U, cols = 32U;
static bool cursor_visible;
static size_t cursor_row, cursor_col;
static contacts_app_state_t state;
static solar_os_contact_t records[4];
static solar_os_endpoint_t endpoints[4];
static uint32_t generation = 1U;
static unsigned mutations;
static solar_os_contact_id_t affected, link_target, link_source;
static solar_os_endpoint_id_t affected_endpoint;
static solar_os_contact_trust_t affected_trust;
static esp_err_t mutation_error;
static unsigned allocations;
#if SOLAR_OS_PACKAGE_APP_CHAT
static unsigned direct_opens, chat_launches;
static solar_os_contact_id_t chat_contact;
static esp_err_t direct_error, launch_error;
const solar_os_app_t solar_os_chat_app = {.name = "chat"};

esp_err_t solar_os_messaging_direct_open(solar_os_contact_id_t contact,
    solar_os_conversation_id_t *conversation)
{
    direct_opens++;
    chat_contact = contact;
    if (direct_error != ESP_OK) return direct_error;
    *conversation = 100U + contact;
    return ESP_OK;
}

esp_err_t solar_os_context_request_launch_ex(solar_os_context_t *ctx,
    const solar_os_app_t *app, int argc, char **argv, solar_os_launch_policy_t mode)
{
    assert(ctx != NULL && app == &solar_os_chat_app);
    assert(argc == 2 && !strcmp(argv[0], "chat"));
    assert(strtoul(argv[1], NULL, 10) == 100U + chat_contact);
    assert(mode == SOLAR_OS_LAUNCH_CHILD_RETURN);
    chat_launches++;
    return launch_error;
}
#endif

size_t strlcpy(char *dst, const char *src, size_t capacity)
{
    const size_t length = strlen(src);
    if (capacity) {
        const size_t copy = length < capacity - 1U ? length : capacity - 1U;
        memcpy(dst, src, copy); dst[copy] = '\0';
    }
    return length;
}
const char *esp_err_to_name(esp_err_t error) { return error == ESP_OK ? "OK" : "ESP_ERR_NOT_FOUND"; }
const char *solar_os_messaging_provider_name(solar_os_messaging_provider_id_t provider)
{ (void)provider; return "meshcore"; }
const char *solar_os_contact_trust_name(solar_os_contact_trust_t trust)
{ return trust == SOLAR_OS_CONTACT_TRUST_TRUSTED ? "trusted" : trust == SOLAR_OS_CONTACT_TRUST_BLOCKED ? "blocked" : "discovered"; }
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED && tag); allocations++; return calloc(count, size); }
void solar_os_memory_free(void *pointer) { if (pointer) allocations--; free(pointer); }
void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *message)
{ (void)message; assert(code == 0); ctx->exit_requested = true; }

esp_err_t solar_os_contacts_init(void) { return ESP_OK; }
esp_err_t solar_os_contacts_get_status(solar_os_contacts_status_t *status)
{
    *status = (solar_os_contacts_status_t){.contact_count = 3, .endpoint_count = 4,
        .contact_capacity = SOLAR_OS_CONTACT_CAPACITY, .endpoint_capacity = SOLAR_OS_ENDPOINT_CAPACITY,
        .generation = generation, .records_in_psram = true, .persistent = true};
    return ESP_OK;
}
size_t solar_os_contacts_snapshot(solar_os_contact_t *out, size_t capacity, bool filter,
    solar_os_contact_trust_t trust, size_t *total)
{
    (void)trust; assert(!filter);
    size_t count = 0;
    for (size_t i = 0; i < 4; i++) if (records[i].id && count < capacity) out[count++] = records[i];
    if (total) *total = count;
    return count;
}
esp_err_t solar_os_contacts_get(solar_os_contact_id_t id, solar_os_contact_t *out)
{
    for (size_t i = 0; i < 4; i++) if (id && records[i].id == id) { *out = records[i]; return ESP_OK; }
    return ESP_ERR_NOT_FOUND;
}
size_t solar_os_contacts_endpoint_snapshot(solar_os_contact_id_t id, solar_os_endpoint_t *out, size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; i < 4; i++)
        if (endpoints[i].id && (!id || endpoints[i].contact_id == id) && count < capacity)
            out[count++] = endpoints[i];
    return count;
}
esp_err_t solar_os_contacts_set_trust(solar_os_contact_id_t id, solar_os_endpoint_id_t endpoint,
    solar_os_contact_trust_t trust)
{
    mutations++; affected = id; affected_endpoint = endpoint; affected_trust = trust;
    if (mutation_error) return mutation_error;
    for (size_t i = 0; i < 4; i++) if (endpoints[i].contact_id == id && (!endpoint || endpoints[i].id == endpoint))
        endpoints[i].trust = trust;
    generation++;
    return ESP_OK;
}
esp_err_t solar_os_contacts_rename(solar_os_contact_id_t id, const char *name)
{
    mutations++; affected = id;
    if (mutation_error) return mutation_error;
    if (!name[0]) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < 4; i++) if (records[i].id == id)
        strlcpy(records[i].display_name, name, sizeof(records[i].display_name));
    generation++; return ESP_OK;
}
esp_err_t solar_os_contacts_remove(solar_os_contact_id_t id)
{
    mutations++; affected = id;
    if (mutation_error) return mutation_error;
    for (size_t i = 0; i < 4; i++) {
        if (records[i].id == id) records[i].id = 0;
        if (endpoints[i].contact_id == id) endpoints[i].id = 0;
    }
    generation++; return ESP_OK;
}
esp_err_t solar_os_contacts_link(solar_os_contact_id_t target, solar_os_contact_id_t source)
{
    mutations++; link_target = target; link_source = source;
    if (mutation_error) return mutation_error;
    for (size_t i = 0; i < 4; i++) {
        if (records[i].id == source) records[i].id = 0;
        if (endpoints[i].contact_id == source) endpoints[i].contact_id = target;
    }
    generation++; return ESP_OK;
}

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

static solar_os_context_t context;
static void key(uint8_t ch)
{
    const solar_os_event_t event = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = (char)ch};
    assert(solar_os_contacts_app.event(&context, &event));
}
static void tick(void)
{
    const solar_os_event_t event = {.type = SOLAR_OS_EVENT_TICK};
    assert(solar_os_contacts_app.event(&context, &event));
}
static void select_contact(solar_os_contact_id_t id)
{
    contacts_app_back();
    for (size_t i = 0; i < state.visible_count; i++)
        if (state.contacts[state.visible[i]].id == id) { state.cursor = i; contacts_app_render(); return; }
    assert(!"contact hidden");
}

int main(void)
{
    contacts_app_state = &state;
    for (size_t i = 0; i < 3; i++) {
        records[i] = (solar_os_contact_t){.id = (uint32_t)i + 1U, .endpoint_count = 1,
            .primary_provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE};
        snprintf(records[i].display_name, sizeof(records[i].display_name), "Contact %u", (unsigned)i + 1U);
        endpoints[i] = (solar_os_endpoint_t){.id = (uint32_t)i + 11U, .contact_id = (uint32_t)i + 1U,
            .provider = SOLAR_OS_MESSAGING_PROVIDER_MESHCORE};
    }
    endpoints[3] = (solar_os_endpoint_t){.id = 14U, .contact_id = 1U,
        .trust = SOLAR_OS_CONTACT_TRUST_TRUSTED, .provider = SOLAR_OS_MESSAGING_PROVIDER_LINK};
    assert(solar_os_contacts_app.start(&context) == ESP_OK && allocations == 2U);
    assert(strstr(screen[rows - 1U], "a Actions") && strstr(screen[rows - 1U], "? Help"));
    key('/'); assert(cursor_visible && cursor_row == rows - 1U && cursor_col == 9U);
    key('C'); assert(cursor_visible && cursor_row == rows - 1U && cursor_col == 10U);
    key('\b'); assert(cursor_visible && cursor_col == 9U);
    for (size_t i = 0; i < SOLAR_OS_CONTACT_NAME_MAX; i++) key('x');
    assert(cursor_visible && cursor_row == rows - 1U && cursor_col == cols - 1U);
    key('\r'); assert(!cursor_visible);
    key('/'); assert(cursor_visible && cursor_col == cols - 1U);
    key(SOLAR_OS_KEY_ESCAPE); assert(!cursor_visible && state.search_len == 0U);
    key('?'); assert(state.mode == CONTACTS_HELP);
    key(SOLAR_OS_KEY_END); assert(state.menu.top > 0U);
    key(SOLAR_OS_KEY_ESCAPE);

    /* Menu trust applies to all endpoints; selecting one affects only its ID. */
    select_contact(1); key('a'); key('t');
    assert(state.mode == CONTACTS_ENDPOINTS && state.endpoint_count == 2U);
    key('\r'); assert(affected == 1U && affected_endpoint == 0U && affected_trust == SOLAR_OS_CONTACT_TRUST_TRUSTED);
    key('\r'); key('b'); key(SOLAR_OS_KEY_DOWN); key('\r');
    assert(affected == 1U && affected_endpoint == 11U && affected_trust == SOLAR_OS_CONTACT_TRUST_BLOCKED);
    assert(endpoints[3].trust == SOLAR_OS_CONTACT_TRUST_TRUSTED);
    key('\r');

    /* Endpoint filters match any endpoint, not just primary trust. */
    key('f'); key(SOLAR_OS_KEY_DOWN); key(SOLAR_OS_KEY_DOWN); key('\r');
    assert(state.filter == 2U && state.visible_count == 1U);
    assert(state.contacts[state.visible[0]].id == 1U);
    key('f'); key(SOLAR_OS_KEY_HOME); key('\r');
    key('s'); assert(state.mode == CONTACTS_STATUS); key(SOLAR_OS_KEY_ESCAPE);

    /* Rename is editable, cancel is inert, and failure is displayed. */
    select_contact(1); key('r'); assert(cursor_visible);
    assert(cursor_row == 1U && cursor_col == 6U + strlen(state.edit));
    key(SOLAR_OS_KEY_HOME); assert(cursor_visible && cursor_row == 1U && cursor_col == 6U);
    key(SOLAR_OS_KEY_END); assert(cursor_row == 1U && cursor_col == 6U + strlen(state.edit));
    const unsigned before_edit = mutations;
    const bool fullscreen = state.tui.fullscreen;
    key(SOLAR_OS_KEY_ALT_PREFIX); key('\r');
    assert(state.mode == CONTACTS_RENAME && mutations == before_edit && state.tui.fullscreen != fullscreen);
    key('q'); key(SOLAR_OS_KEY_ESCAPE); assert(!context.exit_requested);
    assert(!cursor_visible);
    key('r'); key(SOLAR_OS_KEY_END); key('X'); key('\r');
    assert(!strcmp(records[0].display_name, "Contact 1X") && !cursor_visible);
    key('\r'); mutation_error = ESP_ERR_NOT_FOUND; key('t'); key('\r');
    assert(state.mode == CONTACTS_NOTICE && strstr(state.notice, "NOT_FOUND"));
    mutation_error = ESP_OK; key('\r');

#if SOLAR_OS_PACKAGE_APP_CHAT
    /* Chat uses the selected contact and opens its conversation as a child. */
    const unsigned before_chat = mutations;
    select_contact(2); key('c');
    assert(direct_opens == 1U && chat_launches == 1U && chat_contact == 2U);
    assert(state.mode == CONTACTS_BROWSE && !context.exit_requested);
    solar_os_contacts_app.resume(&context);
    assert(state.contacts[state.visible[state.cursor]].id == 2U);
    key('\r'); assert(state.detail); key('c');
    assert(chat_launches == 2U && chat_contact == 2U && state.detail);
    solar_os_contacts_app.resume(&context);
    key(SOLAR_OS_KEY_ESCAPE);

    /* The action menu freezes identity through provider updates. */
    select_contact(1); key('a');
    strlcpy(records[0].display_name, "New name", sizeof(records[0].display_name));
    generation++; tick(); solar_os_contacts_app.resume(&context);
    key('\r'); assert(chat_launches == 3U && chat_contact == 1U);
    assert(state.mode == CONTACTS_BROWSE);

    direct_error = ESP_ERR_INVALID_STATE;
    key('c');
    assert(chat_launches == 3U && state.mode == CONTACTS_NOTICE);
    assert(strstr(state.notice, "Chat:"));
    direct_error = ESP_OK; key('\r');
    launch_error = ESP_ERR_NO_MEM;
    key('c'); assert(chat_launches == 4U && state.mode == CONTACTS_NOTICE);
    launch_error = ESP_OK; key('\r');

    /* The shortcut remains ordinary text when editing or searching. */
    const unsigned before_search = direct_opens;
    key('/'); key('c');
    assert(state.searching && !strcmp(state.search, "c"));
    key(SOLAR_OS_KEY_ESCAPE);
    key('r'); key('c'); assert(state.mode == CONTACTS_RENAME);
    key(SOLAR_OS_KEY_ESCAPE); assert(direct_opens == before_search);
    assert(mutations == before_chat);
#else
    /* Contacts-only firmware retains its actions without a Chat dependency. */
    select_contact(1); key('a');
    assert(contacts_actions[0][0] == 't');
    key('c'); assert(state.mode == CONTACTS_ACTIONS);
    key(SOLAR_OS_KEY_ESCAPE);
#endif
    select_contact(1);

    /* Details update after a trust change and after external provider changes. */
    key('\r'); assert(state.detail); key('t'); key(SOLAR_OS_KEY_DOWN); key('\r'); key('\r');
    assert(state.detail && state.endpoints[0].trust == SOLAR_OS_CONTACT_TRUST_TRUSTED);
    strlcpy(records[0].display_name, "Updated", sizeof(records[0].display_name));
    generation++; tick(); assert(!strcmp(state.selected.display_name, "Updated"));
    key(SOLAR_OS_KEY_ESCAPE);

    /* IDs remain frozen through a provider update and a resume while a menu is open. */
    select_contact(1); key('a');
    strlcpy(records[0].display_name, "ZZZ", sizeof(records[0].display_name));
    generation++; tick(); solar_os_contacts_app.resume(&context);
    key('t'); key('\r'); assert(affected == 1U); key('\r');

    /* Link shows all contacts despite filtering, preserves target/source order,
       and requires Y rather than Enter to move/remove the source. */
    select_contact(1); key('f'); key(SOLAR_OS_KEY_DOWN); key(SOLAR_OS_KEY_DOWN); key('\r');
    key('l'); assert(state.mode == CONTACTS_LINK && contacts_app_menu_count() == 2U);
    key('\r'); const unsigned before_link = mutations;
    key('\r'); assert(mutations == before_link); key('n'); assert(state.mode == CONTACTS_BROWSE);
    key('l'); key('\r'); key('y');
    assert(link_target == 1U && link_source == 2U && records[1].id == 0U);
    assert(endpoints[1].contact_id == 1U); key('\r');
    key('f'); key(SOLAR_OS_KEY_HOME); key('\r');

    select_contact(3); key('d'); const unsigned before_remove = mutations;
    key('\r'); key('n'); assert(mutations == before_remove && records[2].id == 3U);
    key('d'); key('y'); assert(affected == 3U && records[2].id == 0U); key('\r');

    /* Losing an open contact returns to an empty list; shortcuts stay inert. */
    select_contact(1); key('\r'); assert(state.detail);
    records[0].id = 0U;
    for (size_t i = 0; i < 4; i++) endpoints[i].id = 0U;
    generation++; tick(); assert(!state.detail && state.visible_count == 0U);
    const unsigned before_empty = mutations;
    for (const char *ch = "actbrld"; *ch; ch++) key((uint8_t)*ch);
    assert(state.mode == CONTACTS_BROWSE && mutations == before_empty);
#if SOLAR_OS_PACKAGE_APP_CHAT
    assert(direct_opens == before_search);
#endif

    /* Every screen stays inside the terminal and retains its footer at small sizes. */
    memset(state.edit, 'x', SOLAR_OS_CONTACT_NAME_MAX);
    state.edit[SOLAR_OS_CONTACT_NAME_MAX] = '\0';
    state.input = (solar_os_tui_input_state_t){.cursor = SOLAR_OS_CONTACT_NAME_MAX};
    for (rows = 4U; rows <= 20U; rows += 8U) for (cols = 20U; cols <= 100U; cols += 20U) {
        state.tui.fullscreen = rows <= 10U;
        for (contacts_app_mode_t mode = CONTACTS_BROWSE; mode <= CONTACTS_NOTICE; mode++) {
            contacts_app_mode(mode); contacts_app_render();
            if (mode == CONTACTS_RENAME)
                assert(cursor_visible && cursor_row == 1U && cursor_col < cols);
            else assert(!cursor_visible);
            bool footer = false;
            for (size_t col = 0; col < cols; col++) footer |= screen[rows - 1U][col] != ' ';
            assert(footer);
#if SOLAR_OS_PACKAGE_APP_CHAT
            if (mode == CONTACTS_BROWSE) {
                assert(strstr(screen[rows - 1U], "c Chat"));
                assert(strstr(screen[rows - 1U], "? Help"));
            }
#endif
        }
        contacts_app_mode(CONTACTS_BROWSE);
        state.searching = true;
        memset(state.search, 'x', SOLAR_OS_CONTACT_NAME_MAX);
        state.search_len = SOLAR_OS_CONTACT_NAME_MAX;
        state.search[state.search_len] = '\0';
        contacts_app_render();
        const size_t end = 9U + state.search_len;
        assert(cursor_visible && cursor_row == rows - 1U && cursor_col == (end < cols ? end : cols - 1U));
        key(SOLAR_OS_KEY_ESCAPE); assert(!cursor_visible);
    }
    rows = 4U; cols = 32U;
    contacts_app_back(); key('?'); key('q'); assert(context.exit_requested);
    solar_os_contacts_app.stop(&context); assert(allocations == 0U);
    puts("Contacts actions, endpoint scope, confirmation, filtering, refresh and layout passed");
    return 0;
}
