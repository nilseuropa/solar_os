#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "solar_os.h"
#include "solar_os_gfx.h"
#include "solar_os_http_client.h"
#include "solar_os_key_codes.h"
#include "solar_os_mjpeg.h"
#include "solar_os_text_search.h"

typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
#define SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(size)
#define SOLAR_OS_TASK_STOP_WAIT_MS 2000U
#define SOLAR_OS_TASK_ROLE_FOREGROUND 0
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY 0
#define pdPASS 1
typedef int BaseType_t;

#include "web_types.inc"

static web_state_t state;
#define web state
struct solar_os_gfx { int width, height; };
static struct solar_os_gfx canvas = {320, 240};
static solar_os_context_t context;
static web_line_t lines[20];
static web_item_t items[2];
static web_control_t controls[1];
static int cancelled, discarded, reset_count, opened, parsed, created;
static bool wifi_ready = true, worker_stops = true;
static BaseType_t create_result = pdPASS;
static web_event_t queued_event;
static bool event_queued;
static solar_os_gfx_icon_t drawn_icons[3];
static size_t icon_count;
static solar_os_gfx_font_t current_font;

static size_t strlcpy(char *out, const char *in, size_t capacity)
{
    const size_t len = strlen(in);
    if (capacity > 0) {
        const size_t copied = len < capacity - 1 ? len : capacity - 1;
        memcpy(out, in, copied);
        out[copied] = '\0';
    }
    return len;
}

solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
size_t solar_os_gfx_width(const solar_os_gfx_t *gfx) { return gfx->width; }
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx) { return gfx->height; }
void solar_os_gfx_set_color(solar_os_gfx_t *gfx, solar_os_gfx_color_t color)
{ (void)gfx; (void)color; }
void solar_os_gfx_set_font(solar_os_gfx_t *gfx, solar_os_gfx_font_t font)
{ (void)gfx; current_font = font; }
void solar_os_gfx_fill_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    assert(x >= 0 && y >= 0 && width > 0 && height > 0);
    assert(x + width <= gfx->width && y + height <= WEB_HEADER_HEIGHT);
}
void solar_os_gfx_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{ solar_os_gfx_fill_rect(gfx, x, y, width, height); }
void solar_os_gfx_line(solar_os_gfx_t *gfx, int x, int y, int x1, int y1)
{
    assert(x >= 0 && x < gfx->width && y >= 0 && y < WEB_HEADER_HEIGHT);
    assert(x1 >= 0 && x1 < gfx->width && y1 >= 0 && y1 < WEB_HEADER_HEIGHT);
}
void solar_os_gfx_icon(solar_os_gfx_t *gfx, int x, int y,
                       solar_os_gfx_icon_t icon, solar_os_gfx_icon_size_t size)
{
    assert(x >= 0 && x + (int)size <= gfx->width &&
           y >= 0 && y + (int)size <= WEB_HEADER_HEIGHT);
    assert(icon_count < 3);
    drawn_icons[icon_count++] = icon;
}
void solar_os_gfx_text(solar_os_gfx_t *gfx, int x, int y, const char *text)
{
    assert(current_font == SOLAR_OS_GFX_FONT_MONO_12);
    assert(x >= 0 && x + (int)strlen(text) * WEB_NAV_CHAR_WIDTH <= gfx->width);
    assert(y > 0 && y < WEB_HEADER_HEIGHT);
}
void solar_os_context_finish(solar_os_context_t *ctx, int code, const char *message)
{ (void)code; (void)message; ctx->exit_requested = true; }

