#include "solar_os_web.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_log.h"
#include "solar_os_stb_image.h"
#include "solar_os_task.h"
#include "solar_os_webp_decoder.h"
#include "solar_os_wifi.h"

#define WEB_URL_MAX 256
#define WEB_STATUS_MAX 96
#define WEB_HTML_MAX (96U * 1024U)
#define WEB_LINE_COUNT 768
#define WEB_LINE_MAX 96
#define WEB_LINK_COUNT 64
#define WEB_LINK_TITLE_MAX 64
#define WEB_CONTROL_COUNT 32
#define WEB_CONTROL_NAME_MAX 48
#define WEB_CONTROL_VALUE_MAX 96
#define WEB_FORM_COUNT 8
#define WEB_IMAGE_COUNT 8
#define WEB_IMAGE_MAX_BYTES (512U * 1024U)
#define WEB_IMAGE_MAX_PIXELS (480U * 320U)
#define WEB_IMAGE_MAX_HEIGHT 112
#define WEB_ITEM_COUNT (WEB_LINK_COUNT + WEB_CONTROL_COUNT)
#define WEB_HISTORY_COUNT 8
#define WEB_TAG_MAX 256
#define WEB_TASK_STACK 24576
#define WEB_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define WEB_EVENT_QUEUE_LEN 8
#define WEB_TIMEOUT_MS 12000
#define WEB_REDIRECT_MAX 5
#define WEB_MARGIN_X 5
#define WEB_HEADER_HEIGHT 18
#define WEB_FOOTER_HEIGHT 12
#define WEB_LINE_HEIGHT 11
#define WEB_CONTROL_HEIGHT 22
#define WEB_CONTROL_FIELD_HEIGHT 16
#define WEB_CONTROL_BOX_SIZE 11
#define WEB_TEXT_BASELINE 9

typedef enum {
    WEB_LINE_NORMAL,
    WEB_LINE_HEADING,
    WEB_LINE_LINK,
    WEB_LINE_CONTROL,
    WEB_LINE_IMAGE,
    WEB_LINE_STATUS,
} web_line_style_t;

typedef struct {
    char text[WEB_LINE_MAX];
    int16_t link_index;
    int16_t control_index;
    int16_t image_index;
    uint16_t height;
    uint8_t style;
} web_line_t;

typedef struct {
    char href[WEB_URL_MAX];
    char title[WEB_LINK_TITLE_MAX];
} web_link_t;

typedef enum {
    WEB_CONTROL_TEXT,
    WEB_CONTROL_CHECKBOX,
    WEB_CONTROL_RADIO,
    WEB_CONTROL_SUBMIT,
    WEB_CONTROL_HIDDEN,
} web_control_type_t;

typedef struct {
    web_control_type_t type;
    int16_t form_index;
    bool checked;
    bool visible;
    char name[WEB_CONTROL_NAME_MAX];
    char value[WEB_CONTROL_VALUE_MAX];
    char label[WEB_CONTROL_VALUE_MAX];
} web_control_t;

typedef struct {
    char action[WEB_URL_MAX];
    char method[8];
} web_form_t;

typedef enum {
    WEB_IMAGE_DECODE_NONE,
    WEB_IMAGE_DECODE_STB,
    WEB_IMAGE_DECODE_WEBP,
} web_image_decoder_t;

typedef struct {
    char src[WEB_URL_MAX];
    char alt[WEB_LINK_TITLE_MAX];
    uint8_t *gray;
    uint32_t width;
    uint32_t height;
    uint16_t draw_width;
    uint16_t draw_height;
    bool attempted;
    bool loaded;
    bool truncated;
    int status_code;
    web_image_decoder_t decoder;
} web_image_t;

typedef enum {
    WEB_ITEM_LINK,
    WEB_ITEM_CONTROL,
} web_item_type_t;

typedef struct {
    web_item_type_t type;
    int16_t index;
    int16_t line_index;
} web_item_t;

typedef enum {
    WEB_EVENT_STATUS,
    WEB_EVENT_ERROR,
    WEB_EVENT_DONE,
} web_event_type_t;

typedef struct {
    web_event_type_t type;
    int status_code;
    uint32_t bytes_read;
    bool truncated;
    char message[WEB_STATUS_MAX];
} web_event_t;

typedef struct {
    bool active;
    bool loading;
    bool loaded;
    bool redraw;
    bool html_truncated;
    volatile bool stop_requested;
    volatile bool task_done;
    TaskHandle_t task;
    QueueHandle_t events;
    esp_http_client_handle_t client;
    uint8_t *html;
    size_t html_len;
    web_line_t *lines;
    size_t line_count;
    web_link_t *links;
    size_t link_count;
    web_control_t *controls;
    size_t control_count;
    web_form_t *forms;
    size_t form_count;
    web_image_t *images;
    size_t image_count;
    web_item_t *items;
    size_t item_count;
    int scroll;
    int selected_item;
    bool editing;
    int edit_control;
    size_t edit_cursor;
    char edit_original[WEB_CONTROL_VALUE_MAX];
    int status_code;
    uint32_t bytes_read;
    size_t wrap_cols;
    char history[WEB_HISTORY_COUNT][WEB_URL_MAX];
    size_t history_count;
    char url[WEB_URL_MAX];
    char base_url[WEB_URL_MAX];
    char status[WEB_STATUS_MAX];
} web_state_t;

static const char *TAG = "solar_os_web";
static web_state_t web;

static bool web_resolve_url(const char *base, const char *href, char *out, size_t out_len);
static const char *web_current_base_url(void);

static void *web_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *web_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void web_free_image_data(web_image_t *image)
{
    if (image == NULL || image->gray == NULL) {
        return;
    }

    switch (image->decoder) {
    case WEB_IMAGE_DECODE_WEBP:
        solar_os_webp_free(image->gray);
        break;
    case WEB_IMAGE_DECODE_STB:
    case WEB_IMAGE_DECODE_NONE:
    default:
        solar_os_stb_image_free(image->gray);
        break;
    }

    image->gray = NULL;
    image->width = 0;
    image->height = 0;
    image->draw_width = 0;
    image->draw_height = 0;
    image->loaded = false;
    image->decoder = WEB_IMAGE_DECODE_NONE;
}

static bool web_url_supported(const char *url)
{
    return url != NULL &&
        (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0);
}

static bool web_is_printable(char ch)
{
    return (unsigned char)ch >= 0x20U && (unsigned char)ch < 0x7fU;
}

static void web_init_line(web_line_t *line, web_line_style_t style)
{
    if (line == NULL) {
        return;
    }

    memset(line, 0, sizeof(*line));
    line->link_index = -1;
    line->control_index = -1;
    line->image_index = -1;
    line->height = WEB_LINE_HEIGHT;
    line->style = (uint8_t)style;
}

static void web_set_status(const char *status)
{
    strlcpy(web.status, status != NULL ? status : "", sizeof(web.status));
    web.redraw = true;
}

static bool web_send_event(const web_event_t *event)
{
    if (event == NULL || web.events == NULL) {
        return false;
    }
    while (!web.stop_requested) {
        if (xQueueSend(web.events, event, pdMS_TO_TICKS(100)) == pdPASS) {
            return true;
        }
    }
    return false;
}

static void web_send_message(web_event_type_t type, const char *message)
{
    web_event_t event = {
        .type = type,
    };
    if (message != NULL) {
        strlcpy(event.message, message, sizeof(event.message));
    }
    (void)web_send_event(&event);
}

static void web_reset_document(void)
{
    web.html_len = 0;
    web.html_truncated = false;
    web.line_count = 0;
    web.link_count = 0;
    web.control_count = 0;
    web.form_count = 0;
    web.image_count = 0;
    web.item_count = 0;
    web.scroll = 0;
    web.selected_item = -1;
    web.editing = false;
    web.edit_control = -1;
    web.edit_cursor = 0;
    web.edit_original[0] = '\0';
    web.status_code = -1;
    web.bytes_read = 0;
    web.base_url[0] = '\0';

    if (web.html != NULL) {
        web.html[0] = '\0';
    }
    if (web.lines != NULL) {
        memset(web.lines, 0, sizeof(web.lines[0]) * WEB_LINE_COUNT);
    }
    if (web.links != NULL) {
        memset(web.links, 0, sizeof(web.links[0]) * WEB_LINK_COUNT);
    }
    if (web.controls != NULL) {
        memset(web.controls, 0, sizeof(web.controls[0]) * WEB_CONTROL_COUNT);
    }
    if (web.forms != NULL) {
        memset(web.forms, 0, sizeof(web.forms[0]) * WEB_FORM_COUNT);
    }
    if (web.items != NULL) {
        memset(web.items, 0, sizeof(web.items[0]) * WEB_ITEM_COUNT);
    }
    if (web.images != NULL) {
        for (size_t i = 0; i < WEB_IMAGE_COUNT; i++) {
            web_free_image_data(&web.images[i]);
        }
        memset(web.images, 0, sizeof(web.images[0]) * WEB_IMAGE_COUNT);
    }
}

static bool web_allocate_buffers(void)
{
    web.html = web_malloc(WEB_HTML_MAX + 1U);
    web.lines = web_calloc(WEB_LINE_COUNT, sizeof(web.lines[0]));
    web.links = web_calloc(WEB_LINK_COUNT, sizeof(web.links[0]));
    web.controls = web_calloc(WEB_CONTROL_COUNT, sizeof(web.controls[0]));
    web.forms = web_calloc(WEB_FORM_COUNT, sizeof(web.forms[0]));
    web.images = web_calloc(WEB_IMAGE_COUNT, sizeof(web.images[0]));
    web.items = web_calloc(WEB_ITEM_COUNT, sizeof(web.items[0]));
    web.events = xQueueCreate(WEB_EVENT_QUEUE_LEN, sizeof(web_event_t));

    if (web.html == NULL ||
        web.lines == NULL ||
        web.links == NULL ||
        web.controls == NULL ||
        web.forms == NULL ||
        web.images == NULL ||
        web.items == NULL ||
        web.events == NULL) {
        return false;
    }
    web_reset_document();
    return true;
}

static void web_free_buffers(void)
{
    if (web.events != NULL) {
        vQueueDelete(web.events);
        web.events = NULL;
    }
    if (web.html != NULL) {
        heap_caps_free(web.html);
        web.html = NULL;
    }
    if (web.lines != NULL) {
        heap_caps_free(web.lines);
        web.lines = NULL;
    }
    if (web.links != NULL) {
        heap_caps_free(web.links);
        web.links = NULL;
    }
    if (web.controls != NULL) {
        heap_caps_free(web.controls);
        web.controls = NULL;
    }
    if (web.forms != NULL) {
        heap_caps_free(web.forms);
        web.forms = NULL;
    }
    if (web.images != NULL) {
        for (size_t i = 0; i < WEB_IMAGE_COUNT; i++) {
            web_free_image_data(&web.images[i]);
        }
        heap_caps_free(web.images);
        web.images = NULL;
    }
    if (web.items != NULL) {
        heap_caps_free(web.items);
        web.items = NULL;
    }
}

static bool web_append_html(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || web.html == NULL) {
        return true;
    }

    web.bytes_read += (uint32_t)len;
    const size_t remaining = WEB_HTML_MAX - web.html_len;
    const size_t copy_len = len < remaining ? len : remaining;
    if (copy_len > 0) {
        memcpy(web.html + web.html_len, data, copy_len);
        web.html_len += copy_len;
        web.html[web.html_len] = '\0';
    }
    if (copy_len < len) {
        web.html_truncated = true;
    }
    return true;
}

static bool web_http_event_is_redirect_body(esp_http_client_event_t *event)
{
    if (event == NULL || event->client == NULL) {
        return false;
    }

    const int status = esp_http_client_get_status_code(event->client);
    return status >= 300 && status < 400;
}

