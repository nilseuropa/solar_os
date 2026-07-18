#include "solar_os_xml.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "solar_os_memory.h"

#define XML_TAG_MAX 768U

static void *xml_malloc(size_t size)
{
    return solar_os_memory_alloc(size,
                                 SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                 "xml");
}

static void xml_free(void *ptr)
{
    solar_os_memory_free(ptr);
}

const char *solar_os_xml_local_name(const char *name)
{
    if (name == NULL) {
        return "";
    }
    const char *colon = strrchr(name, ':');
    return colon != NULL ? colon + 1 : name;
}

bool solar_os_xml_name_eq(const char *name, const char *local_name)
{
    return name != NULL &&
        local_name != NULL &&
        strcasecmp(solar_os_xml_local_name(name), local_name) == 0;
}

const char *solar_os_xml_attr(const solar_os_xml_event_t *event, const char *name)
{
    if (event == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < event->attr_count; i++) {
        if (strcasecmp(event->attrs[i].name, name) == 0 ||
            strcasecmp(solar_os_xml_local_name(event->attrs[i].name), name) == 0) {
            return event->attrs[i].value;
        }
    }
    return NULL;
}

static bool xml_append_char(char *out, size_t out_len, size_t *used, char ch)
{
    if (out == NULL || used == NULL || *used + 1U >= out_len) {
        return false;
    }
    out[(*used)++] = ch;
    out[*used] = '\0';
    return true;
}

static bool xml_append_utf8_or_ascii(char *out, size_t out_len, size_t *used, uint32_t value)
{
    if (value == 0) {
        return true;
    }
    if (value < 0x80U) {
        return xml_append_char(out, out_len, used, (char)value);
    }
    return xml_append_char(out, out_len, used, '?');
}

static bool xml_append_text(char *out, size_t out_len, size_t *used, const char *text)
{
    if (text == NULL) {
        return true;
    }
    while (*text != '\0') {
        if (!xml_append_char(out, out_len, used, *text++)) {
            return false;
        }
    }
    return true;
}

static bool xml_decode_entity(const char *source,
                              size_t source_len,
                              size_t start,
                              size_t *next,
                              char *out,
                              size_t out_len,
                              size_t *used)
{
    size_t end = start;
    while (end < source_len && source[end] != ';' && end - start < 16U) {
        end++;
    }
    if (end >= source_len || source[end] != ';') {
        return false;
    }

    const size_t len = end - start;
    if (len == 2 && strncasecmp(&source[start], "lt", len) == 0) {
        *next = end + 1U;
        return xml_append_char(out, out_len, used, '<');
    }
    if (len == 2 && strncasecmp(&source[start], "gt", len) == 0) {
        *next = end + 1U;
        return xml_append_char(out, out_len, used, '>');
    }
    if (len == 3 && strncasecmp(&source[start], "amp", len) == 0) {
        *next = end + 1U;
        return xml_append_char(out, out_len, used, '&');
    }
    if (len == 4 && strncasecmp(&source[start], "quot", len) == 0) {
        *next = end + 1U;
        return xml_append_char(out, out_len, used, '"');
    }
    if (len == 4 && strncasecmp(&source[start], "apos", len) == 0) {
        *next = end + 1U;
        return xml_append_char(out, out_len, used, '\'');
    }
    if (len == 4 && strncasecmp(&source[start], "nbsp", len) == 0) {
        *next = end + 1U;
        return xml_append_char(out, out_len, used, ' ');
    }

    static const struct {
        const char *name;
        const char *replacement;
    } html_entities[] = {
        {"lsquo", "'"},
        {"rsquo", "'"},
        {"ldquo", "\""},
        {"rdquo", "\""},
        {"ndash", "-"},
        {"mdash", "-"},
        {"hellip", "..."},
        {"bull", "*"},
        {"copy", "(c)"},
        {"reg", "(r)"},
        {"trade", "TM"},
    };
    for (size_t i = 0; i < sizeof(html_entities) / sizeof(html_entities[0]); i++) {
        if (strlen(html_entities[i].name) == len &&
            strncasecmp(&source[start], html_entities[i].name, len) == 0) {
            *next = end + 1U;
            return xml_append_text(out, out_len, used, html_entities[i].replacement);
        }
    }

    if (len > 1U && source[start] == '#') {
        uint32_t value = 0;
        size_t pos = start + 1U;
        const bool hex = pos < end && (source[pos] == 'x' || source[pos] == 'X');
        if (hex) {
            pos++;
        }
        for (; pos < end; pos++) {
            const unsigned char ch = (unsigned char)source[pos];
            if (hex && isxdigit(ch)) {
                value *= 16U;
                value += (uint32_t)(isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10);
            } else if (!hex && isdigit(ch)) {
                value = value * 10U + (uint32_t)(ch - '0');
            } else {
                return false;
            }
        }
        *next = end + 1U;
        return xml_append_utf8_or_ascii(out, out_len, used, value);
    }

    return false;
}