static void web_render(solar_os_context_t *ctx) { (void)ctx; }
static void web_resume(solar_os_context_t *ctx) { (void)ctx; }
static void web_cancel_request(void) { cancelled++; }
static void web_discard_pending_mjpeg_frame(void) { discarded++; }
static void web_drain_mjpeg_frames(solar_os_context_t *ctx) { (void)ctx; }
static bool web_open_selected(solar_os_context_t *ctx)
{ (void)ctx; opened++; return true; }
static void web_select_next_item(solar_os_gfx_t *gfx, int delta)
{ (void)gfx; (void)delta; }
static bool web_handle_edit_key(uint8_t key) { (void)key; return web.editing; }
static bool web_handle_search_input(uint8_t key) { (void)key; return true; }
static bool web_find_next(void) { return true; }
static void web_update_image_line_heights(void) {}
static void web_parse_html(void)
{
    // Reflow must retain the existing controls/images rather than fetch again.
    assert(web.reflowing);
    assert(web.preserved_control_count == web.control_count);
    assert(web.preserved_image_count == web.image_count);
    parsed++;
}
static void web_reset_document(void)
{
    assert(web.task == NULL || web.task_done);
    reset_count++;
    web.line_count = 0;
    web.selected_item = -1;
    web.editing = false;
    web.mjpeg_stream = false;
}
static bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, unsigned ms)
{ (void)task; (void)ms; if (worker_stops) *done = true; return *done; }
static void web_task(void *user) { (void)user; }
static BaseType_t solar_os_task_create_pinned_external(
    void (*entry)(void *), const char *name, unsigned stack, void *user,
    unsigned priority, TaskHandle_t *task, int core, int role)
{
    (void)entry; (void)name; (void)stack; (void)user;
    (void)priority; (void)core; (void)role;
    created++;
    if (create_result == pdPASS) *task = &created;
    return create_result;
}
typedef struct { bool started, connected, has_ip; } solar_os_wifi_status_t;
static void solar_os_wifi_get_status(solar_os_wifi_status_t *status)
{ *status = (solar_os_wifi_status_t){wifi_ready, wifi_ready, wifi_ready}; }
static void xQueueReset(QueueHandle_t queue) { (void)queue; event_queued = false; }
static int xQueueReceive(QueueHandle_t queue, web_event_t *event, unsigned wait)
{
    (void)queue; (void)wait;
    if (!event_queued) return 0;
    *event = queued_event;
    event_queued = false;
    icon_count = 0;
    return pdPASS;
}

#include "web_interaction.inc"

static void reset(void)
{
    memset(&state, 0, sizeof(state));
    memset(&context, 0, sizeof(context));
    memset(lines, 0, sizeof(lines));
    canvas = (struct solar_os_gfx){320, 240};
    context.gfx = &canvas;
    web.lines = lines;
    web.items = items;
    web.controls = controls;
    web.events = &queued_event;
    web.selected_item = -1;
    web.edit_control = -1;
    web.zoom = 1;
    cancelled = discarded = opened = parsed = reset_count = created = 0;
    wifi_ready = worker_stops = true;
    event_queued = false;
    create_result = pdPASS;
    strlcpy(web.url, "http://old/", sizeof(web.url));
    strlcpy(web.address, web.url, sizeof(web.address));
}

static void key(uint8_t ch)
{
    const solar_os_event_t event = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = (char)ch};
    assert(web_event(&context, &event));
}

static void type(const char *text)
{ while (*text) key((uint8_t)*text++); }

static void pointer(int x, int y, bool relative, solar_os_input_pointer_action_t action)
{
    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_POINTER,
        .data.pointer = {
            .mode = relative ? SOLAR_OS_INPUT_POINTER_RELATIVE : SOLAR_OS_INPUT_POINTER_ABSOLUTE,
            .action = action, .buttons = SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY,
            .x = x, .y = y, .delta_x = x, .delta_y = y,
        },
    };
    assert(web_event(&context, &event));
}

static void done(void)
{
    web.task_done = true;
    queued_event = (web_event_t){.type = WEB_EVENT_DONE, .status_code = 200};
    event_queued = true;
    const solar_os_event_t tick = {.type = SOLAR_OS_EVENT_TICK};
    assert(web_event(&context, &tick));
}