static esp_err_t web_http_event(esp_http_client_event_t *event)
{
    if (event == NULL) {
        return ESP_OK;
    }
    if (web.stop_requested) {
        return ESP_FAIL;
    }

    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (web_http_event_is_redirect_body(event)) {
            return ESP_OK;
        }
        web_append_html((const uint8_t *)event->data, (size_t)event->data_len);
    }
    return ESP_OK;
}

static bool web_line_empty(void)
{
    if (web.line_count == 0) {
        return true;
    }
    const web_line_t *line = &web.lines[web.line_count - 1U];
    return line->text[0] == '\0' &&
        line->link_index < 0 &&
        line->control_index < 0 &&
        line->image_index < 0;
}

static web_line_t *web_current_line(web_line_style_t style)
{
    if (web.lines == NULL) {
        return NULL;
    }
    if (web.line_count == 0) {
        web.line_count = 1;
        web_init_line(&web.lines[0], style);
    }
    return &web.lines[web.line_count - 1U];
}

static void web_newline(web_line_style_t style)
{
    if (web.lines == NULL || web.line_count >= WEB_LINE_COUNT) {
        return;
    }
    if (web.line_count == 0) {
        web.line_count = 1;
        web_init_line(&web.lines[0], style);
        return;
    }
    if (web_line_empty()) {
        web.lines[web.line_count - 1U].style = (uint8_t)style;
        return;
    }

    web_line_t *line = &web.lines[web.line_count++];
    web_init_line(line, style);
}

static void web_append_char(char ch, int current_link, web_line_style_t style)
{
    if (ch == '\0' || web.lines == NULL || web.line_count >= WEB_LINE_COUNT) {
        return;
    }
    if (ch == '\n' || ch == '\r') {
        web_newline(style);
        return;
    }

    web_line_t *line = web_current_line(style);
    if (line == NULL) {
        return;
    }

    size_t len = strlen(line->text);
    if ((ch == ' ' || ch == '\t') &&
        (len == 0 || line->text[len - 1U] == ' ')) {
        return;
    }
    if (ch == '\t') {
        ch = ' ';
    }

    const size_t wrap_cols = web.wrap_cols > 8U && web.wrap_cols < WEB_LINE_MAX ?
        web.wrap_cols : WEB_LINE_MAX - 1U;
    if (len + 1U > wrap_cols) {
        char carry[WEB_LINE_MAX];
        carry[0] = '\0';
        char *last_space = strrchr(line->text, ' ');
        if (last_space != NULL && last_space > line->text) {
            strlcpy(carry, last_space + 1, sizeof(carry));
            *last_space = '\0';
        }

        web_newline(style);
        line = web_current_line(style);
        if (line == NULL) {
            return;
        }
        if (current_link >= 0) {
            line->link_index = (int16_t)current_link;
            line->style = WEB_LINE_LINK;
        }
        if (carry[0] != '\0') {
            strlcpy(line->text, carry, sizeof(line->text));
        }
        len = strlen(line->text);
        if (ch == ' ') {
            return;
        }
    }

    line->text[len++] = ch;
    line->text[len] = '\0';
    if (current_link >= 0 && line->link_index < 0) {
        line->link_index = (int16_t)current_link;
        line->style = WEB_LINE_LINK;
    } else if (line->style == WEB_LINE_NORMAL && style != WEB_LINE_NORMAL) {
        line->style = (uint8_t)style;
    }
}

static void web_append_text(const char *text, int current_link, web_line_style_t style)
{
    if (text == NULL) {
        return;
    }
    for (const char *p = text; *p != '\0'; p++) {
        web_append_char(*p, current_link, style);
    }
}

static char web_entity_char(const char *entity, size_t len)
{
    if (len == 2 && strncasecmp(entity, "lt", len) == 0) {
        return '<';
    }
    if (len == 2 && strncasecmp(entity, "gt", len) == 0) {
        return '>';
    }
    if (len == 3 && strncasecmp(entity, "amp", len) == 0) {
        return '&';
    }
    if (len == 4 && strncasecmp(entity, "quot", len) == 0) {
        return '"';
    }
    if (len == 4 && strncasecmp(entity, "nbsp", len) == 0) {
        return ' ';
    }
    if (len > 1 && entity[0] == '#') {
        int value = 0;
        if (entity[1] == 'x' || entity[1] == 'X') {
            for (size_t i = 2; i < len; i++) {
                const char ch = entity[i];
                if (ch >= '0' && ch <= '9') {
                    value = value * 16 + ch - '0';
                } else if (ch >= 'a' && ch <= 'f') {
                    value = value * 16 + ch - 'a' + 10;
                } else if (ch >= 'A' && ch <= 'F') {
                    value = value * 16 + ch - 'A' + 10;
                } else {
                    return '?';
                }
            }
        } else {
            for (size_t i = 1; i < len; i++) {
                if (!isdigit((unsigned char)entity[i])) {
                    return '?';
                }
                value = value * 10 + entity[i] - '0';
            }
        }
        return value >= 32 && value < 127 ? (char)value : '?';
    }
    return '?';
}

static void web_append_entity(const char *html,
                              size_t len,
                              size_t *index,
                              int current_link,
                              web_line_style_t style)
{
    const size_t start = *index + 1U;
    size_t end = start;
    while (end < len && end - start < 12U && html[end] != ';' && html[end] != '<') {
        end++;
    }
    if (end < len && html[end] == ';') {
        web_append_char(web_entity_char(html + start, end - start), current_link, style);
        *index = end;
    } else {
        web_append_char('&', current_link, style);
    }
}

static void web_copy_lower_name(const char *tag, char *name, size_t name_len)
{
    if (name == NULL || name_len == 0) {
        return;
    }
    name[0] = '\0';
    if (tag == NULL) {
        return;
    }

    while (*tag != '\0' && isspace((unsigned char)*tag)) {
        tag++;
    }
    if (*tag == '/') {
        tag++;
    }
    while (*tag != '\0' && isspace((unsigned char)*tag)) {
        tag++;
    }

    size_t used = 0;
    while (*tag != '\0' &&
           !isspace((unsigned char)*tag) &&
           *tag != '/' &&
           *tag != '>' &&
           used + 1U < name_len) {
        name[used++] = (char)tolower((unsigned char)*tag++);
    }
    name[used] = '\0';
}

static const char *web_find_attr(const char *tag, const char *attr)
{
    const size_t attr_len = strlen(attr);
    for (const char *p = tag; *p != '\0'; p++) {
        if ((p == tag || isspace((unsigned char)p[-1]) || p[-1] == '/') &&
            strncasecmp(p, attr, attr_len) == 0) {
            const char *q = p + attr_len;
            while (*q != '\0' && isspace((unsigned char)*q)) {
                q++;
            }
            if (*q == '=') {
                return q + 1;
            }
        }
    }
    return NULL;
}

static bool web_tag_attr(const char *tag, const char *attr, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    const char *p = web_find_attr(tag, attr);
    if (p == NULL) {
        return false;
    }
    while (*p != '\0' && isspace((unsigned char)*p)) {
        p++;
    }

    char quote = '\0';
    if (*p == '"' || *p == '\'') {
        quote = *p++;
    }

    size_t used = 0;
    while (*p != '\0' && used + 1U < out_len) {
        if (quote != '\0') {
            if (*p == quote) {
                break;
            }
        } else if (isspace((unsigned char)*p) || *p == '>') {
            break;
        }
        out[used++] = *p++;
    }
    out[used] = '\0';
    return used > 0;
}

static bool web_tag_has_attr(const char *tag, const char *attr)
{
    if (tag == NULL || attr == NULL) {
        return false;
    }

    const size_t attr_len = strlen(attr);
    for (const char *p = tag; *p != '\0'; p++) {
        if ((p == tag || isspace((unsigned char)p[-1]) || p[-1] == '/') &&
            strncasecmp(p, attr, attr_len) == 0) {
            const char *q = p + attr_len;
            if (*q == '\0' || isspace((unsigned char)*q) || *q == '=' || *q == '/' || *q == '>') {
                return true;
            }
        }
    }
    return false;
}

static bool web_srcset_first_url(const char *srcset, char *out, size_t out_len)
{
    if (srcset == NULL || out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    while (*srcset != '\0' && isspace((unsigned char)*srcset)) {
        srcset++;
    }
    const char *end = srcset;
    while (*end != '\0' && *end != ',' && !isspace((unsigned char)*end)) {
        end++;
    }
    const size_t len = (size_t)(end - srcset);
    if (len == 0 || len + 1U > out_len) {
        return false;
    }

    memcpy(out, srcset, len);
    out[len] = '\0';
    return true;
}

static bool web_tag_image_src(const char *tag, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    static const char *const attrs[] = {
        "src",
        "data-src",
        "data-original",
        "data-lazy-src",
    };
    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
        if (web_tag_attr(tag, attrs[i], out, out_len)) {
            return true;
        }
    }

    char srcset[WEB_URL_MAX];
    if (web_tag_attr(tag, "srcset", srcset, sizeof(srcset))) {
        return web_srcset_first_url(srcset, out, out_len);
    }
    return false;
}

static void web_lower_ascii(char *text)
{
    if (text == NULL) {
        return;
    }
    for (; *text != '\0'; text++) {
        *text = (char)tolower((unsigned char)*text);
    }
}

static int web_add_link(const char *href)
{
    if (href == NULL || href[0] == '\0' || web.links == NULL ||
        web.link_count >= WEB_LINK_COUNT) {
        return -1;
    }
    if (href[0] == '#') {
        return -1;
    }

    const int index = (int)web.link_count++;
    strlcpy(web.links[index].href, href, sizeof(web.links[index].href));
    snprintf(web.links[index].title, sizeof(web.links[index].title), "%d", index + 1);
    return index;
}

static int web_add_form(const char *action, const char *method)
{
    if (web.forms == NULL || web.form_count >= WEB_FORM_COUNT) {
        return -1;
    }

    const int index = (int)web.form_count++;
    web.forms[index].action[0] = '\0';
    if (action != NULL) {
        strlcpy(web.forms[index].action, action, sizeof(web.forms[index].action));
    }
    strlcpy(web.forms[index].method,
            method != NULL && method[0] != '\0' ? method : "get",
            sizeof(web.forms[index].method));
    web_lower_ascii(web.forms[index].method);
    return index;
}

static int web_add_control(web_control_type_t type,
                           int form_index,
                           const char *name,
                           const char *value,
                           const char *label,
                           bool checked,
                           bool visible)
{
    if (web.controls == NULL || web.control_count >= WEB_CONTROL_COUNT) {
        return -1;
    }

    const int index = (int)web.control_count++;
    web_control_t *control = &web.controls[index];
    memset(control, 0, sizeof(*control));
    control->type = type;
    control->form_index = (int16_t)form_index;
    control->checked = checked;
    control->visible = visible;
    if (name != NULL) {
        strlcpy(control->name, name, sizeof(control->name));
    }
    if (value != NULL) {
        strlcpy(control->value, value, sizeof(control->value));
    }
    if (label != NULL && label[0] != '\0') {
        strlcpy(control->label, label, sizeof(control->label));
    } else if (name != NULL && name[0] != '\0') {
        strlcpy(control->label, name, sizeof(control->label));
    } else if (type == WEB_CONTROL_SUBMIT) {
        strlcpy(control->label, "submit", sizeof(control->label));
    }

    return index;
}

static void web_add_control_line(int control_index)
{
    if (control_index < 0 || web.lines == NULL || web.line_count >= WEB_LINE_COUNT) {
        return;
    }

    web_newline(WEB_LINE_CONTROL);
    web_line_t *line = web_current_line(WEB_LINE_CONTROL);
    if (line == NULL) {
        return;
    }
    line->control_index = (int16_t)control_index;
    line->style = WEB_LINE_CONTROL;
    line->height = WEB_CONTROL_HEIGHT;
    web_newline(WEB_LINE_NORMAL);
}