esp_err_t solar_os_xml_decode_entities(const char *source,
                                       size_t source_len,
                                       char *out,
                                       size_t out_len)
{
    if ((source == NULL && source_len > 0) || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    size_t used = 0;
    for (size_t i = 0; i < source_len;) {
        if (source[i] == '&') {
            size_t next = i + 1U;
            if (xml_decode_entity(source, source_len, i + 1U, &next, out, out_len, &used)) {
                i = next;
                continue;
            }
        }
        if (!xml_append_char(out, out_len, &used, source[i])) {
            return ESP_ERR_INVALID_SIZE;
        }
        i++;
    }
    return ESP_OK;
}

static bool xml_name_char(char ch, bool first)
{
    const unsigned char c = (unsigned char)ch;
    return isalpha(c) || ch == '_' || ch == ':' ||
        (!first && (isdigit(c) || ch == '-' || ch == '.'));
}

static void xml_copy_name(const char **cursor, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (cursor == NULL || *cursor == NULL) {
        return;
    }

    size_t used = 0;
    bool first = true;
    while (**cursor != '\0' && xml_name_char(**cursor, first)) {
        if (used + 1U < out_len) {
            out[used++] = **cursor;
        }
        (*cursor)++;
        first = false;
    }
    out[used] = '\0';
}

static void xml_skip_space(const char **cursor)
{
    while (cursor != NULL && *cursor != NULL && isspace((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

static esp_err_t xml_copy_attr_value(const char **cursor, char *out, size_t out_len)
{
    if (cursor == NULL || *cursor == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    char raw[SOLAR_OS_XML_ATTR_VALUE_MAX];
    size_t raw_len = 0;
    char quote = '\0';
    if (**cursor == '"' || **cursor == '\'') {
        quote = **cursor;
        (*cursor)++;
    }

    while (**cursor != '\0') {
        if (quote != '\0') {
            if (**cursor == quote) {
                (*cursor)++;
                break;
            }
        } else if (isspace((unsigned char)**cursor) || **cursor == '/') {
            break;
        }
        if (raw_len + 1U < sizeof(raw)) {
            raw[raw_len++] = **cursor;
        }
        (*cursor)++;
    }
    raw[raw_len] = '\0';
    return solar_os_xml_decode_entities(raw, raw_len, out, out_len);
}

static esp_err_t xml_parse_start_tag(char *tag,
                                     solar_os_xml_event_cb_t callback,
                                     void *user)
{
    const char *cursor = tag;
    xml_skip_space(&cursor);

    char name[SOLAR_OS_XML_NAME_MAX];
    xml_copy_name(&cursor, name, sizeof(name));
    if (name[0] == '\0') {
        return ESP_ERR_INVALID_RESPONSE;
    }

    solar_os_xml_attr_t *attrs = xml_malloc(sizeof(attrs[0]) * SOLAR_OS_XML_MAX_ATTRS);
    if (attrs == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(attrs, 0, sizeof(attrs[0]) * SOLAR_OS_XML_MAX_ATTRS);
    size_t attr_count = 0;
    bool self_closing = false;
    esp_err_t ret = ESP_OK;

    while (*cursor != '\0') {
        xml_skip_space(&cursor);
        if (*cursor == '\0') {
            break;
        }
        if (*cursor == '/') {
            self_closing = true;
            cursor++;
            xml_skip_space(&cursor);
            if (*cursor != '\0') {
                ret = ESP_ERR_INVALID_RESPONSE;
                goto done;
            }
            break;
        }

        char attr_name[SOLAR_OS_XML_NAME_MAX];
        xml_copy_name(&cursor, attr_name, sizeof(attr_name));
        if (attr_name[0] == '\0') {
            ret = ESP_ERR_INVALID_RESPONSE;
            goto done;
        }
        xml_skip_space(&cursor);
        if (*cursor != '=') {
            ret = ESP_ERR_INVALID_RESPONSE;
            goto done;
        }
        cursor++;
        xml_skip_space(&cursor);

        char value[SOLAR_OS_XML_ATTR_VALUE_MAX];
        ret = xml_copy_attr_value(&cursor, value, sizeof(value));
        if (ret != ESP_OK) {
            goto done;
        }
        if (attr_count < SOLAR_OS_XML_MAX_ATTRS) {
            strlcpy(attrs[attr_count].name, attr_name, sizeof(attrs[attr_count].name));
            strlcpy(attrs[attr_count].value, value, sizeof(attrs[attr_count].value));
            attr_count++;
        }
    }

    solar_os_xml_event_t event = {
        .type = SOLAR_OS_XML_EVENT_START,
        .name = name,
        .attrs = attrs,
        .attr_count = attr_count,
        .self_closing = self_closing,
    };
    ret = callback(&event, user);
    if (ret != ESP_OK || !self_closing) {
        goto done;
    }

    solar_os_xml_event_t end_event = {
        .type = SOLAR_OS_XML_EVENT_END,
        .name = name,
    };
    ret = callback(&end_event, user);

done:
    xml_free(attrs);
    return ret;
}

static esp_err_t xml_emit_text(solar_os_xml_event_type_t type,
                               const char *source,
                               size_t len,
                               bool decode,
                               solar_os_xml_event_cb_t callback,
                               void *user)
{
    char *text = xml_malloc(len + 1U);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    if (decode) {
        ret = solar_os_xml_decode_entities(source, len, text, len + 1U);
    } else {
        if (len > 0) {
            memcpy(text, source, len);
        }
        text[len] = '\0';
    }
    if (ret == ESP_OK) {
        solar_os_xml_event_t event = {
            .type = type,
            .text = text,
            .text_len = strlen(text),
        };
        ret = callback(&event, user);
    }

    xml_free(text);
    return ret;
}

static size_t xml_find_token(const char *source, size_t source_len, size_t start, const char *token)
{
    const size_t token_len = token != NULL ? strlen(token) : 0;
    if (token_len == 0 || start >= source_len) {
        return SIZE_MAX;
    }
    for (size_t i = start; i + token_len <= source_len; i++) {
        if (memcmp(&source[i], token, token_len) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t xml_find_tag_end(const char *source, size_t source_len, size_t start)
{
    bool quoted = false;
    char quote = '\0';
    for (size_t i = start; i < source_len; i++) {
        const char ch = source[i];
        if (quoted) {
            if (ch == quote) {
                quoted = false;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quoted = true;
            quote = ch;
            continue;
        }
        if (ch == '>') {
            return i;
        }
    }
    return SIZE_MAX;
}

esp_err_t solar_os_xml_parse(const char *source,
                             size_t source_len,
                             solar_os_xml_event_cb_t callback,
                             void *user)
{
    if ((source == NULL && source_len > 0) || callback == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < source_len;) {
        if (source[i] != '<') {
            const size_t start = i;
            while (i < source_len && source[i] != '<') {
                i++;
            }
            if (i > start) {
                esp_err_t ret = xml_emit_text(SOLAR_OS_XML_EVENT_TEXT,
                                              &source[start],
                                              i - start,
                                              true,
                                              callback,
                                              user);
                if (ret != ESP_OK) {
                    return ret;
                }
            }
            continue;
        }

        if (i + 4U <= source_len && memcmp(&source[i], "<!--", 4) == 0) {
            const size_t end = xml_find_token(source, source_len, i + 4U, "-->");
            if (end == SIZE_MAX) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            esp_err_t ret = xml_emit_text(SOLAR_OS_XML_EVENT_COMMENT,
                                          &source[i + 4U],
                                          end - i - 4U,
                                          false,
                                          callback,
                                          user);
            if (ret != ESP_OK) {
                return ret;
            }
            i = end + 3U;
            continue;
        }

        if (i + 9U <= source_len && memcmp(&source[i], "<![CDATA[", 9) == 0) {
            const size_t end = xml_find_token(source, source_len, i + 9U, "]]>");
            if (end == SIZE_MAX) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            esp_err_t ret = xml_emit_text(SOLAR_OS_XML_EVENT_CDATA,
                                          &source[i + 9U],
                                          end - i - 9U,
                                          false,
                                          callback,
                                          user);
            if (ret != ESP_OK) {
                return ret;
            }
            i = end + 3U;
            continue;
        }

        if (i + 2U <= source_len && source[i + 1U] == '?') {
            const size_t end = xml_find_token(source, source_len, i + 2U, "?>");
            if (end == SIZE_MAX) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            esp_err_t ret = xml_emit_text(SOLAR_OS_XML_EVENT_PI,
                                          &source[i + 2U],
                                          end - i - 2U,
                                          false,
                                          callback,
                                          user);
            if (ret != ESP_OK) {
                return ret;
            }
            i = end + 2U;
            continue;
        }

        if (i + 2U <= source_len && source[i + 1U] == '!') {
            const size_t end = xml_find_tag_end(source, source_len, i + 2U);
            if (end == SIZE_MAX) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            i = end + 1U;
            continue;
        }

        const size_t end = xml_find_tag_end(source, source_len, i + 1U);
        if (end == SIZE_MAX) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const size_t tag_len = end - i - 1U;
        if (tag_len == 0 || tag_len >= XML_TAG_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }

        char *tag = xml_malloc(tag_len + 1U);
        if (tag == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(tag, &source[i + 1U], tag_len);
        tag[tag_len] = '\0';

        esp_err_t ret = ESP_OK;
        const char *cursor = tag;
        xml_skip_space(&cursor);
        if (*cursor == '/') {
            cursor++;
            xml_skip_space(&cursor);
            char name[SOLAR_OS_XML_NAME_MAX];
            xml_copy_name(&cursor, name, sizeof(name));
            if (name[0] == '\0') {
                ret = ESP_ERR_INVALID_RESPONSE;
            } else {
                solar_os_xml_event_t event = {
                    .type = SOLAR_OS_XML_EVENT_END,
                    .name = name,
                };
                ret = callback(&event, user);
            }
        } else {
            ret = xml_parse_start_tag(tag, callback, user);
        }

        xml_free(tag);
        if (ret != ESP_OK) {
            return ret;
        }
        i = end + 1U;
    }

    return ESP_OK;
}