static void test_address(void)
{
    reset();
    key(0x0c); // Ctrl+L selects the current URL.
    assert(web.address_editing && web.address_selected);
    type("camera.local:8080/page");
    assert(strcmp(web.address, "camera.local:8080/page") == 0);
    key(SOLAR_OS_KEY_HOME);
    type("X");
    key('\b');
    key(SOLAR_OS_KEY_END);
    key('\b');
    type("e");
    key('\r');
    assert(strcmp(web.url, "http://camera.local:8080/page") == 0);
    assert(web.loading && !web.address_editing);
    done();
    key(0x0c);
    key(SOLAR_OS_KEY_DELETE);
    assert(web.address[0] == '\0');
    type(" https://example.org/  ");
    key('\r');
    assert(strcmp(web.url, "https://example.org/") == 0);
    done();
    key(0x0c);
    type("ftp://example.org/");
    key('\r');
    assert(web.address_editing && strstr(web.status, "http://") != NULL);
    key(SOLAR_OS_KEY_ESCAPE);
    assert(!context.exit_requested && !web.address_editing);
    assert(strcmp(web.address, web.url) == 0);
    key(0x0c);
    key(0x15); // Ctrl+U
    key('\r');
    assert(strstr(web.status, "enter an address") != NULL);
    for (int i = 0; i < WEB_URL_MAX + 20; i++) key('a');
    assert(strlen(web.address) == WEB_URL_MAX - 1);
    key('\r');
    assert(strstr(web.status, "too long") != NULL);
    key(SOLAR_OS_KEY_APP_EXIT);
    assert(context.exit_requested);
}

static void test_toolbar_and_links(void)
{
    const int widths[] = {128, 160, 240, 320, 480, 800};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        reset();
        canvas.width = widths[i];
        const web_toolbar_layout_t layout = web_toolbar_layout(canvas.width);
        assert(layout.address_width >= 4 * WEB_NAV_CHAR_WIDTH);
        assert(layout.go_x + layout.button_width <= canvas.width - WEB_NAV_MARGIN);
        web_draw_toolbar(&canvas, canvas.width);
        assert(drawn_icons[0] == SOLAR_OS_GFX_ICON_ARROW_LEFT);
        assert(drawn_icons[1] == SOLAR_OS_GFX_ICON_ARROW_RIGHT);
        assert(drawn_icons[2] == SOLAR_OS_GFX_ICON_RELOAD);
        pointer(layout.address_x + 4, 12, false, SOLAR_OS_INPUT_POINTER_PRESS);
        assert(web.address_editing && web.address_selected);
        icon_count = 0;
        web_draw_toolbar(&canvas, canvas.width);
        type("host/");
        icon_count = 0;
        web_draw_toolbar(&canvas, canvas.width);
        pointer(layout.go_x + 5, 12, false, SOLAR_OS_INPUT_POINTER_PRESS);
        assert(strcmp(web.url, "http://host/") == 0 && web.loading);
        icon_count = 0;
        web_draw_toolbar(&canvas, canvas.width);
        assert(drawn_icons[2] == SOLAR_OS_GFX_ICON_MEDIA_STOP);
        done();
    }
    reset();
    web.loaded = true;
    web.line_count = 2;
    web.item_count = 1;
    lines[0] = (web_line_t){.height = 16, .link_index = 0, .control_index = -1,
                           .image_index = -1};
    strlcpy(lines[0].text, "link", sizeof(lines[0].text));
    lines[1] = (web_line_t){.height = 200, .link_index = 0, .control_index = -1};
    items[0] = (web_item_t){.type = WEB_ITEM_LINK, .index = 0};
    pointer(200, WEB_HEADER_HEIGHT + 8, false, SOLAR_OS_INPUT_POINTER_PRESS);
    assert(opened == 0 && web.selected_item == -1); // Blank area is not a link.
    pointer(WEB_MARGIN_X + 4, WEB_HEADER_HEIGHT + 8, false, SOLAR_OS_INPUT_POINTER_MOVE);
    assert(web.selected_item == 0 && !web.pointer_visible && opened == 0);
    pointer(0, 0, true, SOLAR_OS_INPUT_POINTER_PRESS);
    assert(web.pointer_visible && opened == 1);
    pointer(10, WEB_HEADER_HEIGHT + 30, false, SOLAR_OS_INPUT_POINTER_PRESS);
    assert(opened == 1); // A clipped, unrendered row must not be clickable.
    pointer(30000, -30000, true, SOLAR_OS_INPUT_POINTER_MOVE);
    assert(web.pointer_x == canvas.width - 1 && web.pointer_y == 0);
}