static int web_add_image(const char *src, const char *alt)
{
    if (src == NULL || src[0] == '\0' || web.images == NULL ||
        web.image_count >= WEB_IMAGE_COUNT) {
        return -1;
    }

    const int index = (int)web.image_count++;
    web_image_t *image = &web.images[index];
    memset(image, 0, sizeof(*image));
    strlcpy(image->src, src, sizeof(image->src));
    if (alt != NULL) {
        strlcpy(image->alt, alt, sizeof(image->alt));
    }
    image->status_code = -1;
    return index;
}

static void web_add_image_line(int image_index, int current_link)
{
    if (image_index < 0 || web.lines == NULL || web.line_count >= WEB_LINE_COUNT) {
        return;
    }

    web_newline(WEB_LINE_IMAGE);
    web_line_t *line = web_current_line(WEB_LINE_IMAGE);
    if (line == NULL) {
        return;
    }
    line->image_index = (int16_t)image_index;
    line->link_index = (int16_t)current_link;
    line->style = WEB_LINE_IMAGE;
    line->height = 22;

    const web_image_t *image = &web.images[image_index];
    if (image->alt[0] != '\0') {
        snprintf(line->text, sizeof(line->text), "[image: %s]", image->alt);
    } else {
        strlcpy(line->text, "[image]", sizeof(line->text));
    }
    web_newline(WEB_LINE_NORMAL);
}

static bool web_item_seen(web_item_type_t type, int index)
{
    for (size_t i = 0; i < web.item_count; i++) {
        if (web.items[i].type == type && web.items[i].index == index) {
            return true;
        }
    }
    return false;
}

static void web_rebuild_items(void)
{
    if (web.items == NULL || web.lines == NULL) {
        return;
    }

    web.item_count = 0;
    memset(web.items, 0, sizeof(web.items[0]) * WEB_ITEM_COUNT);
    for (size_t i = 0; i < web.line_count && web.item_count < WEB_ITEM_COUNT; i++) {
        const web_line_t *line = &web.lines[i];
        if (line->link_index >= 0 && !web_item_seen(WEB_ITEM_LINK, line->link_index)) {
            web.items[web.item_count++] = (web_item_t) {
                .type = WEB_ITEM_LINK,
                .index = line->link_index,
                .line_index = (int16_t)i,
            };
        }
        if (line->control_index >= 0 && !web_item_seen(WEB_ITEM_CONTROL, line->control_index)) {
            web.items[web.item_count++] = (web_item_t) {
                .type = WEB_ITEM_CONTROL,
                .index = line->control_index,
                .line_index = (int16_t)i,
            };
        }
    }
}

static bool web_line_selected(const web_line_t *line)
{
    if (line == NULL || web.selected_item < 0 || web.selected_item >= (int)web.item_count) {
        return false;
    }
    const web_item_t *item = &web.items[web.selected_item];
    if (item->type == WEB_ITEM_LINK) {
        return line->link_index == item->index;
    }
    return line->control_index == item->index;
}

static bool web_tag_is_block(const char *name)
{
    return strcmp(name, "p") == 0 ||
        strcmp(name, "div") == 0 ||
        strcmp(name, "section") == 0 ||
        strcmp(name, "article") == 0 ||
        strcmp(name, "header") == 0 ||
        strcmp(name, "footer") == 0 ||
        strcmp(name, "tr") == 0 ||
        strcmp(name, "table") == 0 ||
        strcmp(name, "form") == 0 ||
        strcmp(name, "blockquote") == 0;
}

static bool web_tag_is_heading(const char *name)
{
    return strcmp(name, "h1") == 0 ||
        strcmp(name, "h2") == 0 ||
        strcmp(name, "h3") == 0 ||
        strcmp(name, "h4") == 0 ||
        strcmp(name, "h5") == 0 ||
        strcmp(name, "h6") == 0 ||
        strcmp(name, "title") == 0;
}

static void web_skip_element(const char *html, size_t len, size_t *index, const char *name)
{
    char needle[24];
    snprintf(needle, sizeof(needle), "</%s", name);

    for (size_t i = *index + 1U; i + strlen(needle) < len; i++) {
        if (html[i] == '<' && strncasecmp(html + i, needle, strlen(needle)) == 0) {
            while (i < len && html[i] != '>') {
                i++;
            }
            *index = i < len ? i : len - 1U;
            return;
        }
    }
    *index = len - 1U;
}

static void web_parse_html(void)
{
    if (web.html == NULL || web.lines == NULL || web.links == NULL) {
        return;
    }

    web.line_count = 0;
    web.link_count = 0;
    web.control_count = 0;
    web.form_count = 0;
    web.image_count = 0;
    web.item_count = 0;
    int current_link = -1;
    int current_form = -1;
    web_line_style_t current_style = WEB_LINE_NORMAL;

    const char *html = (const char *)web.html;
    const size_t len = web.html_len;
    bool last_was_space = true;

    for (size_t i = 0; i < len && !web.stop_requested; i++) {
        const char ch = html[i];
        if (ch == '<') {
            size_t end = i + 1U;
            bool quoted = false;
            char quote = '\0';
            while (end < len) {
                const char tag_ch = html[end];
                if (quoted) {
                    if (tag_ch == quote) {
                        quoted = false;
                    }
                } else if (tag_ch == '"' || tag_ch == '\'') {
                    quoted = true;
                    quote = tag_ch;
                } else if (tag_ch == '>') {
                    break;
                }
                end++;
            }
            if (end >= len) {
                break;
            }

            char tag[WEB_TAG_MAX];
            const size_t tag_len = end - i - 1U;
            const size_t copy_len = tag_len < sizeof(tag) - 1U ? tag_len : sizeof(tag) - 1U;
            memcpy(tag, html + i + 1U, copy_len);
            tag[copy_len] = '\0';

            char name[24];
            web_copy_lower_name(tag, name, sizeof(name));
            const bool closing = tag[0] == '/';

            if (name[0] == '\0' || name[0] == '!' || name[0] == '?') {
                i = end;
                continue;
            }
            if (!closing && (strcmp(name, "script") == 0 || strcmp(name, "style") == 0)) {
                i = end;
                web_skip_element(html, len, &i, name);
                continue;
            }

            if (closing) {
                if (strcmp(name, "a") == 0) {
                    current_link = -1;
                    current_style = WEB_LINE_NORMAL;
                } else if (strcmp(name, "form") == 0) {
                    current_form = -1;
                    web_newline(WEB_LINE_NORMAL);
                    last_was_space = true;
                } else if (web_tag_is_heading(name)) {
                    current_style = WEB_LINE_NORMAL;
                    web_newline(WEB_LINE_NORMAL);
                    last_was_space = true;
                } else if (web_tag_is_block(name) || strcmp(name, "li") == 0) {
                    web_newline(WEB_LINE_NORMAL);
                    last_was_space = true;
                }
                i = end;
                continue;
            }

            if (strcmp(name, "br") == 0) {
                web_newline(current_style);
                last_was_space = true;
            } else if (strcmp(name, "base") == 0) {
                char href[WEB_URL_MAX];
                if (web_tag_attr(tag, "href", href, sizeof(href))) {
                    char resolved[WEB_URL_MAX];
                    if (web_resolve_url(web_current_base_url(), href, resolved, sizeof(resolved))) {
                        strlcpy(web.base_url, resolved, sizeof(web.base_url));
                        SOLAR_OS_LOGD(TAG, "base URL %s", web.base_url);
                    }
                }
            } else if (strcmp(name, "form") == 0) {
                char action[WEB_URL_MAX] = "";
                char method[8] = "get";
                (void)web_tag_attr(tag, "action", action, sizeof(action));
                (void)web_tag_attr(tag, "method", method, sizeof(method));
                current_form = web_add_form(action, method);
                web_newline(WEB_LINE_NORMAL);
                last_was_space = true;
            } else if (strcmp(name, "input") == 0) {
                char type[24] = "text";
                char name_attr[WEB_CONTROL_NAME_MAX] = "";
                char value[WEB_CONTROL_VALUE_MAX] = "";
                char label[WEB_CONTROL_VALUE_MAX] = "";
                (void)web_tag_attr(tag, "type", type, sizeof(type));
                (void)web_tag_attr(tag, "name", name_attr, sizeof(name_attr));
                (void)web_tag_attr(tag, "value", value, sizeof(value));
                (void)web_tag_attr(tag, "placeholder", label, sizeof(label));
                web_lower_ascii(type);

                web_control_type_t control_type = WEB_CONTROL_TEXT;
                bool visible = true;
                if (strcmp(type, "hidden") == 0) {
                    control_type = WEB_CONTROL_HIDDEN;
                    visible = false;
                } else if (strcmp(type, "checkbox") == 0) {
                    control_type = WEB_CONTROL_CHECKBOX;
                    if (value[0] == '\0') {
                        strlcpy(value, "on", sizeof(value));
                    }
                } else if (strcmp(type, "radio") == 0) {
                    control_type = WEB_CONTROL_RADIO;
                    if (value[0] == '\0') {
                        strlcpy(value, "on", sizeof(value));
                    }
                } else if (strcmp(type, "submit") == 0 || strcmp(type, "button") == 0) {
                    control_type = WEB_CONTROL_SUBMIT;
                    if (value[0] == '\0') {
                        strlcpy(value, "submit", sizeof(value));
                    }
                    if (label[0] == '\0') {
                        strlcpy(label, value, sizeof(label));
                    }
                } else if (strcmp(type, "password") == 0 ||
                           strcmp(type, "search") == 0 ||
                           strcmp(type, "email") == 0 ||
                           strcmp(type, "url") == 0 ||
                           strcmp(type, "number") == 0) {
                    control_type = WEB_CONTROL_TEXT;
                } else if (strcmp(type, "text") != 0) {
                    visible = false;
                }

                const int control = (!visible && control_type != WEB_CONTROL_HIDDEN) ?
                    -1 :
                    web_add_control(control_type,
                                    current_form,
                                    name_attr,
                                    value,
                                    label,
                                    web_tag_has_attr(tag, "checked"),
                                    visible);
                if (control >= 0 && visible) {
                    web_add_control_line(control);
                    last_was_space = true;
                }
            } else if (strcmp(name, "li") == 0) {
                web_newline(WEB_LINE_NORMAL);
                web_append_text("* ", -1, WEB_LINE_NORMAL);
                last_was_space = false;
            } else if (web_tag_is_block(name)) {
                web_newline(WEB_LINE_NORMAL);
                last_was_space = true;
            } else if (web_tag_is_heading(name)) {
                web_newline(WEB_LINE_HEADING);
                current_style = WEB_LINE_HEADING;
                last_was_space = true;
            } else if (strcmp(name, "a") == 0) {
                char href[WEB_URL_MAX];
                if (web_tag_attr(tag, "href", href, sizeof(href))) {
                    current_link = web_add_link(href);
                    if (current_link >= 0) {
                        char prefix[16];
                        snprintf(prefix, sizeof(prefix), "[%d] ", current_link + 1);
                        web_append_text(prefix, current_link, WEB_LINE_LINK);
                        current_style = WEB_LINE_LINK;
                        last_was_space = false;
                    }
                }
            } else if (strcmp(name, "img") == 0) {
                char src[WEB_URL_MAX];
                char alt[WEB_LINK_TITLE_MAX];
                src[0] = '\0';
                alt[0] = '\0';
                (void)web_tag_image_src(tag, src, sizeof(src));
                (void)web_tag_attr(tag, "alt", alt, sizeof(alt));
                const int image = web_add_image(src, alt);
                if (image >= 0) {
                    web_add_image_line(image, current_link);
                    last_was_space = true;
                } else if (alt[0] != '\0') {
                    web_append_text("[image: ", current_link, WEB_LINE_NORMAL);
                    web_append_text(alt, current_link, WEB_LINE_NORMAL);
                    web_append_text("]", current_link, WEB_LINE_NORMAL);
                    last_was_space = false;
                } else {
                    web_append_text("[image]", current_link, WEB_LINE_NORMAL);
                    last_was_space = false;
                }
            }
            i = end;
            continue;
        }

        if (ch == '&') {
            web_append_entity(html, len, &i, current_link, current_style);
            last_was_space = false;
            continue;
        }
        if (isspace((unsigned char)ch)) {
            if (!last_was_space) {
                web_append_char(' ', current_link, current_style);
                last_was_space = true;
            }
            continue;
        }
        if ((unsigned char)ch >= 0x20U) {
            web_append_char(ch, current_link, current_style);
            last_was_space = false;
        }
    }

    if (web.line_count == 0) {
        web_append_text("(empty page)", -1, WEB_LINE_STATUS);
    }
    web_rebuild_items();
}

