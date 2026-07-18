#include "solar_os_html.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "solar_os_memory.h"
#include "solar_os_xml.h"

static const char *TAG = "solar_os_html";

typedef struct {
    solar_os_doc_t *doc;
    const char *asset_base;
    char *out;
    size_t len;
    size_t cap;
    bool in_pre;
    bool pending_space;
    char link_stack[8][SOLAR_OS_XML_ATTR_VALUE_MAX];
    size_t link_depth;
    size_t table_depth;
    size_t table_row;
    size_t row_cells;
    size_t skip_depth;
} html_ctx_t;

static void *html_malloc(size_t size)
{
    return solar_os_memory_alloc(size,
                                 SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                 "html");
}

static void html_free(void *ptr)
{
    solar_os_memory_free(ptr);
}

static esp_err_t html_reserve(html_ctx_t *ctx, size_t add_len)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->len + add_len + 1U <= ctx->cap) {
        return ESP_OK;
    }

    size_t next = ctx->cap > 0 ? ctx->cap * 2U : 1024U;
    while (next < ctx->len + add_len + 1U) {
        next *= 2U;
    }

    char *buf = html_malloc(next);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (ctx->out != NULL && ctx->len > 0) {
        memcpy(buf, ctx->out, ctx->len);
    }
    html_free(ctx->out);
    ctx->out = buf;
    ctx->cap = next;
    ctx->out[ctx->len] = '\0';
    return ESP_OK;
}

