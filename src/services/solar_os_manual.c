#include "solar_os_manual.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
#include "solar_os_docs.h"
#endif
#include "solar_os_memory.h"

#define MANUAL_SEARCH_MAX 12U
#define MANUAL_TOKEN_MAX 31U

#include "solar_os_manual_data.h"

static void manual_use_embedded(const char *text,
                                const char **result,
                                size_t *result_len,
                                bool *owned)
{
    *result = text;
    *result_len = text != NULL ? strlen(text) : 0U;
    *owned = false;
}

#if SOLAR_OS_PACKAGE_SERVICE_DOCS
static esp_err_t manual_read_external(const char *id, char **source, size_t *source_len)
{
    return solar_os_docs_load_page(id, source, source_len);
}

static char *manual_markdown_body(char *source)
{
    if (source == NULL || strncmp(source, "+++\n", 4U) != 0) {
        return NULL;
    }
    char *end = strstr(source + 4U, "\n+++\n");
    return end != NULL ? end + 5U : NULL;
}

static esp_err_t manual_extract_markdown(char *source,
                                         size_t source_len,
                                         const char **result,
                                         size_t *result_len,
                                         bool *owned)
{
    char *body = manual_markdown_body(source);
    if (body == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t body_len = source_len - (size_t)(body - source);
    memmove(source, body, body_len);
    source[body_len] = '\0';
    *result = source;
    *result_len = body_len;
    *owned = true;
    return ESP_OK;
}

static bool manual_heading(const char *line,
                           size_t line_len,
                           size_t *level,
                           const char **text,
                           size_t *text_len)
{
    size_t hashes = 0U;
    while (hashes < line_len && hashes < 6U && line[hashes] == '#') {
        hashes++;
    }
    if (hashes == 0U || hashes >= line_len || line[hashes] != ' ') {
        return false;
    }
    *level = hashes;
    *text = line + hashes + 1U;
    *text_len = line_len - hashes - 1U;
    return true;
}

static bool manual_heading_equals(const char *text,
                                  size_t text_len,
                                  const char *expected)
{
    const size_t expected_len = strlen(expected);
    return text_len == expected_len &&
           strncasecmp(text, expected, expected_len) == 0;
}

static esp_err_t manual_extract_reference(char *source,
                                          const char **result,
                                          size_t *result_len,
                                          bool *owned)
{
    char *body = manual_markdown_body(source);
    if (body == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    char *cursor = body;
    char *start = NULL;
    size_t section_level = 0U;
    while (*cursor != '\0') {
        char *end = strchr(cursor, '\n');
        const size_t len = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        size_t level = 0U;
        size_t text_len = 0U;
        const char *text = NULL;
        if (manual_heading(cursor, len, &level, &text, &text_len)) {
            if (start == NULL && manual_heading_equals(text, text_len, "Quick reference")) {
                start = end != NULL ? end + 1U : cursor + len;
                section_level = level;
            } else if (start != NULL && level <= section_level) {
                *cursor = '\0';
                break;
            }
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1U;
    }
    if (start == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t len = strlen(start);
    while (len > 0U && (start[len - 1U] == '\n' || start[len - 1U] == '\r')) {
        len--;
    }
    char *copy = solar_os_memory_alloc(len + 1U,
                                       SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                       "manual.reference");
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, start, len);
    copy[len] = '\0';
    *result = copy;
    *result_len = len;
    *owned = true;
    return ESP_OK;
}

static esp_err_t manual_render_markdown(char *source,
                                        size_t source_len,
                                        const char **result,
                                        size_t *result_len,
                                        bool *owned)
{
    char *body = manual_markdown_body(source);
    if (body == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t body_len = source_len - (size_t)(body - source);
    const size_t output_capacity = body_len * 3U + 2U;
    char *output = solar_os_memory_alloc(output_capacity,
                                         SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                         "manual.body");
    if (output == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t out = 0U;
    bool fenced = false;
    bool paragraph = false;
    for (char *cursor = body; *cursor != '\0';) {
        char *end = strchr(cursor, '\n');
        size_t len = end != NULL ? (size_t)(end - cursor) : strlen(cursor);
        if (len > 0U && cursor[len - 1U] == '\r') {
            len--;
        }
        if (len >= 3U && strncmp(cursor, "```", 3U) == 0) {
            if (paragraph && out > 0U) {
                output[out++] = '\n';
            }
            paragraph = false;
            fenced = !fenced;
        } else if (fenced) {
            if (out + len + 3U > output_capacity) {
                solar_os_memory_free(output);
                return ESP_ERR_INVALID_SIZE;
            }
            output[out++] = ' ';
            output[out++] = ' ';
            memcpy(output + out, cursor, len);
            out += len;
            output[out++] = '\n';
        } else {
            size_t level = 0U;
            size_t text_len = 0U;
            const char *text = NULL;
            if (manual_heading(cursor, len, &level, &text, &text_len)) {
                if (paragraph && out > 0U) {
                    output[out++] = '\n';
                }
                if (out > 0U && output[out - 1U] != '\n') {
                    output[out++] = '\n';
                }
                for (size_t i = 0U; i < text_len; i++) {
                    output[out++] = (char)toupper((unsigned char)text[i]);
                }
                output[out++] = '\n';
                output[out++] = '\n';
                paragraph = false;
            } else if (len == 0U) {
                if (paragraph && out > 0U) {
                    output[out++] = '\n';
                }
                if (out > 0U && output[out - 1U] != '\n') {
                    output[out++] = '\n';
                }
                paragraph = false;
            } else {
                size_t first = 0U;
                while (first < len && isspace((unsigned char)cursor[first])) {
                    first++;
                }
                if (paragraph && out > 0U && output[out - 1U] != '\n') {
                    output[out++] = ' ';
                }
                for (size_t i = first; i < len; i++) {
                    if (cursor[i] == '`') {
                        continue;
                    }
                    if (cursor[i] == '*' && i + 1U < len && cursor[i + 1U] == '*') {
                        i++;
                        continue;
                    }
                    output[out++] = cursor[i];
                }
                paragraph = true;
            }
        }
        if (end == NULL) {
            break;
        }
        cursor = end + 1U;
    }
    while (out > 0U && output[out - 1U] == '\n') {
        out--;
    }
    output[out++] = '\n';
    output[out] = '\0';
    *result = output;
    *result_len = out;
    *owned = true;
    return ESP_OK;
}
#endif

esp_err_t solar_os_manual_load_markdown(const solar_os_manual_page_t *page,
                                        const char **markdown,
                                        size_t *markdown_len,
                                        bool *owned)
{
    if (page == NULL || markdown == NULL || markdown_len == NULL ||
        owned == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    char *source = NULL;
    size_t source_len = 0U;
    if (manual_read_external(page->id, &source, &source_len) == ESP_OK) {
        const esp_err_t err =
            manual_extract_markdown(source,
                                    source_len,
                                    markdown,
                                    markdown_len,
                                    owned);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        solar_os_memory_free(source);
    }
#endif
    manual_use_embedded(page->markdown, markdown, markdown_len, owned);
    return page->markdown != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_manual_load_body(const solar_os_manual_page_t *page,
                                    const char **body,
                                    size_t *body_len,
                                    bool *owned)
{
    if (page == NULL || body == NULL || body_len == NULL || owned == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    char *source = NULL;
    size_t source_len = 0U;
    if (manual_read_external(page->id, &source, &source_len) == ESP_OK) {
        const esp_err_t err =
            manual_render_markdown(source, source_len, body, body_len, owned);
        solar_os_memory_free(source);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
#endif
    manual_use_embedded(page->body, body, body_len, owned);
    return page->body != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_manual_load_contract(const solar_os_manual_page_t *page,
                                        const char **contract,
                                        size_t *contract_len,
                                        bool *owned)
{
    if (page == NULL || contract == NULL || contract_len == NULL || owned == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    char *source = NULL;
    size_t source_len = 0U;
    if (manual_read_external(page->id, &source, &source_len) == ESP_OK) {
        const esp_err_t err =
            manual_extract_reference(source, contract, contract_len, owned);
        solar_os_memory_free(source);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
#endif
    manual_use_embedded(page->contract, contract, contract_len, owned);
    return page->contract != NULL ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void solar_os_manual_release_text(const char *text, bool owned)
{
    if (owned) {
        solar_os_memory_free((void *)text);
    }
}

size_t solar_os_manual_count(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    const size_t external_count = solar_os_docs_manual_count();
    if (external_count > 0U) {
        return external_count;
    }
#endif
    return solar_os_manual_embedded_count();
}

const solar_os_manual_page_t *solar_os_manual_get(size_t index)
{
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    const solar_os_manual_page_t *external = solar_os_docs_manual_get(index);
    if (external != NULL) {
        return external;
    }
#endif
    return solar_os_manual_embedded_get(index);
}

size_t solar_os_manual_embedded_count(void)
{
    return SOLAR_OS_MANUAL_GENERATED_PAGE_COUNT;
}

const solar_os_manual_page_t *solar_os_manual_embedded_get(size_t index)
{
    return index < solar_os_manual_embedded_count() ?
        &SOLAR_OS_MANUAL_GENERATED_PAGES[index] : NULL;
}

static bool manual_alias_matches(const char *aliases, const char *name)
{
    if (aliases == NULL || name == NULL || name[0] == '\0') {
        return false;
    }
    const size_t name_len = strlen(name);
    const char *alias = aliases;
    while (*alias != '\0') {
        const char *end = strchr(alias, '\n');
        const size_t alias_len = end != NULL ? (size_t)(end - alias) : strlen(alias);
        if (alias_len == name_len && strncasecmp(alias, name, name_len) == 0) {
            return true;
        }
        if (end == NULL) {
            break;
        }
        alias = end + 1;
    }
    return false;
}

const solar_os_manual_page_t *solar_os_manual_find(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < solar_os_manual_count(); i++) {
        const solar_os_manual_page_t *page = solar_os_manual_get(i);
        if (page != NULL && strcasecmp(page->id, name) == 0) {
            return page;
        }
    }

    const solar_os_manual_page_t *match = NULL;
    for (size_t i = 0; i < solar_os_manual_count(); i++) {
        const solar_os_manual_page_t *page = solar_os_manual_get(i);
        if (page == NULL || !manual_alias_matches(page->aliases, name)) {
            continue;
        }
        if (match != NULL) {
            return NULL;
        }
        match = page;
    }
    return match;
}

static bool manual_contains_ci(const char *text, const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    const size_t needle_len = strlen(needle);
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        if (strncasecmp(cursor, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool manual_contains_ci_n(const char *text,
                                 size_t text_len,
                                 const char *needle)
{
    if (text == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    const size_t needle_len = strlen(needle);
    if (needle_len > text_len) {
        return false;
    }
    for (size_t offset = 0U; offset + needle_len <= text_len; offset++) {
        if (strncasecmp(text + offset, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool manual_stop_word(const char *token)
{
    static const char *const words[] = {
        "a", "an", "and", "for", "how", "in", "of", "on", "the", "to", "use", "with",
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strcmp(token, words[i]) == 0) {
            return true;
        }
    }
    return false;
}

static unsigned manual_score(const solar_os_manual_page_t *page, const char *query)
{
    if (page == NULL || query == NULL || query[0] == '\0') {
        return 0U;
    }
    if (strcasecmp(page->id, query) == 0) {
        return 100000U;
    }
    if (manual_alias_matches(page->aliases, query)) {
        return 50000U;
    }

    unsigned score = 0U;
    char token[MANUAL_TOKEN_MAX + 1U];
    size_t token_len = 0U;
    for (const unsigned char *cursor = (const unsigned char *)query;; cursor++) {
        const bool separator = *cursor == '\0' || !isalnum(*cursor);
        if (!separator && token_len < MANUAL_TOKEN_MAX) {
            token[token_len++] = (char)tolower(*cursor);
        }
        if (separator && token_len > 0U) {
            token[token_len] = '\0';
            if (!manual_stop_word(token)) {
                if (manual_contains_ci(page->id, token)) {
                    score += 900U;
                }
                if (manual_contains_ci(page->aliases, token)) {
                    score += 700U;
                }
                if (manual_contains_ci(page->title, token)) {
                    score += 500U;
                }
                if (manual_contains_ci(page->keywords, token)) {
                    score += 300U;
                }
                if (manual_contains_ci(page->summary, token)) {
                    score += 100U;
                }
                if (manual_contains_ci(page->contract, token)) {
                    score += 20U;
                }
            }
            token_len = 0U;
        }
        if (*cursor == '\0') {
            break;
        }
    }
    return score;
}

size_t solar_os_manual_search(const char *query,
                              const solar_os_manual_page_t **results,
                              size_t capacity)
{
    if (query == NULL || query[0] == '\0' || results == NULL || capacity == 0U) {
        return 0U;
    }
    if (capacity > MANUAL_SEARCH_MAX) {
        capacity = MANUAL_SEARCH_MAX;
    }

    unsigned scores[MANUAL_SEARCH_MAX] = {0};
    size_t count = 0U;
    for (size_t candidate = 0U; candidate < solar_os_manual_count(); candidate++) {
        const solar_os_manual_page_t *page = solar_os_manual_get(candidate);
        const unsigned score = manual_score(page, query);
        if (score == 0U) {
            continue;
        }
        size_t insert = 0U;
        while (insert < count &&
               (scores[insert] > score ||
                (scores[insert] == score &&
                 strcmp(results[insert]->id, page->id) < 0))) {
            insert++;
        }
        if (insert >= capacity) {
            continue;
        }
        if (count < capacity) {
            count++;
        }
        for (size_t move = count - 1U; move > insert; move--) {
            results[move] = results[move - 1U];
            scores[move] = scores[move - 1U];
        }
        results[insert] = page;
        scores[insert] = score;
    }
    return count;
}

size_t solar_os_manual_reference_count(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    /* Embedded byte offsets cannot describe downloaded page revisions. The
     * agent falls back to the signed per-page Quick Reference metadata. */
    if (solar_os_docs_manual_index_available()) {
        return 0U;
    }
#endif
    return SOLAR_OS_MANUAL_GENERATED_REFERENCE_COUNT;
}

const solar_os_manual_reference_t *solar_os_manual_reference_get(size_t index)
{
    if (index >= solar_os_manual_reference_count()) {
        return NULL;
    }
    return &SOLAR_OS_MANUAL_GENERATED_REFERENCES[index + 1U];
}

esp_err_t solar_os_manual_reference_text(
    const solar_os_manual_reference_t *reference,
    const char **text,
    size_t *text_len)
{
    if (reference == NULL || text == NULL || text_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const solar_os_manual_page_t *page = solar_os_manual_find(reference->page_id);
    if (page == NULL || page->body == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t body_len = strlen(page->body);
    if ((size_t)reference->offset > body_len ||
        (size_t)reference->length > body_len - (size_t)reference->offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    *text = page->body + reference->offset;
    *text_len = reference->length;
    return ESP_OK;
}

static bool manual_reference_qualifier(const char *token)
{
    static const char *const qualifiers[] = {
        "api", "lua", "micropython", "python", "script", "scripting", "solaros",
    };
    for (size_t i = 0U; i < sizeof(qualifiers) / sizeof(qualifiers[0]); i++) {
        if (strcmp(token, qualifiers[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool manual_reference_language_matches(
    const solar_os_manual_reference_t *reference,
    const char *token)
{
    if (strcmp(token, "python") == 0 || strcmp(token, "micropython") == 0) {
        return strcmp(reference->page_id, "python") == 0;
    }
    if (strcmp(token, "lua") == 0) {
        return strcmp(reference->page_id, "lua") == 0;
    }
    return false;
}

static unsigned manual_reference_score(
    const solar_os_manual_reference_t *reference,
    const char *query)
{
    const char *text = NULL;
    size_t text_len = 0U;
    if (reference == NULL || query == NULL || query[0] == '\0' ||
        solar_os_manual_reference_text(reference, &text, &text_len) != ESP_OK) {
        return 0U;
    }

    unsigned score = 0U;
    unsigned task_tokens = 0U;
    unsigned task_matches = 0U;
    unsigned structural_matches = 0U;
    bool language_qualified = false;
    char token[MANUAL_TOKEN_MAX + 1U];
    size_t token_len = 0U;
    for (const unsigned char *cursor = (const unsigned char *)query;; cursor++) {
        const bool separator = *cursor == '\0' || !isalnum(*cursor);
        if (!separator && token_len < MANUAL_TOKEN_MAX) {
            token[token_len++] = (char)tolower(*cursor);
        }
        if (separator && token_len > 0U) {
            token[token_len] = '\0';
            if (!manual_stop_word(token)) {
                if (manual_reference_qualifier(token)) {
                    if (manual_reference_language_matches(reference, token)) {
                        score += 1000U;
                        language_qualified = true;
                    }
                } else {
                    task_tokens++;
                    bool matched = false;
                    if (manual_contains_ci(reference->topic, token)) {
                        score += 600U;
                        matched = true;
                        structural_matches++;
                    }
                    if (manual_contains_ci(reference->section, token)) {
                        score += 400U;
                        matched = true;
                        structural_matches++;
                    }
                    if (manual_contains_ci_n(text, text_len, token)) {
                        score += 30U;
                        matched = true;
                    }
                    if (matched) {
                        task_matches++;
                    }
                }
            }
            token_len = 0U;
        }
        if (*cursor == '\0') {
            break;
        }
    }
    if (task_tokens > 0U && task_matches == 0U) {
        return 0U;
    }
    if (task_tokens > 0U && structural_matches == 0U &&
        task_matches * 2U < task_tokens) {
        return 0U;
    }
    if (task_tokens == 0U && !language_qualified) {
        return 0U;
    }
    if (reference->part == 1U && structural_matches > 0U) {
        score += 100U;
    }
    return score;
}

size_t solar_os_manual_reference_search(
    const char *query,
    const solar_os_manual_reference_t **results,
    size_t capacity)
{
    if (query == NULL || query[0] == '\0' || results == NULL || capacity == 0U) {
        return 0U;
    }
    if (capacity > MANUAL_SEARCH_MAX) {
        capacity = MANUAL_SEARCH_MAX;
    }

    unsigned scores[MANUAL_SEARCH_MAX] = {0};
    size_t count = 0U;
    for (size_t candidate = 0U;
         candidate < solar_os_manual_reference_count();
         candidate++) {
        const solar_os_manual_reference_t *reference =
            solar_os_manual_reference_get(candidate);
        const unsigned score = manual_reference_score(reference, query);
        if (score == 0U) {
            continue;
        }
        size_t insert = 0U;
        while (insert < count &&
               (scores[insert] > score ||
                (scores[insert] == score &&
                 (strcmp(results[insert]->topic, reference->topic) < 0 ||
                  (strcmp(results[insert]->topic, reference->topic) == 0 &&
                   results[insert]->part < reference->part))))) {
            insert++;
        }
        if (insert >= capacity) {
            continue;
        }
        if (count < capacity) {
            count++;
        }
        for (size_t move = count - 1U; move > insert; move--) {
            results[move] = results[move - 1U];
            scores[move] = scores[move - 1U];
        }
        results[insert] = reference;
        scores[insert] = score;
    }
    return count;
}