static bool web_base_parts(const char *base,
                           char *scheme_host,
                           size_t scheme_host_len,
                           char *dir,
                           size_t dir_len)
{
    if (base == NULL || scheme_host == NULL || scheme_host_len == 0 ||
        dir == NULL || dir_len == 0) {
        return false;
    }
    scheme_host[0] = '\0';
    dir[0] = '\0';

    const char *scheme = strstr(base, "://");
    if (scheme == NULL) {
        return false;
    }
    const char *host = scheme + 3;
    const char *path = strchr(host, '/');
    const size_t host_len = path != NULL ? (size_t)(path - base) : strlen(base);
    if (host_len + 1U > scheme_host_len) {
        return false;
    }
    memcpy(scheme_host, base, host_len);
    scheme_host[host_len] = '\0';

    if (path == NULL) {
        strlcpy(dir, "/", dir_len);
        return true;
    }

    const char *last = strrchr(path, '/');
    const size_t path_len = last != NULL ? (size_t)(last - path + 1) : 1U;
    if (path_len + 1U > dir_len) {
        return false;
    }
    memcpy(dir, path, path_len);
    dir[path_len] = '\0';
    return true;
}

static bool web_copy_url_no_fragment(const char *url, char *out, size_t out_len)
{
    if (url == NULL || out == NULL || out_len == 0) {
        return false;
    }

    while (*url != '\0' && isspace((unsigned char)*url)) {
        url++;
    }

    size_t len = strcspn(url, "#");
    while (len > 0 && isspace((unsigned char)url[len - 1U])) {
        len--;
    }
    if (len == 0 || len + 1U > out_len) {
        out[0] = '\0';
        return false;
    }

    memcpy(out, url, len);
    out[len] = '\0';
    return true;
}

static bool web_url_has_scheme(const char *href)
{
    if (href == NULL) {
        return false;
    }
    for (const char *p = href; *p != '\0'; p++) {
        if (*p == ':') {
            return true;
        }
        if (*p == '/' || *p == '?' || *p == '#') {
            return false;
        }
    }
    return false;
}

static void web_url_pop_dir(char *dir, size_t dir_len)
{
    if (dir == NULL || dir_len == 0) {
        return;
    }
    size_t len = strlen(dir);
    if (len <= 1U) {
        strlcpy(dir, "/", dir_len);
        return;
    }
    if (dir[len - 1U] == '/') {
        dir[--len] = '\0';
    }

    char *slash = strrchr(dir, '/');
    if (slash == NULL || slash == dir) {
        strlcpy(dir, "/", dir_len);
        return;
    }
    slash[1] = '\0';
}

static bool web_resolve_url(const char *base, const char *href, char *out, size_t out_len)
{
    if (href == NULL || href[0] == '\0' || out == NULL || out_len == 0) {
        return false;
    }
    out[0] = '\0';

    char ref[WEB_URL_MAX];
    if (!web_copy_url_no_fragment(href, ref, sizeof(ref))) {
        return false;
    }
    if (web_url_supported(ref)) {
        return strlcpy(out, ref, out_len) < out_len;
    }
    if (web_url_has_scheme(ref)) {
        return false;
    }

    char scheme_host[WEB_URL_MAX];
    char dir[WEB_URL_MAX];
    if (!web_base_parts(base, scheme_host, sizeof(scheme_host), dir, sizeof(dir))) {
        return false;
    }

    if (strncmp(ref, "//", 2) == 0) {
        const char *sep = strstr(base, "://");
        if (sep == NULL) {
            return false;
        }
        const size_t scheme_len = (size_t)(sep - base);
        if (scheme_len + 1U + strlen(ref) + 1U > out_len) {
            return false;
        }
        memcpy(out, base, scheme_len);
        out[scheme_len] = ':';
        strlcpy(out + scheme_len + 1U, ref, out_len - scheme_len - 1U);
        return true;
    }

    if (ref[0] == '?') {
        char base_page[WEB_URL_MAX];
        if (!web_copy_url_no_fragment(base, base_page, sizeof(base_page))) {
            return false;
        }
        char *query = strchr(base_page, '?');
        if (query != NULL) {
            *query = '\0';
        }
        snprintf(out, out_len, "%s%s", base_page, ref);
    } else if (ref[0] == '/') {
        snprintf(out, out_len, "%s%s", scheme_host, ref);
    } else {
        const char *rel = ref;
        while (strncmp(rel, "./", 2) == 0) {
            rel += 2;
        }
        while (strncmp(rel, "../", 3) == 0) {
            web_url_pop_dir(dir, sizeof(dir));
            rel += 3;
            while (strncmp(rel, "./", 2) == 0) {
                rel += 2;
            }
        }
        snprintf(out, out_len, "%s%s%s", scheme_host, dir, rel);
    }
    return out[0] != '\0';
}

static const char *web_current_base_url(void)
{
    return web.base_url[0] != '\0' ? web.base_url : web.url;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t max_len;
    bool truncated;
    char redirect_url[WEB_URL_MAX];
} web_fetch_buffer_t;