static esp_err_t html_append_len(html_ctx_t *ctx, const char *text, size_t len)
{
    if (ctx == NULL || (text == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = html_reserve(ctx, len);
    if (ret != ESP_OK) {
        return ret;
    }
    if (len > 0) {
        memcpy(&ctx->out[ctx->len], text, len);
        ctx->len += len;
    }
    ctx->out[ctx->len] = '\0';
    return ESP_OK;
}

static esp_err_t html_append(html_ctx_t *ctx, const char *text)
{
    return html_append_len(ctx, text, text != NULL ? strlen(text) : 0);
}

static esp_err_t html_append_char(html_ctx_t *ctx, char ch)
{
    return html_append_len(ctx, &ch, 1U);
}

static bool html_ends_with(const html_ctx_t *ctx, char ch)
{
    return ctx != NULL && ctx->len > 0 && ctx->out[ctx->len - 1U] == ch;
}

static esp_err_t html_newline(html_ctx_t *ctx)
{
    if (ctx == NULL || ctx->len == 0 || html_ends_with(ctx, '\n')) {
        return ESP_OK;
    }
    ctx->pending_space = false;
    return html_append_char(ctx, '\n');
}

static esp_err_t html_blank(html_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ctx->pending_space = false;
    if (ctx->len == 0) {
        return ESP_OK;
    }
    if (ctx->len >= 2U && ctx->out[ctx->len - 1U] == '\n' && ctx->out[ctx->len - 2U] == '\n') {
        return ESP_OK;
    }
    if (!html_ends_with(ctx, '\n')) {
        ESP_RETURN_ON_ERROR(html_append_char(ctx, '\n'), TAG, "append newline");
    }
    return html_append_char(ctx, '\n');
}

static bool html_block_tag(const char *name)
{
    return solar_os_xml_name_eq(name, "p") ||
        solar_os_xml_name_eq(name, "div") ||
        solar_os_xml_name_eq(name, "section") ||
        solar_os_xml_name_eq(name, "article") ||
        solar_os_xml_name_eq(name, "header") ||
        solar_os_xml_name_eq(name, "footer") ||
        solar_os_xml_name_eq(name, "nav") ||
        solar_os_xml_name_eq(name, "aside");
}

static int html_heading_level(const char *name)
{
    const char *local = solar_os_xml_local_name(name);
    return local[0] == 'h' && local[1] >= '1' && local[1] <= '6' && local[2] == '\0' ?
        local[1] - '0' :
        0;
}

static bool html_ref_has_scheme(const char *ref)
{
    if (ref == NULL) {
        return false;
    }
    for (const char *p = ref; *p != '\0'; p++) {
        if (*p == ':') {
            return true;
        }
        if (*p == '/' || *p == '?' || *p == '#') {
            return false;
        }
    }
    return false;
}

static void html_pop_dir(char *dir)
{
    if (dir == NULL || dir[0] == '\0' || strcmp(dir, "/") == 0) {
        return;
    }
    size_t len = strlen(dir);
    if (len > 0 && dir[len - 1U] == '/') {
        dir[--len] = '\0';
    }
    char *slash = strrchr(dir, '/');
    if (slash == NULL) {
        dir[0] = '\0';
    } else {
        slash[1] = '\0';
    }
}

static esp_err_t html_resolve_asset(const char *base, const char *src, char *out, size_t out_len)
{
    if (src == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t clean_len = strcspn(src, "?#");
    if (clean_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    char clean[SOLAR_OS_XML_ATTR_VALUE_MAX];
    if (clean_len >= sizeof(clean)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(clean, src, clean_len);
    clean[clean_len] = '\0';

    if (base == NULL || base[0] == '\0' || clean[0] == '/' || html_ref_has_scheme(clean)) {
        const char *copy = clean[0] == '/' ? clean + 1 : clean;
        return strlcpy(out, copy, out_len) < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    char dir[SOLAR_OS_XML_ATTR_VALUE_MAX];
    if (strlcpy(dir, base, sizeof(dir)) >= sizeof(dir)) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *slash = strrchr(dir, '/');
    if (slash != NULL) {
        slash[1] = '\0';
    } else {
        dir[0] = '\0';
    }

    const char *rel = clean;
    while (strncmp(rel, "./", 2) == 0) {
        rel += 2;
    }
    while (strncmp(rel, "../", 3) == 0) {
        html_pop_dir(dir);
        rel += 3;
        while (strncmp(rel, "./", 2) == 0) {
            rel += 2;
        }
    }

    const int written = snprintf(out, out_len, "%s%s", dir, rel);
    return written >= 0 && (size_t)written < out_len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t html_text(html_ctx_t *ctx, const char *text, size_t len)
{
    if (ctx == NULL || text == NULL || len == 0) {
        return ESP_OK;
    }
    if (ctx->skip_depth > 0) {
        return ESP_OK;
    }

    if (ctx->in_pre) {
        return html_append_len(ctx, text, len);
    }

    for (size_t i = 0; i < len; i++) {
        const unsigned char ch = (unsigned char)text[i];
        if (isspace(ch)) {
            ctx->pending_space = ctx->len > 0 && !html_ends_with(ctx, '\n');
            continue;
        }
        if (ctx->pending_space) {
            ESP_RETURN_ON_ERROR(html_append_char(ctx, ' '), TAG, "space");
            ctx->pending_space = false;
        }
        ESP_RETURN_ON_ERROR(html_append_char(ctx, (char)ch), TAG, "text");
    }
    return ESP_OK;
}

static const char *html_image_attr(const solar_os_xml_event_t *event)
{
    const char *src = solar_os_xml_attr(event, "src");
    if (src == NULL) {
        src = solar_os_xml_attr(event, "href");
    }
    if (src == NULL) {
        src = solar_os_xml_attr(event, "xlink:href");
    }
    if (src == NULL) {
        src = solar_os_xml_attr(event, "data");
    }
    if (src == NULL) {
        src = solar_os_xml_attr(event, "data-src");
    }
    return src;
}

static bool html_image_tag(const char *name)
{
    return solar_os_xml_name_eq(name, "img") ||
        solar_os_xml_name_eq(name, "image") ||
        solar_os_xml_name_eq(name, "object") ||
        solar_os_xml_name_eq(name, "embed");
}

static esp_err_t html_start(const solar_os_xml_event_t *event, html_ctx_t *ctx)
{
    const char *name = event->name;
    if (ctx->skip_depth > 0) {
        ctx->skip_depth++;
        return ESP_OK;
    }
    if (solar_os_xml_name_eq(name, "head") ||
        solar_os_xml_name_eq(name, "style") ||
        solar_os_xml_name_eq(name, "script") ||
        solar_os_xml_name_eq(name, "metadata")) {
        ctx->skip_depth = 1;
        return ESP_OK;
    }

    const int heading = html_heading_level(name);
    if (heading > 0) {
        ESP_RETURN_ON_ERROR(html_blank(ctx), TAG, "heading gap");
        for (int i = 0; i < heading; i++) {
            ESP_RETURN_ON_ERROR(html_append_char(ctx, '#'), TAG, "heading marker");
        }
        return html_append(ctx, " ");
    }
    if (html_block_tag(name)) {
        return html_blank(ctx);
    }
    if (solar_os_xml_name_eq(name, "br")) {
        return html_newline(ctx);
    }
    if (solar_os_xml_name_eq(name, "hr")) {
        ESP_RETURN_ON_ERROR(html_blank(ctx), TAG, "hr gap");
        ESP_RETURN_ON_ERROR(html_append(ctx, "---"), TAG, "hr");
        return html_blank(ctx);
    }
    if (solar_os_xml_name_eq(name, "blockquote")) {
        ESP_RETURN_ON_ERROR(html_blank(ctx), TAG, "quote gap");
        return html_append(ctx, "> ");
    }
    if (solar_os_xml_name_eq(name, "ul") || solar_os_xml_name_eq(name, "ol")) {
        return html_blank(ctx);
    }
    if (solar_os_xml_name_eq(name, "li")) {
        ESP_RETURN_ON_ERROR(html_newline(ctx), TAG, "li newline");
        return html_append(ctx, "- ");
    }
    if (solar_os_xml_name_eq(name, "pre")) {
        ESP_RETURN_ON_ERROR(html_blank(ctx), TAG, "pre gap");
        ESP_RETURN_ON_ERROR(html_append(ctx, "```\n"), TAG, "pre open");
        ctx->in_pre = true;
        return ESP_OK;
    }
    if (solar_os_xml_name_eq(name, "code") && !ctx->in_pre) {
        return html_append_char(ctx, '`');
    }
    if (solar_os_xml_name_eq(name, "strong") || solar_os_xml_name_eq(name, "b")) {
        return html_append(ctx, "**");
    }
    if (solar_os_xml_name_eq(name, "em") || solar_os_xml_name_eq(name, "i")) {
        return html_append(ctx, "*");
    }
    if (solar_os_xml_name_eq(name, "a")) {
        const char *href = solar_os_xml_attr(event, "href");
        if (ctx->link_depth < sizeof(ctx->link_stack) / sizeof(ctx->link_stack[0])) {
            strlcpy(ctx->link_stack[ctx->link_depth++],
                    href != NULL ? href : "",
                    sizeof(ctx->link_stack[0]));
        }
        return html_append_char(ctx, '[');
    }
    if (html_image_tag(name)) {
        const char *src = html_image_attr(event);
        if (src == NULL) {
            return ESP_OK;
        }
        char resolved[SOLAR_OS_XML_ATTR_VALUE_MAX];
        if (html_resolve_asset(ctx->asset_base, src, resolved, sizeof(resolved)) == ESP_OK) {
            src = resolved;
        }
        ESP_RETURN_ON_ERROR(html_blank(ctx), TAG, "image gap");
        ESP_RETURN_ON_ERROR(html_append(ctx, "![]("), TAG, "image open");
        ESP_RETURN_ON_ERROR(html_append(ctx, src), TAG, "image src");
        ESP_RETURN_ON_ERROR(html_append(ctx, ")"), TAG, "image close");
        return html_blank(ctx);
    }
    if (solar_os_xml_name_eq(name, "table")) {
        ctx->table_depth++;
        ctx->table_row = 0;
        return html_blank(ctx);
    }
    if (solar_os_xml_name_eq(name, "tr")) {
        ctx->row_cells = 0;
        return html_newline(ctx);
    }
    if (solar_os_xml_name_eq(name, "td") || solar_os_xml_name_eq(name, "th")) {
        ctx->row_cells++;
        ESP_RETURN_ON_ERROR(html_append(ctx, "| "), TAG, "cell");
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t html_end(const solar_os_xml_event_t *event, html_ctx_t *ctx)
{
    const char *name = event->name;
    if (ctx->skip_depth > 0) {
        ctx->skip_depth--;
        return ESP_OK;
    }
    if (html_heading_level(name) > 0 || html_block_tag(name) || solar_os_xml_name_eq(name, "blockquote")) {
        return html_blank(ctx);
    }
    if (solar_os_xml_name_eq(name, "li")) {
        return html_newline(ctx);
    }
    if (solar_os_xml_name_eq(name, "pre")) {
        ctx->in_pre = false;
        ESP_RETURN_ON_ERROR(html_newline(ctx), TAG, "pre newline");
        ESP_RETURN_ON_ERROR(html_append(ctx, "```"), TAG, "pre close");
        return html_blank(ctx);
    }
    if (solar_os_xml_name_eq(name, "code") && !ctx->in_pre) {
        return html_append_char(ctx, '`');
    }
    if (solar_os_xml_name_eq(name, "strong") || solar_os_xml_name_eq(name, "b")) {
        return html_append(ctx, "**");
    }
    if (solar_os_xml_name_eq(name, "em") || solar_os_xml_name_eq(name, "i")) {
        return html_append(ctx, "*");
    }
    if (solar_os_xml_name_eq(name, "a")) {
        const char *href = "";
        if (ctx->link_depth > 0) {
            href = ctx->link_stack[--ctx->link_depth];
        }
        ESP_RETURN_ON_ERROR(html_append(ctx, "]("), TAG, "link close");
        ESP_RETURN_ON_ERROR(html_append(ctx, href), TAG, "link href");
        return html_append(ctx, ")");
    }
    if (solar_os_xml_name_eq(name, "td") || solar_os_xml_name_eq(name, "th")) {
        return html_append_char(ctx, ' ');
    }
    if (solar_os_xml_name_eq(name, "tr")) {
        ESP_RETURN_ON_ERROR(html_append(ctx, "|"), TAG, "row end");
        ESP_RETURN_ON_ERROR(html_newline(ctx), TAG, "row newline");
        if (ctx->table_depth > 0 && ctx->table_row == 0 && ctx->row_cells > 0) {
            for (size_t i = 0; i < ctx->row_cells; i++) {
                ESP_RETURN_ON_ERROR(html_append(ctx, "| --- "), TAG, "table delimiter");
            }
            ESP_RETURN_ON_ERROR(html_append(ctx, "|"), TAG, "table delimiter end");
            ESP_RETURN_ON_ERROR(html_newline(ctx), TAG, "table delimiter newline");
        }
        ctx->table_row++;
        return ESP_OK;
    }
    if (solar_os_xml_name_eq(name, "table")) {
        if (ctx->table_depth > 0) {
            ctx->table_depth--;
        }
        return html_blank(ctx);
    }
    return ESP_OK;
}

static esp_err_t html_xml_event(const solar_os_xml_event_t *event, void *user)
{
    html_ctx_t *ctx = (html_ctx_t *)user;
    if (event == NULL || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (event->type) {
    case SOLAR_OS_XML_EVENT_START:
        return html_start(event, ctx);
    case SOLAR_OS_XML_EVENT_END:
        return html_end(event, ctx);
    case SOLAR_OS_XML_EVENT_TEXT:
    case SOLAR_OS_XML_EVENT_CDATA:
        return html_text(ctx, event->text, event->text_len);
    case SOLAR_OS_XML_EVENT_COMMENT:
    case SOLAR_OS_XML_EVENT_PI:
    default:
        return ESP_OK;
    }
}

esp_err_t solar_os_html_parse_doc(solar_os_doc_t *doc,
                                  const char *source,
                                  size_t source_len,
                                  const char *path)
{
    if (doc == NULL || (source == NULL && source_len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    char *markdown = NULL;
    size_t markdown_len = 0;
    esp_err_t ret = solar_os_html_to_markdown(source, source_len, NULL, &markdown, &markdown_len);
    if (ret == ESP_OK) {
        ret = solar_os_doc_parse_markdown(doc,
                                          markdown != NULL ? markdown : "",
                                          markdown_len,
                                          path);
    }

    solar_os_html_free(markdown);
    return ret;
}

esp_err_t solar_os_html_to_markdown(const char *source,
                                    size_t source_len,
                                    const char *asset_base,
                                    char **out_markdown,
                                    size_t *out_len)
{
    if ((source == NULL && source_len > 0) || out_markdown == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_markdown = NULL;
    *out_len = 0;

    html_ctx_t ctx = {
        .asset_base = asset_base,
    };
    esp_err_t ret = solar_os_xml_parse(source, source_len, html_xml_event, &ctx);
    if (ret != ESP_OK) {
        html_free(ctx.out);
        return ret;
    }

    *out_markdown = ctx.out;
    *out_len = ctx.len;
    return ESP_OK;
}

void solar_os_html_free(char *markdown)
{
    html_free(markdown);
}