static void test_stop_and_history(void)
{
    reset();
    web.loaded = true;
    assert(web_start_load(&context, "http://new/", true) == ESP_OK);
    assert(web.back_history_count == 1);
    const web_toolbar_layout_t layout = web_toolbar_layout(canvas.width);
    pointer(WEB_NAV_MARGIN * 3 + layout.button_width * 2 + 8, 12,
            false, SOLAR_OS_INPUT_POINTER_PRESS);
    assert(web.stop_requested && web.stopping && cancelled == 1);
    const int resets_before = reset_count;
    web_drain_events(&context);
    assert(reset_count == resets_before && web.loading); // Worker still owns data.
    done(); // A queued late DONE must not resurrect a cancelled page.
    assert(!web.loaded && !web.loading && !web.stopping && !event_queued);
    assert(strcmp(web.status, "stopped") == 0 && web.task == NULL);
    pointer(WEB_NAV_MARGIN + 8, 12, false, SOLAR_OS_INPUT_POINTER_PRESS);
    assert(strcmp(web.url, "http://old/") == 0 && web.forward_history_count == 1);
    done();
    pointer(WEB_NAV_MARGIN * 2 + layout.button_width + 8, 12,
            false, SOLAR_OS_INPUT_POINTER_PRESS);
    assert(strcmp(web.url, "http://new/") == 0 && web.back_history_count == 1);
    done();
    pointer(WEB_NAV_MARGIN * 3 + layout.button_width * 2 + 8, 12,
            false, SOLAR_OS_INPUT_POINTER_PRESS);
    assert(web.loading && web.restore_pending && web.back_history_count == 1);
    done();
    wifi_ready = false;
    key(SOLAR_OS_KEY_LEFT);
    assert(web.back_history_count == 1 && web.forward_history_count == 0);
    assert(strcmp(web.url, "http://new/") == 0 && web.loaded);
    wifi_ready = true;
    web.task_done = false;
    web.task = &created;
    worker_stops = false;
    key(SOLAR_OS_KEY_LEFT);
    assert(web.back_history_count == 1 && web.forward_history_count == 0);
    assert(strstr(web.status, "timeout") != NULL);
    assert(web.stopping);
    done();
    assert(!web.stopping && web.task == NULL);

    reset();
    web.mjpeg_stream = web.loaded = true;
    web.task = &created;
    web.line_count = 1;
    key('r');
    assert(web.stopping && web.stop_requested);
    done();
    assert(web.loaded && web.line_count == 1 && reset_count == 0);
    assert(!web_request_active()); // Last video frame survives Stop.
}

static void test_zoom_and_focus(void)
{
    reset();
    web.loaded = true;
    web.control_count = 1;
    web.image_count = 1;
    web.editing = true;
    web.edit_control = 0;
    strlcpy(controls[0].value, "field", sizeof(controls[0].value));
    key(SOLAR_OS_KEY_CTRL_PLUS);
    assert(web.zoom == 2 && parsed == 1 && web.editing);
    key(SOLAR_OS_KEY_CTRL_MINUS);
    assert(web.zoom == 1 && parsed == 2);
    assert(strcmp(controls[0].value, "field") == 0 && created == 0);
    key(0x0c);
    assert(web.address_editing && !web.editing);
    key(SOLAR_OS_KEY_CTRL_PLUS);
    assert(web.zoom == 2 && parsed == 3 && web.address_editing);
    for (int i = 0; i < 10; i++) key(SOLAR_OS_KEY_CTRL_PLUS);
    assert(web.zoom == WEB_MAX_ZOOM);
    for (int i = 0; i < 10; i++) key(SOLAR_OS_KEY_CTRL_MINUS);
    assert(web.zoom == 0);
    assert(strcmp(web.address, "http://old/") == 0);
}

int main(void)
{
    test_address();
    test_toolbar_and_links();
    test_stop_and_history();
    test_zoom_and_focus();
    return 0;
}