static esp_err_t web_fetch_event(esp_http_client_event_t *event)
{
    if (event == NULL) {
        return ESP_OK;
    }
    if (web.stop_requested) {
        return ESP_FAIL;
    }
    if (event->event_id == HTTP_EVENT_ON_HEADER) {
        web_fetch_buffer_t *buffer = (web_fetch_buffer_t *)event->user_data;
        if (buffer != NULL &&
            event->header_key != NULL &&
            event->header_value != NULL &&
            strcasecmp(event->header_key, "Location") == 0) {
            strlcpy(buffer->redirect_url, event->header_value, sizeof(buffer->redirect_url));
        }
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    if (web_http_event_is_redirect_body(event)) {
        return ESP_OK;
    }

    web_fetch_buffer_t *buffer = (web_fetch_buffer_t *)event->user_data;
    if (buffer == NULL || buffer->data == NULL || event->data == NULL || event->data_len <= 0) {
        return ESP_OK;
    }

    const size_t len = (size_t)event->data_len;
    const size_t remaining = buffer->max_len - buffer->len;
    const size_t copy_len = len < remaining ? len : remaining;
    if (copy_len > 0) {
        memcpy(buffer->data + buffer->len, event->data, copy_len);
        buffer->len += copy_len;
    }
    if (copy_len < len) {
        buffer->truncated = true;
    }
    return ESP_OK;
}

static esp_err_t web_fetch_bytes(const char *url,
                                 size_t max_len,
                                 uint8_t **out_data,
                                 size_t *out_len,
                                 bool *out_truncated,
                                 int *out_status)
{
    if (url == NULL || out_data == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = NULL;
    *out_len = 0;
    if (out_truncated != NULL) {
        *out_truncated = false;
    }
    if (out_status != NULL) {
        *out_status = -1;
    }

    char current_url[WEB_URL_MAX];
    if (strlcpy(current_url, url, sizeof(current_url)) >= sizeof(current_url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *data = web_malloc(max_len);
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    web_fetch_buffer_t buffer = {
        .data = data,
        .max_len = max_len,
    };
    esp_err_t err = ESP_FAIL;
    int status = -1;
    for (int redirect = 0; redirect <= WEB_REDIRECT_MAX && !web.stop_requested; redirect++) {
        buffer.len = 0;
        buffer.truncated = false;
        buffer.redirect_url[0] = '\0';

        esp_http_client_config_t config = {
            .url = current_url,
            .method = HTTP_METHOD_GET,
            .timeout_ms = WEB_TIMEOUT_MS,
            .disable_auto_redirect = true,
            .event_handler = web_fetch_event,
            .buffer_size = 1024,
            .buffer_size_tx = 512,
            .user_agent = "SolarOS-web/0.1",
            .user_data = &buffer,
        };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            heap_caps_free(data);
            return ESP_ERR_NO_MEM;
        }

        web.client = client;
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        web.client = NULL;
        esp_http_client_cleanup(client);

        if (out_status != NULL) {
            *out_status = status;
        }
        if (out_truncated != NULL) {
            *out_truncated = buffer.truncated;
        }

        if (err == ESP_OK &&
            status >= 300 &&
            status < 400 &&
            buffer.redirect_url[0] != '\0') {
            char next_url[WEB_URL_MAX];
            if (!web_resolve_url(current_url, buffer.redirect_url, next_url, sizeof(next_url))) {
                heap_caps_free(data);
                return ESP_FAIL;
            }
            SOLAR_OS_LOGI(TAG, "redirect %d: %s -> %s", status, current_url, next_url);
            strlcpy(current_url, next_url, sizeof(current_url));
            continue;
        }

        break;
    }

    if (out_status != NULL) {
        *out_status = status;
    }
    if (out_truncated != NULL) {
        *out_truncated = buffer.truncated;
    }
    if (err != ESP_OK) {
        heap_caps_free(data);
        return err;
    }
    if (status < 200 || status >= 300 || buffer.truncated) {
        heap_caps_free(data);
        return ESP_FAIL;
    }

    *out_data = data;
    *out_len = buffer.len;
    return ESP_OK;
}

static solar_os_gfx_color_t web_gray_to_color(uint8_t gray)
{
    const uint8_t level = (uint8_t)(((uint16_t)gray * SOLAR_OS_GFX_GRAY_MAX + 127U) / 255U);
    return solar_os_gfx_gray(level);
}

static bool web_bytes_are_webp(const uint8_t *data, size_t len)
{
    return data != NULL &&
        len >= 12U &&
        memcmp(data, "RIFF", 4) == 0 &&
        memcmp(data + 8, "WEBP", 4) == 0;
}

static bool web_detect_image_format(const uint8_t *data,
                                    size_t len,
                                    web_image_decoder_t *decoder,
                                    const char **format)
{
    if (decoder != NULL) {
        *decoder = WEB_IMAGE_DECODE_NONE;
    }
    if (format != NULL) {
        *format = "image";
    }
    if (data == NULL || len < 2U) {
        return false;
    }

    if (data[0] == 0xff && data[1] == 0xd8) {
        if (decoder != NULL) {
            *decoder = WEB_IMAGE_DECODE_STB;
        }
        if (format != NULL) {
            *format = "JPEG";
        }
        return true;
    }
    if (len >= 8U &&
        data[0] == 0x89 &&
        data[1] == 'P' &&
        data[2] == 'N' &&
        data[3] == 'G' &&
        data[4] == 0x0d &&
        data[5] == 0x0a &&
        data[6] == 0x1a &&
        data[7] == 0x0a) {
        if (decoder != NULL) {
            *decoder = WEB_IMAGE_DECODE_STB;
        }
        if (format != NULL) {
            *format = "PNG";
        }
        return true;
    }
    if (len >= 3U && memcmp(data, "GIF", 3) == 0) {
        if (decoder != NULL) {
            *decoder = WEB_IMAGE_DECODE_STB;
        }
        if (format != NULL) {
            *format = "GIF";
        }
        return true;
    }
    if (web_bytes_are_webp(data, len)) {
        if (decoder != NULL) {
            *decoder = WEB_IMAGE_DECODE_WEBP;
        }
        if (format != NULL) {
            *format = "WebP";
        }
        return true;
    }
    return false;
}

static bool web_url_ext_eq(const char *dot, const char *end, const char *ext)
{
    const size_t dot_len = (size_t)(end - dot);
    return dot_len == strlen(ext) && strncasecmp(dot, ext, dot_len) == 0;
}

static const char *web_url_extension(const char *url, const char **out_end)
{
    if (out_end != NULL) {
        *out_end = NULL;
    }
    if (url == NULL) {
        return NULL;
    }

    const char *end = strpbrk(url, "?#");
    if (end == NULL) {
        end = url + strlen(url);
    }
    const char *dot = end;
    while (dot > url && dot[-1] != '/' && dot[-1] != ':') {
        dot--;
        if (*dot == '.') {
            if (out_end != NULL) {
                *out_end = end;
            }
            return dot;
        }
    }
    return NULL;
}

static bool web_url_looks_like_image(const char *url)
{
    const char *end = NULL;
    const char *dot = web_url_extension(url, &end);
    if (dot == NULL || end == NULL) {
        return false;
    }

    return web_url_ext_eq(dot, end, ".jpg") ||
        web_url_ext_eq(dot, end, ".jpeg") ||
        web_url_ext_eq(dot, end, ".png") ||
        web_url_ext_eq(dot, end, ".gif") ||
        web_url_ext_eq(dot, end, ".webp");
}

static bool web_url_is_unsupported_image(const char *url)
{
    const char *end = NULL;
    const char *dot = web_url_extension(url, &end);
    if (dot == NULL || end == NULL) {
        return false;
    }

    return web_url_ext_eq(dot, end, ".svg") ||
        web_url_ext_eq(dot, end, ".svgz");
}

static void web_apply_image_layout(web_image_t *image,
                                   uint32_t width,
                                   uint32_t height,
                                   const char *src,
                                   const char *format)
{
    image->width = width;
    image->height = height;
    image->loaded = true;

    uint32_t draw_width = width;
    uint32_t draw_height = height;
    const uint32_t max_width = 400U - (2U * WEB_MARGIN_X);
    if (draw_width > max_width) {
        draw_height = (draw_height * max_width) / draw_width;
        draw_width = max_width;
    }
    if (draw_height > WEB_IMAGE_MAX_HEIGHT) {
        draw_width = (draw_width * WEB_IMAGE_MAX_HEIGHT) / draw_height;
        draw_height = WEB_IMAGE_MAX_HEIGHT;
    }
    if (draw_width == 0) {
        draw_width = 1;
    }
    if (draw_height == 0) {
        draw_height = 1;
    }
    image->draw_width = (uint16_t)draw_width;
    image->draw_height = (uint16_t)draw_height;
    SOLAR_OS_LOGI(TAG,
                  "%s image loaded %" PRIu32 "x%" PRIu32 " draw=%ux%u stack_high_water=%u src=%s",
                  format != NULL ? format : "image",
                  width,
                  height,
                  (unsigned)image->draw_width,
                  (unsigned)image->draw_height,
                  (unsigned)uxTaskGetStackHighWaterMark(NULL),
                  src != NULL ? src : "");
}

static esp_err_t web_decode_image_bytes(web_image_t *image,
                                        const uint8_t *data,
                                        size_t len,
                                        const char *src)
{
    if (image == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *gray = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    web_image_decoder_t decoder = WEB_IMAGE_DECODE_STB;
    const char *format = "image";
    (void)web_detect_image_format(data, len, &decoder, &format);

    esp_err_t err;
    if (decoder == WEB_IMAGE_DECODE_WEBP) {
        err = solar_os_webp_decode_gray(data,
                                        len,
                                        WEB_IMAGE_MAX_PIXELS,
                                        &gray,
                                        &width,
                                        &height);
    } else {
        decoder = WEB_IMAGE_DECODE_STB;
        err = solar_os_stb_decode_gray(data,
                                       len,
                                       WEB_IMAGE_MAX_PIXELS,
                                       &gray,
                                       &width,
                                       &height);
    }
    if (err != ESP_OK || gray == NULL || width == 0 || height == 0) {
        SOLAR_OS_LOGW(TAG,
                      "%s image decode failed: %s src=%s reason=%s bytes=%u",
                      format,
                      esp_err_to_name(err),
                      src != NULL ? src : "",
                      decoder == WEB_IMAGE_DECODE_STB ?
                          solar_os_stb_failure_reason() :
                          "webp decode failed",
                      (unsigned)len);
        return err;
    }

    web_free_image_data(image);
    image->gray = gray;
    image->decoder = decoder;
    web_apply_image_layout(image, width, height, src, format);
    return ESP_OK;
}

static void web_update_image_line_heights(void)
{
    if (web.lines == NULL || web.images == NULL) {
        return;
    }

    for (size_t i = 0; i < web.line_count; i++) {
        web_line_t *line = &web.lines[i];
        if (line->image_index < 0 || line->image_index >= (int)web.image_count) {
            continue;
        }
        const web_image_t *image = &web.images[line->image_index];
        line->height = image->loaded && image->draw_height > 0 ?
            (uint16_t)(image->draw_height + 4U) : 22U;
    }
}

static void web_load_images(void)
{
    for (size_t i = 0; i < web.image_count && !web.stop_requested; i++) {
        web_image_t *image = &web.images[i];
        image->attempted = true;

        char resolved[WEB_URL_MAX];
        if (!web_resolve_url(web_current_base_url(), image->src, resolved, sizeof(resolved))) {
            continue;
        }
        if (web_url_is_unsupported_image(resolved)) {
            SOLAR_OS_LOGD(TAG, "skip unsupported image src=%s", resolved);
            continue;
        }

        char status[WEB_STATUS_MAX];
        snprintf(status, sizeof(status), "image %u/%u", (unsigned)(i + 1U), (unsigned)web.image_count);
        web_send_message(WEB_EVENT_STATUS, status);

        uint8_t *data = NULL;
        size_t len = 0;
        bool truncated = false;
        int status_code = -1;
        esp_err_t err = web_fetch_bytes(resolved,
                                        WEB_IMAGE_MAX_BYTES,
                                        &data,
                                        &len,
                                        &truncated,
                                        &status_code);
        image->status_code = status_code;
        image->truncated = truncated;
        if (err != ESP_OK || data == NULL || len == 0) {
            SOLAR_OS_LOGW(TAG,
                          "image fetch failed: %s status=%d truncated=%s max=%u src=%s",
                          esp_err_to_name(err),
                          status_code,
                          truncated ? "yes" : "no",
                          (unsigned)WEB_IMAGE_MAX_BYTES,
                          resolved);
            continue;
        }

        err = web_decode_image_bytes(image, data, len, resolved);
        heap_caps_free(data);
        if (err != ESP_OK) {
            continue;
        }
    }

    web_update_image_line_heights();
}

static esp_err_t web_build_image_document(const uint8_t *data, size_t len, const char *src)
{
    if (data == NULL || len == 0 || src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    web.line_count = 0;
    web.link_count = 0;
    web.control_count = 0;
    web.form_count = 0;
    web.image_count = 0;
    web.item_count = 0;

    const int image_index = web_add_image(src, "image");
    if (image_index < 0) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = web_decode_image_bytes(&web.images[image_index], data, len, src);
    if (err != ESP_OK) {
        return err;
    }

    web_add_image_line(image_index, -1);
    web_update_image_line_heights();
    web_rebuild_items();
    return ESP_OK;
}

static esp_err_t web_load_direct_image_document(uint32_t *out_bytes,
                                                bool *out_truncated,
                                                int *out_status)
{
    uint8_t *data = NULL;
    size_t len = 0;
    bool truncated = false;
    int status_code = -1;
    esp_err_t err = web_fetch_bytes(web.url,
                                    WEB_IMAGE_MAX_BYTES,
                                    &data,
                                    &len,
                                    &truncated,
                                    &status_code);
    if (out_bytes != NULL) {
        *out_bytes = (uint32_t)len;
    }
    if (out_truncated != NULL) {
        *out_truncated = truncated;
    }
    if (out_status != NULL) {
        *out_status = status_code;
    }
    if (err != ESP_OK || data == NULL || len == 0) {
        SOLAR_OS_LOGW(TAG,
                      "direct image fetch failed: %s status=%d truncated=%s max=%u src=%s",
                      esp_err_to_name(err),
                      status_code,
                      truncated ? "yes" : "no",
                      (unsigned)WEB_IMAGE_MAX_BYTES,
                      web.url);
        heap_caps_free(data);
        return err;
    }

    err = web_build_image_document(data, len, web.url);
    heap_caps_free(data);
    return err;
}

static void web_task(void *arg)
{
    (void)arg;

    web_send_message(WEB_EVENT_STATUS, "connecting");
    web.html_len = 0;
    web.html_truncated = false;
    if (web.html != NULL) {
        web.html[0] = '\0';
    }

    esp_http_client_handle_t client = NULL;
    if (web_url_looks_like_image(web.url)) {
        web_send_message(WEB_EVENT_STATUS, "image");
        uint32_t bytes_read = 0;
        bool truncated = false;
        int status_code = -1;
        const esp_err_t image_err =
            web_load_direct_image_document(&bytes_read, &truncated, &status_code);
        if (image_err == ESP_OK) {
            web_event_t event = {
                .type = WEB_EVENT_DONE,
                .status_code = status_code,
                .bytes_read = bytes_read,
                .truncated = truncated,
            };
            (void)web_send_event(&event);
        } else {
            web_event_t event = {
                .type = WEB_EVENT_ERROR,
                .status_code = status_code,
                .bytes_read = bytes_read,
                .truncated = truncated,
            };
            snprintf(event.message, sizeof(event.message), "%s", esp_err_to_name(image_err));
            (void)web_send_event(&event);
        }
        goto done;
    }

    esp_http_client_config_t config = {
        .url = web.url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = WEB_TIMEOUT_MS,
        .disable_auto_redirect = false,
        .event_handler = web_http_event,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "SolarOS-web/0.1",
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    SOLAR_OS_LOGI(TAG, "GET %s", web.url);
    client = esp_http_client_init(&config);
    if (client == NULL) {
        web_send_message(WEB_EVENT_ERROR, "HTTP client init failed");
        goto done;
    }
    web.client = client;

    const esp_err_t err = esp_http_client_perform(client);
    web.status_code = esp_http_client_get_status_code(client);

    if (web.stop_requested) {
        web_send_message(WEB_EVENT_ERROR, "cancelled");
    } else if (err != ESP_OK) {
        web_event_t event = {
            .type = WEB_EVENT_ERROR,
            .status_code = web.status_code,
            .bytes_read = web.bytes_read,
            .truncated = web.html_truncated,
        };
        snprintf(event.message, sizeof(event.message), "%s", esp_err_to_name(err));
        (void)web_send_event(&event);
    } else {
        web_send_message(WEB_EVENT_STATUS, "rendering");
        web_image_decoder_t decoder = WEB_IMAGE_DECODE_NONE;
        if (web_detect_image_format(web.html, web.html_len, &decoder, NULL)) {
            const esp_err_t image_err = web_build_image_document(web.html, web.html_len, web.url);
            if (image_err != ESP_OK) {
                web_event_t event = {
                    .type = WEB_EVENT_ERROR,
                    .status_code = web.status_code,
                    .bytes_read = web.bytes_read,
                    .truncated = web.html_truncated,
                };
                snprintf(event.message, sizeof(event.message), "%s", esp_err_to_name(image_err));
                (void)web_send_event(&event);
                goto done;
            }
        } else {
            web_parse_html();
            if (web.image_count > 0) {
                web_load_images();
            }
        }
        web_event_t event = {
            .type = WEB_EVENT_DONE,
            .status_code = web.status_code,
            .bytes_read = web.bytes_read,
            .truncated = web.html_truncated,
        };
        (void)web_send_event(&event);
    }

done:
    if (client != NULL) {
        web.client = NULL;
        esp_http_client_cleanup(client);
    }
    SOLAR_OS_LOGD(TAG,
                  "task done stack_high_water=%u",
                  (unsigned)uxTaskGetStackHighWaterMark(NULL));
    web.task_done = true;
    vTaskDelete(NULL);
}

static int web_body_height(solar_os_gfx_t *gfx)
{
    const int height = (int)solar_os_gfx_height(gfx);
    const int body = height - WEB_HEADER_HEIGHT - WEB_FOOTER_HEIGHT - 2;
    return body > WEB_LINE_HEIGHT ? body : WEB_LINE_HEIGHT;
}

static int web_line_height_at(int line_index)
{
    if (line_index < 0 || line_index >= (int)web.line_count || web.lines == NULL) {
        return WEB_LINE_HEIGHT;
    }
    return web.lines[line_index].height > 0 ? web.lines[line_index].height : WEB_LINE_HEIGHT;
}

static int web_visible_line_count(solar_os_gfx_t *gfx)
{
    const int body = web_body_height(gfx);
    int used = 0;
    int count = 0;
    for (int i = web.scroll; i < (int)web.line_count; i++) {
        const int line_height = web_line_height_at(i);
        if (count > 0 && used + line_height > body) {
            break;
        }
        used += line_height;
        count++;
        if (used >= body) {
            break;
        }
    }
    if (count > 0) {
        return count;
    }
    return body > WEB_LINE_HEIGHT ? body / WEB_LINE_HEIGHT : 1;
}

static int web_max_scroll(solar_os_gfx_t *gfx)
{
    if (web.line_count == 0) {
        return 0;
    }

    const int body = web_body_height(gfx);
    int used = 0;
    for (int i = (int)web.line_count - 1; i >= 0; i--) {
        const int line_height = web_line_height_at(i);
        if (used + line_height > body) {
            const int max_scroll = i + 1;
            return max_scroll < (int)web.line_count ? max_scroll : (int)web.line_count - 1;
        }
        used += line_height;
    }
    return 0;
}

static void web_clamp_scroll(solar_os_gfx_t *gfx)
{
    const int max_scroll = web_max_scroll(gfx);
    if (web.scroll < 0) {
        web.scroll = 0;
    }
    if (web.scroll > max_scroll) {
        web.scroll = max_scroll;
    }
}

static int web_first_line_for_item(int item_index)
{
    if (item_index < 0 || item_index >= (int)web.item_count) {
        return -1;
    }
    return web.items[item_index].line_index;
}

static void web_make_item_visible(solar_os_gfx_t *gfx)
{
    const int line = web_first_line_for_item(web.selected_item);
    if (line < 0) {
        return;
    }

    const int visible = web_visible_line_count(gfx);
    if (line < web.scroll) {
        web.scroll = line;
    } else if (line >= web.scroll + visible) {
        web.scroll = line - visible + 1;
    }
    web_clamp_scroll(gfx);
}

static void web_select_next_item(solar_os_gfx_t *gfx, int delta)
{
    if (web.item_count == 0) {
        web.selected_item = -1;
        return;
    }
    if (web.selected_item < 0) {
        web.selected_item = delta >= 0 ? 0 : (int)web.item_count - 1;
    } else {
        web.selected_item += delta;
        if (web.selected_item < 0) {
            web.selected_item = (int)web.item_count - 1;
        } else if (web.selected_item >= (int)web.item_count) {
            web.selected_item = 0;
        }
    }
    web_make_item_visible(gfx);
}

static void web_draw_text_clipped(solar_os_gfx_t *gfx,
                                  int x,
                                  int baseline_y,
                                  const char *text,
                                  size_t max_chars)
{
    char clipped[WEB_LINE_MAX];
    if (text == NULL) {
        return;
    }
    strlcpy(clipped, text, sizeof(clipped));
    if (max_chars + 1U < sizeof(clipped)) {
        clipped[max_chars] = '\0';
    }
    solar_os_gfx_text(gfx, x, baseline_y, clipped);
}

static const char *web_control_label(const web_control_t *control, const char *fallback)
{
    if (control == NULL) {
        return fallback != NULL ? fallback : "";
    }
    if (control->label[0] != '\0') {
        return control->label;
    }
    if (control->name[0] != '\0') {
        return control->name;
    }
    return fallback != NULL ? fallback : "";
}

static bool web_control_is_editing(const web_control_t *control)
{
    return web.editing &&
        web.edit_control >= 0 &&
        web.edit_control < (int)web.control_count &&
        control == &web.controls[web.edit_control];
}

static size_t web_copy_control_value_window(const web_control_t *control,
                                            char *out,
                                            size_t out_len,
                                            size_t visible_chars)
{
    if (out == NULL || out_len == 0) {
        return 0;
    }
    out[0] = '\0';
    if (control == NULL || visible_chars == 0) {
        return 0;
    }

    const size_t len = strlen(control->value);
    size_t cursor = web_control_is_editing(control) ? web.edit_cursor : 0;
    if (cursor > len) {
        cursor = len;
    }

    size_t start = 0;
    if (web_control_is_editing(control) && cursor >= visible_chars) {
        start = cursor - visible_chars + 1U;
    }

    size_t copy_len = len > start ? len - start : 0;
    if (copy_len > visible_chars) {
        copy_len = visible_chars;
    }
    if (copy_len + 1U > out_len) {
        copy_len = out_len - 1U;
    }
    if (copy_len > 0) {
        memcpy(out, control->value + start, copy_len);
    }
    out[copy_len] = '\0';

    if (cursor < start) {
        return 0;
    }
    size_t cursor_pos = cursor - start;
    return cursor_pos <= copy_len ? cursor_pos : copy_len;
}

static void web_draw_control_text(solar_os_gfx_t *gfx,
                                  const web_control_t *control,
                                  int row_x,
                                  int row_y,
                                  int row_w)
{
    const int label_w = row_w > 230 ? 112 : 78;
    const int field_x = row_x + label_w + 4;
    const int field_y = row_y + 3;
    const int field_w = row_x + row_w - field_x;
    if (field_w < 36) {
        return;
    }

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    web_draw_text_clipped(gfx,
                          row_x,
                          row_y + 14,
                          web_control_label(control, "text"),
                          (size_t)(label_w / 6));

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, field_x, field_y, field_w, WEB_CONTROL_FIELD_HEIGHT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, field_x, field_y, field_w, WEB_CONTROL_FIELD_HEIGHT);
    if (web_control_is_editing(control)) {
        solar_os_gfx_rect(gfx, field_x - 1, field_y - 1, field_w + 2, WEB_CONTROL_FIELD_HEIGHT + 2);
    }

    const size_t visible_chars = field_w > 8 ? (size_t)((field_w - 8) / 6) : 0;
    char value[WEB_CONTROL_VALUE_MAX];
    const size_t cursor_pos = web_copy_control_value_window(control,
                                                           value,
                                                           sizeof(value),
                                                           visible_chars);
    web_draw_text_clipped(gfx,
                          field_x + 3,
                          field_y + 12,
                          value,
                          visible_chars);

    if (web_control_is_editing(control)) {
        const int cursor_x = field_x + 3 + (int)cursor_pos * 6;
        if (cursor_x < field_x + field_w - 2) {
            solar_os_gfx_line(gfx,
                              cursor_x,
                              field_y + 3,
                              cursor_x,
                              field_y + WEB_CONTROL_FIELD_HEIGHT - 4);
        }
    }
}

static void web_draw_control_button(solar_os_gfx_t *gfx,
                                    const web_control_t *control,
                                    int row_x,
                                    int row_y,
                                    int row_w,
                                    bool selected)
{
    const char *label = web_control_label(control, "submit");
    const size_t label_len = strlen(label);
    int button_w = (int)label_len * 6 + 20;
    if (button_w < 54) {
        button_w = 54;
    }
    if (button_w > row_w) {
        button_w = row_w;
    }

    const int button_h = 16;
    const int button_x = row_x;
    const int button_y = row_y + 3;
    solar_os_gfx_set_color(gfx, selected ? SOLAR_OS_GFX_COLOR_BLACK : SOLAR_OS_GFX_COLOR_LIGHT);
    solar_os_gfx_fill_rect(gfx, button_x, button_y, button_w, button_h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, button_x, button_y, button_w, button_h);

    const size_t max_chars = button_w > 10 ? (size_t)((button_w - 10) / 6) : 0;
    const int text_w = (int)((label_len < max_chars ? label_len : max_chars) * 6U);
    const int text_x = button_x + (button_w - text_w) / 2;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    solar_os_gfx_set_color(gfx, selected ? SOLAR_OS_GFX_COLOR_WHITE : SOLAR_OS_GFX_COLOR_BLACK);
    web_draw_text_clipped(gfx, text_x, button_y + 12, label, max_chars);
}

static void web_draw_control_choice(solar_os_gfx_t *gfx,
                                    const web_control_t *control,
                                    int row_x,
                                    int row_y,
                                    int row_w)
{
    const int box_x = row_x + 2;
    const int box_y = row_y + 5;
    const int center_x = box_x + WEB_CONTROL_BOX_SIZE / 2;
    const int center_y = box_y + WEB_CONTROL_BOX_SIZE / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    if (control->type == WEB_CONTROL_RADIO) {
        solar_os_gfx_fill_circle(gfx, center_x, center_y, WEB_CONTROL_BOX_SIZE / 2);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_circle(gfx, center_x, center_y, WEB_CONTROL_BOX_SIZE / 2);
        if (control->checked) {
            solar_os_gfx_fill_circle(gfx, center_x, center_y, 3);
        }
    } else {
        solar_os_gfx_fill_rect(gfx, box_x, box_y, WEB_CONTROL_BOX_SIZE, WEB_CONTROL_BOX_SIZE);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        solar_os_gfx_rect(gfx, box_x, box_y, WEB_CONTROL_BOX_SIZE, WEB_CONTROL_BOX_SIZE);
        if (control->checked) {
            solar_os_gfx_line(gfx, box_x + 2, box_y + 6, box_x + 5, box_y + 9);
            solar_os_gfx_line(gfx, box_x + 5, box_y + 9, box_x + 10, box_y + 2);
        }
    }

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    web_draw_text_clipped(gfx,
                          box_x + WEB_CONTROL_BOX_SIZE + 7,
                          row_y + 14,
                          web_control_label(control, control->type == WEB_CONTROL_RADIO ? "radio" : "checkbox"),
                          row_w > WEB_CONTROL_BOX_SIZE + 12 ?
                              (size_t)((row_w - WEB_CONTROL_BOX_SIZE - 12) / 6) :
                              0);
}

static void web_draw_control_widget(solar_os_gfx_t *gfx,
                                    const web_control_t *control,
                                    int row_x,
                                    int row_y,
                                    int row_w,
                                    bool selected)
{
    if (gfx == NULL || control == NULL || row_w <= 0) {
        return;
    }

    switch (control->type) {
    case WEB_CONTROL_TEXT:
        web_draw_control_text(gfx, control, row_x, row_y, row_w);
        break;
    case WEB_CONTROL_CHECKBOX:
    case WEB_CONTROL_RADIO:
        web_draw_control_choice(gfx, control, row_x, row_y, row_w);
        break;
    case WEB_CONTROL_SUBMIT:
        web_draw_control_button(gfx, control, row_x, row_y, row_w, selected);
        break;
    case WEB_CONTROL_HIDDEN:
    default:
        break;
    }
}

static void web_selection_status(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';

    if (web.selected_item < 0 || web.selected_item >= (int)web.item_count) {
        strlcpy(out, web.status, out_len);
        return;
    }

    const web_item_t *item = &web.items[web.selected_item];
    if (item->type == WEB_ITEM_LINK &&
        item->index >= 0 &&
        item->index < (int)web.link_count) {
        snprintf(out,
                 out_len,
                 "link %d/%u: %s",
                 web.selected_item + 1,
                 (unsigned)web.item_count,
                 web.links[item->index].href);
        return;
    }

    if (item->type == WEB_ITEM_CONTROL &&
        item->index >= 0 &&
        item->index < (int)web.control_count) {
        const web_control_t *control = &web.controls[item->index];
        const char *kind = "field";
        if (control->type == WEB_CONTROL_CHECKBOX) {
            kind = "checkbox";
        } else if (control->type == WEB_CONTROL_RADIO) {
            kind = "radio";
        } else if (control->type == WEB_CONTROL_SUBMIT) {
            kind = "submit";
        }
        snprintf(out,
                 out_len,
                 "%s %d/%u: %s",
                 kind,
                 web.selected_item + 1,
                 (unsigned)web.item_count,
                 control->label[0] != '\0' ? control->label : control->name);
        return;
    }

    strlcpy(out, web.status, out_len);
}

static void web_draw_image(solar_os_gfx_t *gfx,
                           const web_image_t *image,
                           int origin_x,
                           int origin_y,
                           int draw_width,
                           int draw_height)
{
    if (gfx == NULL ||
        image == NULL ||
        image->gray == NULL ||
        image->width == 0 ||
        image->height == 0 ||
        draw_width <= 0 ||
        draw_height <= 0) {
        return;
    }

    for (int dy = 0; dy < draw_height; dy++) {
        const uint32_t sy =
            (uint32_t)(((uint64_t)dy * image->height) / (uint32_t)draw_height);
        solar_os_gfx_color_t run_color = SOLAR_OS_GFX_COLOR_WHITE;
        int run_start = 0;
        bool run_active = false;

        for (int dx = 0; dx < draw_width; dx++) {
            const uint32_t sx =
                (uint32_t)(((uint64_t)dx * image->width) / (uint32_t)draw_width);
            const uint8_t gray = image->gray[(size_t)sy * image->width + sx];
            const solar_os_gfx_color_t color = web_gray_to_color(gray);
            if (!run_active) {
                run_active = true;
                run_color = color;
                run_start = dx;
            } else if (color != run_color) {
                solar_os_gfx_set_color(gfx, run_color);
                solar_os_gfx_fill_rect(gfx,
                                       origin_x + run_start,
                                       origin_y + dy,
                                       dx - run_start,
                                       1);
                run_color = color;
                run_start = dx;
            }
        }

        if (run_active) {
            solar_os_gfx_set_color(gfx, run_color);
            solar_os_gfx_fill_rect(gfx,
                                   origin_x + run_start,
                                   origin_y + dy,
                                   draw_width - run_start,
                                   1);
        }
    }
}

static void web_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL) {
        return;
    }

    const int width = (int)solar_os_gfx_width(gfx);
    const int height = (int)solar_os_gfx_height(gfx);
    const size_t max_chars = width > 12 ? (size_t)((width - 2 * WEB_MARGIN_X) / 6) : 20U;

    web_clamp_scroll(gfx);
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);

    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_fill_rect(gfx, 0, 0, width, WEB_HEADER_HEIGHT);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
    char header[WEB_LINE_MAX];
    if (web.loaded && web.item_count > 0) {
        snprintf(header,
                 sizeof(header),
                 "web %d/%u",
                 web.selected_item >= 0 ? web.selected_item + 1 : 0,
                 (unsigned)web.item_count);
    } else {
        strlcpy(header, web.loading ? "web loading" : "web", sizeof(header));
    }
    web_draw_text_clipped(gfx, WEB_MARGIN_X, 13, header, max_chars);

    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    const int url_y = height - 3;
    char footer[WEB_STATUS_MAX];
    web_selection_status(footer, sizeof(footer));
    web_draw_text_clipped(gfx, WEB_MARGIN_X, url_y, footer, max_chars);

    if (web.loading) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, WEB_MARGIN_X, WEB_HEADER_HEIGHT + 24, "Loading...");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, WEB_MARGIN_X, WEB_HEADER_HEIGHT + 40, web.url);
        solar_os_gfx_present(gfx);
        web.redraw = false;
        return;
    }

    if (!web.loaded) {
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BOLD);
        solar_os_gfx_text(gfx, WEB_MARGIN_X, WEB_HEADER_HEIGHT + 24, "web URL");
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_text(gfx, WEB_MARGIN_X, WEB_HEADER_HEIGHT + 40, "arrows scroll, TAB link, ENTER open");
        solar_os_gfx_present(gfx);
        web.redraw = false;
        return;
    }

    int y = WEB_HEADER_HEIGHT;
    for (int row = 0; row < (int)web.line_count; row++) {
        const int line_index = web.scroll + row;
        if (line_index < 0 || line_index >= (int)web.line_count) {
            break;
        }

        const web_line_t *line = &web.lines[line_index];
        const int line_height = line->height > 0 ? line->height : WEB_LINE_HEIGHT;
        if (y + line_height > height - WEB_FOOTER_HEIGHT) {
            break;
        }

        const bool selected = web_line_selected(line);
        if (selected) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_LIGHT);
            solar_os_gfx_fill_rect(gfx,
                                   0,
                                   y,
                                   width,
                                   line_height);
        }

        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        if (line->style == WEB_LINE_IMAGE &&
            line->image_index >= 0 &&
            line->image_index < (int)web.image_count) {
            const web_image_t *image = &web.images[line->image_index];
            if (image->loaded && image->draw_width > 0 && image->draw_height > 0) {
                int draw_width = image->draw_width;
                int draw_height = image->draw_height;
                const int max_width = width > 2 * WEB_MARGIN_X ? width - 2 * WEB_MARGIN_X : width;
                if (draw_width > max_width && draw_width > 0) {
                    draw_height = (draw_height * max_width) / draw_width;
                    draw_width = max_width;
                    if (draw_height < 1) {
                        draw_height = 1;
                    }
                }
                const int image_x = (width - draw_width) / 2;
                web_draw_image(gfx,
                               image,
                               image_x,
                               y + 2,
                               draw_width,
                               draw_height);
            } else {
                solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
                web_draw_text_clipped(gfx,
                                      WEB_MARGIN_X,
                                      y + WEB_TEXT_BASELINE,
                                      line->text,
                                      max_chars);
            }
        } else if (line->control_index >= 0 && line->control_index < (int)web.control_count) {
            web_draw_control_widget(gfx,
                                    &web.controls[line->control_index],
                                    WEB_MARGIN_X,
                                    y,
                                    width > 2 * WEB_MARGIN_X ? width - 2 * WEB_MARGIN_X : width,
                                    selected);
        } else {
            solar_os_gfx_set_font(gfx,
                                  line->style == WEB_LINE_HEADING || line->style == WEB_LINE_LINK ?
                                      SOLAR_OS_GFX_FONT_BOLD :
                                      SOLAR_OS_GFX_FONT_SMALL);
            web_draw_text_clipped(gfx, WEB_MARGIN_X, y + WEB_TEXT_BASELINE, line->text, max_chars);
            if (line->link_index >= 0 && line->text[0] != '\0') {
                const size_t len = strlen(line->text);
                const size_t chars = len < max_chars ? len : max_chars;
                solar_os_gfx_line(gfx,
                                  WEB_MARGIN_X,
                                  y + WEB_TEXT_BASELINE + 2,
                                  WEB_MARGIN_X + (int)chars * 6,
                                  y + WEB_TEXT_BASELINE + 2);
            }
        }
        y += line_height;
    }

    solar_os_gfx_present(gfx);
    web.redraw = false;
}

static void web_history_push_current(void)
{
    if (!web.loaded || web.url[0] == '\0') {
        return;
    }
    if (web.history_count > 0 &&
        strcmp(web.history[web.history_count - 1U], web.url) == 0) {
        return;
    }
    if (web.history_count >= WEB_HISTORY_COUNT) {
        memmove(web.history,
                web.history + 1,
                sizeof(web.history[0]) * (WEB_HISTORY_COUNT - 1U));
        web.history_count = WEB_HISTORY_COUNT - 1U;
    }
    strlcpy(web.history[web.history_count++], web.url, sizeof(web.history[0]));
}

static bool web_history_back(solar_os_context_t *ctx);

static esp_err_t web_start_load(solar_os_context_t *ctx, const char *url, bool push_history)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (url == NULL || !web_url_supported(url)) {
        web_set_status("unsupported URL");
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_wifi_status_t wifi;
    solar_os_wifi_get_status(&wifi);
    if (!wifi.started || !wifi.connected || !wifi.has_ip) {
        strlcpy(web.status, "wifi not connected", sizeof(web.status));
        web.loaded = false;
        web.loading = false;
        web.redraw = true;
        web_render(ctx);
        return ESP_ERR_INVALID_STATE;
    }

    if (web.task != NULL && !web.task_done) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t wrap_cols = gfx != NULL && solar_os_gfx_width(gfx) > 12 ?
        (size_t)((solar_os_gfx_width(gfx) - 2U * WEB_MARGIN_X) / 6U) : WEB_LINE_MAX - 1U;
    if (push_history) {
        web_history_push_current();
    }
    web_reset_document();
    web.wrap_cols = wrap_cols < WEB_LINE_MAX ? wrap_cols : WEB_LINE_MAX - 1U;
    xQueueReset(web.events);

    strlcpy(web.url, url, sizeof(web.url));
    strlcpy(web.base_url, web.url, sizeof(web.base_url));
    snprintf(web.status, sizeof(web.status), "GET %s", web.url);
    web.loading = true;
    web.loaded = false;
    web.stop_requested = false;
    web.task_done = false;
    web.redraw = true;
    web_render(ctx);

    const BaseType_t created = xTaskCreatePinnedToCore(web_task,
                                                       "solar_os_web",
                                                       WEB_TASK_STACK,
                                                       NULL,
                                                       WEB_TASK_PRIORITY,
                                                       &web.task,
                                                       tskNO_AFFINITY);
    if (created != pdPASS) {
        web.task = NULL;
        web.task_done = true;
        web.loading = false;
        web_set_status("task create failed");
        web_render(ctx);
        return ESP_ERR_NO_MEM;
    }

    if (gfx != NULL) {
        web_clamp_scroll(gfx);
    }
    return ESP_OK;
}

static void web_drain_events(solar_os_context_t *ctx)
{
    if (web.events == NULL) {
        return;
    }

    web_event_t event;
    while (xQueueReceive(web.events, &event, 0) == pdPASS) {
        switch (event.type) {
        case WEB_EVENT_STATUS:
            web_set_status(event.message);
            break;
        case WEB_EVENT_ERROR:
            web.loading = false;
            web.loaded = false;
            snprintf(web.status,
                     sizeof(web.status),
                     "error: %s",
                     event.message[0] != '\0' ? event.message : "request failed");
            web.redraw = true;
            break;
        case WEB_EVENT_DONE:
            web.loading = false;
            web.loaded = true;
            web.status_code = event.status_code;
            web.bytes_read = event.bytes_read;
            if (web.item_count > 0) {
                web.selected_item = 0;
                web_make_item_visible(solar_os_context_gfx(ctx));
            }
            snprintf(web.status,
                     sizeof(web.status),
                     "HTTP %d, %lu bytes, %u links, %u fields%s",
                     event.status_code,
                     (unsigned long)event.bytes_read,
                     (unsigned)web.link_count,
                     (unsigned)web.control_count,
                     event.truncated ? ", truncated" : "");
            web.redraw = true;
            break;
        default:
            break;
        }
    }
}

static bool web_url_append_char(char *out, size_t out_len, char ch)
{
    const size_t len = strlen(out);
    if (len + 1U >= out_len) {
        return false;
    }
    out[len] = ch;
    out[len + 1U] = '\0';
    return true;
}

static bool web_url_append_encoded(char *out, size_t out_len, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    if (text == NULL) {
        return true;
    }

    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        const unsigned char ch = *p;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            if (!web_url_append_char(out, out_len, (char)ch)) {
                return false;
            }
        } else if (ch == ' ') {
            if (!web_url_append_char(out, out_len, '+')) {
                return false;
            }
        } else {
            if (strlen(out) + 3U >= out_len) {
                return false;
            }
            if (!web_url_append_char(out, out_len, '%') ||
                !web_url_append_char(out, out_len, hex[(ch >> 4) & 0x0f]) ||
                !web_url_append_char(out, out_len, hex[ch & 0x0f])) {
                return false;
            }
        }
    }
    return true;
}

static bool web_control_submits_value(const web_control_t *control, int submit_control)
{
    if (control == NULL || control->name[0] == '\0') {
        return false;
    }
    switch (control->type) {
    case WEB_CONTROL_TEXT:
    case WEB_CONTROL_HIDDEN:
        return true;
    case WEB_CONTROL_CHECKBOX:
    case WEB_CONTROL_RADIO:
        return control->checked;
    case WEB_CONTROL_SUBMIT:
        return submit_control >= 0 && control == &web.controls[submit_control];
    default:
        return false;
    }
}

static bool web_submit_form(solar_os_context_t *ctx, int submit_control)
{
    if (submit_control < 0 || submit_control >= (int)web.control_count) {
        return false;
    }

    const web_control_t *submit = &web.controls[submit_control];
    const int form_index = submit->form_index;
    const web_form_t *form = form_index >= 0 && form_index < (int)web.form_count ?
        &web.forms[form_index] : NULL;
    if (form != NULL && strcmp(form->method, "post") == 0) {
        web_set_status("POST forms not supported");
        return true;
    }

    char action[WEB_URL_MAX];
    if (form != NULL && form->action[0] != '\0' && strcmp(form->action, "#") != 0) {
        if (!web_resolve_url(web_current_base_url(), form->action, action, sizeof(action))) {
            web_set_status("bad form action");
            return true;
        }
    } else {
        strlcpy(action, web.url, sizeof(action));
    }

    char next_url[WEB_URL_MAX];
    strlcpy(next_url, action, sizeof(next_url));
    bool first = strchr(next_url, '?') == NULL;
    for (size_t i = 0; i < web.control_count; i++) {
        const web_control_t *control = &web.controls[i];
        if (control->form_index != form_index ||
            !web_control_submits_value(control, submit_control)) {
            continue;
        }

        if (!web_url_append_char(next_url, sizeof(next_url), first ? '?' : '&') ||
            !web_url_append_encoded(next_url, sizeof(next_url), control->name) ||
            !web_url_append_char(next_url, sizeof(next_url), '=') ||
            !web_url_append_encoded(next_url, sizeof(next_url), control->value)) {
            web_set_status("form URL too long");
            return true;
        }
        first = false;
    }

    (void)web_start_load(ctx, next_url, true);
    return true;
}

static void web_begin_edit(int control_index)
{
    if (control_index < 0 || control_index >= (int)web.control_count) {
        return;
    }
    web_control_t *control = &web.controls[control_index];
    if (control->type != WEB_CONTROL_TEXT) {
        return;
    }
    web.editing = true;
    web.edit_control = control_index;
    web.edit_cursor = strlen(control->value);
    strlcpy(web.edit_original, control->value, sizeof(web.edit_original));
    web.redraw = true;
}

static void web_finish_edit(bool cancel)
{
    if (web.edit_control >= 0 && web.edit_control < (int)web.control_count && cancel) {
        strlcpy(web.controls[web.edit_control].value,
                web.edit_original,
                sizeof(web.controls[web.edit_control].value));
    }
    web.editing = false;
    web.edit_control = -1;
    web.edit_cursor = 0;
    web.edit_original[0] = '\0';
    web.redraw = true;
}

static void web_edit_insert(char ch)
{
    if (!web.editing ||
        web.edit_control < 0 ||
        web.edit_control >= (int)web.control_count ||
        !web_is_printable(ch)) {
        return;
    }

    web_control_t *control = &web.controls[web.edit_control];
    const size_t len = strlen(control->value);
    if (len + 1U >= sizeof(control->value)) {
        web_set_status("field full");
        return;
    }
    if (web.edit_cursor > len) {
        web.edit_cursor = len;
    }
    memmove(control->value + web.edit_cursor + 1U,
            control->value + web.edit_cursor,
            len - web.edit_cursor + 1U);
    control->value[web.edit_cursor++] = ch;
    web.redraw = true;
}

static void web_edit_backspace(void)
{
    if (!web.editing ||
        web.edit_control < 0 ||
        web.edit_control >= (int)web.control_count ||
        web.edit_cursor == 0) {
        return;
    }
    web_control_t *control = &web.controls[web.edit_control];
    const size_t len = strlen(control->value);
    memmove(control->value + web.edit_cursor - 1U,
            control->value + web.edit_cursor,
            len - web.edit_cursor + 1U);
    web.edit_cursor--;
    web.redraw = true;
}

static void web_edit_delete(void)
{
    if (!web.editing ||
        web.edit_control < 0 ||
        web.edit_control >= (int)web.control_count) {
        return;
    }
    web_control_t *control = &web.controls[web.edit_control];
    const size_t len = strlen(control->value);
    if (web.edit_cursor >= len) {
        return;
    }
    memmove(control->value + web.edit_cursor,
            control->value + web.edit_cursor + 1U,
            len - web.edit_cursor);
    web.redraw = true;
}

static bool web_handle_edit_key(uint8_t ch)
{
    if (!web.editing) {
        return false;
    }

    switch (ch) {
    case SOLAR_OS_KEY_ESCAPE:
        web_finish_edit(true);
        return true;
    case '\r':
    case '\n':
        web_finish_edit(false);
        return true;
    case '\b':
        web_edit_backspace();
        return true;
    case SOLAR_OS_KEY_DELETE:
        web_edit_delete();
        return true;
    case SOLAR_OS_KEY_LEFT:
        if (web.edit_cursor > 0) {
            web.edit_cursor--;
            web.redraw = true;
        }
        return true;
    case SOLAR_OS_KEY_RIGHT:
        if (web.edit_control >= 0 && web.edit_control < (int)web.control_count) {
            const size_t len = strlen(web.controls[web.edit_control].value);
            if (web.edit_cursor < len) {
                web.edit_cursor++;
                web.redraw = true;
            }
        }
        return true;
    case SOLAR_OS_KEY_HOME:
        web.edit_cursor = 0;
        web.redraw = true;
        return true;
    case SOLAR_OS_KEY_END:
        if (web.edit_control >= 0 && web.edit_control < (int)web.control_count) {
            web.edit_cursor = strlen(web.controls[web.edit_control].value);
            web.redraw = true;
        }
        return true;
    default:
        web_edit_insert((char)ch);
        return true;
    }
}

static bool web_parse_args(solar_os_context_t *ctx, const char **url)
{
    const int argc = solar_os_context_argc(ctx);
    *url = NULL;
    if (argc != 2) {
        return false;
    }
    *url = solar_os_context_argv(ctx, 1);
    return web_url_supported(*url);
}

static esp_err_t web_start(solar_os_context_t *ctx)
{
    memset(&web, 0, sizeof(web));
    web.selected_item = -1;
    web.edit_control = -1;

    const char *url = NULL;
    if (!web_parse_args(ctx, &url)) {
        solar_os_context_set_graphics_active(ctx, true);
        web_set_status("usage: web http://host/");
        web_render(ctx);
        return ESP_OK;
    }

    if (!web_allocate_buffers()) {
        web_free_buffers();
        return ESP_ERR_NO_MEM;
    }

    web.active = true;
    solar_os_context_set_graphics_active(ctx, true);
    (void)web_start_load(ctx, url, false);
    return ESP_OK;
}

static void web_stop(solar_os_context_t *ctx)
{
    web.stop_requested = true;
    if (web.client != NULL) {
        (void)esp_http_client_cancel_request(web.client);
    }
    if (!solar_os_task_wait_done(web.task, &web.task_done, SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "web task did not stop within %u ms",
                      (unsigned)SOLAR_OS_TASK_STOP_WAIT_MS);
    }
    web_free_buffers();
    memset(&web, 0, sizeof(web));
    solar_os_context_set_graphics_active(ctx, false);
}

static bool web_history_back(solar_os_context_t *ctx)
{
    if (web.history_count == 0) {
        web_set_status("no back history");
        return true;
    }

    char url[WEB_URL_MAX];
    strlcpy(url, web.history[web.history_count - 1U], sizeof(url));
    web.history_count--;
    (void)web_start_load(ctx, url, false);
    return true;
}

static void web_select_radio(int control_index)
{
    if (control_index < 0 || control_index >= (int)web.control_count) {
        return;
    }

    web_control_t *selected = &web.controls[control_index];
    for (size_t i = 0; i < web.control_count; i++) {
        web_control_t *control = &web.controls[i];
        if (control->type == WEB_CONTROL_RADIO &&
            control->form_index == selected->form_index &&
            strcmp(control->name, selected->name) == 0) {
            control->checked = control == selected;
        }
    }
}

static bool web_activate_control(solar_os_context_t *ctx, int control_index)
{
    if (control_index < 0 || control_index >= (int)web.control_count) {
        return false;
    }

    web_control_t *control = &web.controls[control_index];
    switch (control->type) {
    case WEB_CONTROL_TEXT:
        web_begin_edit(control_index);
        return true;
    case WEB_CONTROL_CHECKBOX:
        control->checked = !control->checked;
        web.redraw = true;
        return true;
    case WEB_CONTROL_RADIO:
        web_select_radio(control_index);
        web.redraw = true;
        return true;
    case WEB_CONTROL_SUBMIT:
        return web_submit_form(ctx, control_index);
    case WEB_CONTROL_HIDDEN:
    default:
        return false;
    }
}

static bool web_open_selected(solar_os_context_t *ctx)
{
    if (web.selected_item < 0 || web.selected_item >= (int)web.item_count) {
        return false;
    }

    const web_item_t *item = &web.items[web.selected_item];
    if (item->type == WEB_ITEM_CONTROL) {
        return web_activate_control(ctx, item->index);
    }
    if (item->type != WEB_ITEM_LINK ||
        item->index < 0 ||
        item->index >= (int)web.link_count) {
        return false;
    }

    char next_url[WEB_URL_MAX];
    if (!web_resolve_url(web_current_base_url(), web.links[item->index].href, next_url, sizeof(next_url))) {
        web_set_status("cannot resolve link");
        return true;
    }

    (void)web_start_load(ctx, next_url, true);
    return true;
}

static bool web_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        web_drain_events(ctx);
        if (web.redraw) {
            web_render(ctx);
        }
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        web_render(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (web_handle_edit_key(ch)) {
        if (web.redraw) {
            web_render(ctx);
        }
        return true;
    }

    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (web.loading) {
        return true;
    }

    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    bool redraw = false;
    switch (ch) {
    case SOLAR_OS_KEY_UP:
        web.scroll--;
        redraw = true;
        break;
    case SOLAR_OS_KEY_DOWN:
        web.scroll++;
        redraw = true;
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        web.scroll -= web_visible_line_count(gfx);
        redraw = true;
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
    case ' ':
        web.scroll += web_visible_line_count(gfx);
        redraw = true;
        break;
    case SOLAR_OS_KEY_HOME:
        web.scroll = 0;
        redraw = true;
        break;
    case SOLAR_OS_KEY_END:
        web.scroll = (int)web.line_count;
        redraw = true;
        break;
    case '\t':
    case 'n':
        web_select_next_item(gfx, 1);
        redraw = true;
        break;
    case 'p':
        web_select_next_item(gfx, -1);
        redraw = true;
        break;
    case 'b':
    case 'B':
        redraw = web_history_back(ctx);
        redraw = true;
        break;
    case '\r':
    case '\n':
        redraw = web_open_selected(ctx);
        break;
    case 'r':
    case 'R':
        (void)web_start_load(ctx, web.url, false);
        redraw = true;
        break;
    default:
        break;
    }

    if (redraw) {
        web_render(ctx);
    }
    return true;
}

const solar_os_app_t solar_os_web_app = {
    .name = "web",
    .summary = "simple web browser",
    .start = web_start,
    .stop = web_stop,
    .event = web_event,
};
