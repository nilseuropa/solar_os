#include <assert.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define SOLAR_OS_BOARD_ID "board"
#define SOLAR_OS_OTA_BOARD_MAX 64
#define SOLAR_OS_OTA_FLAVOR_MAX 32
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_RESPONSE, ESP_ERR_NOT_FOUND };
typedef int esp_err_t;
typedef struct { char board_id[64], target_flavor[32]; } solar_os_ota_check_result_t;

/* A minimal JSON adapter. Selection runs the production implementation. */
typedef struct value {
    enum { STRING, OBJECT, ARRAY } kind;
    const char *text;
    size_t count;
    const char *keys[10];
    struct value *values[10];
} solar_os_json_value_t;
static size_t strlcpy(char *out, const char *text, size_t size)
{
    size_t len = strlen(text);
    if (size) { size_t copy = len < size - 1 ? len : size - 1; memcpy(out, text, copy); out[copy] = 0; }
    return len;
}
static const solar_os_json_value_t *solar_os_json_object_get(const solar_os_json_value_t *v, const char *key)
{
    if (!v || v->kind != OBJECT) return NULL;
    for (size_t i = 0; i < v->count; ++i) if (!strcmp(v->keys[i], key)) return v->values[i];
    return NULL;
}
#define solar_os_json_path_get solar_os_json_object_get
static bool solar_os_json_is_array(const solar_os_json_value_t *v) { return v && v->kind == ARRAY; }
static bool solar_os_json_is_object(const solar_os_json_value_t *v) { return v && v->kind == OBJECT; }
static size_t solar_os_json_array_size(const solar_os_json_value_t *v) { return v->count; }
static const solar_os_json_value_t *solar_os_json_array_get(const solar_os_json_value_t *v, size_t i) { return v->values[i]; }
static esp_err_t solar_os_json_get_string(const solar_os_json_value_t *v, char *out, size_t size)
{
    if (!v || v->kind != STRING || strlen(v->text) >= size) return ESP_ERR_INVALID_RESPONSE;
    strlcpy(out, v->text, size);
    return ESP_OK;
}
static esp_err_t solar_os_json_get_path_string(const solar_os_json_value_t *v, const char *key, char *out, size_t size)
{ return solar_os_json_get_string(solar_os_json_object_get(v, key), out, size); }
static const solar_os_json_value_t *filled;
static int fill_error;
static esp_err_t ota_fill_artifact_from_json(const solar_os_json_value_t *v, const char *base,
                                           solar_os_ota_check_result_t *result)
{ (void)base; (void)result; filled = v; return fill_error; }
#include "ota_selection.inc"

static solar_os_json_value_t string(const char *text) { return (solar_os_json_value_t){.kind=STRING, .text=text}; }
static void put(solar_os_json_value_t *v, const char *key, solar_os_json_value_t *value)
{ assert(v->count < 10); v->keys[v->count] = key; v->values[v->count++] = value; }
static solar_os_ota_check_result_t result(const char *flavor)
{ solar_os_ota_check_result_t r = {0}; strlcpy(r.target_flavor, flavor, sizeof(r.target_flavor)); return r; }

int main(void)
{
    solar_os_json_value_t board = string("board"), other = string("other");
    solar_os_json_value_t compact = string("compact"), writerdeck = string("writerdeck"), full = string("full");
    solar_os_json_value_t artifact = {.kind=OBJECT}, exact = {.kind=OBJECT}, wrong = {.kind=OBJECT};
    put(&artifact, "board_id", &board); put(&artifact, "flavor", &compact);
    put(&exact, "board_id", &board); put(&exact, "flavor", &writerdeck);
    put(&wrong, "board_id", &other); put(&wrong, "flavor", &writerdeck);
    solar_os_json_value_t artifacts = {.kind=ARRAY, .count=2, .values={&wrong, &artifact}};
    solar_os_json_value_t aliases = {.kind=OBJECT}, root = {.kind=OBJECT};
    put(&root, "artifacts", &artifacts); put(&root, "flavor_aliases", &aliases);
    put(&aliases, "writerdeck", &compact);
    solar_os_ota_check_result_t r = result("writerdeck");
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_OK);
    assert(filled == &artifact && !strcmp(r.target_flavor, "compact") && !strcmp(r.board_id, "board"));
    r = result("full");
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_ERR_NOT_FOUND);
    assert(!strcmp(r.target_flavor, "full"));

    artifacts.values[2] = &exact; artifacts.count = 3;
    r = result("writerdeck");
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_OK);
    assert(filled == &exact && !strcmp(r.target_flavor, "writerdeck"));
    fill_error = ESP_ERR_INVALID_RESPONSE;
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_ERR_INVALID_RESPONSE);
    assert(filled == &exact); /* Invalid exact artifacts must not bypass validation via an alias. */
    fill_error = ESP_OK; artifacts.count = 2;

    aliases.values[0] = &writerdeck; r = result("writerdeck");
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_ERR_INVALID_RESPONSE);
    aliases.values[0] = &full; put(&aliases, "full", &writerdeck);
    r = result("writerdeck");
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_ERR_INVALID_RESPONSE);
    assert(!strcmp(r.target_flavor, "writerdeck"));

    solar_os_json_value_t bad = string("../compact"), long_name = string("abcdefghijklmnopqrstuvwxyz0123456789");
    aliases.count = 1;
    solar_os_json_value_t *invalid[] = {&bad, &long_name, &artifact};
    for (size_t i = 0; i < 3; ++i) {
        aliases.values[0] = invalid[i]; r = result("writerdeck");
        assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_ERR_INVALID_RESPONSE);
        assert(!strcmp(r.target_flavor, "writerdeck"));
    }
    aliases.values[0] = &compact; artifacts.count = 1; r = result("writerdeck");
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_ERR_NOT_FOUND);
    assert(!strcmp(r.target_flavor, "writerdeck")); /* Other boards never satisfy a redirect. */
    artifacts.count = 2;

    char names[9][12]; solar_os_json_value_t targets[9];
    aliases.count = 0;
    for (size_t i = 0; i < 9; ++i) { snprintf(names[i], sizeof(names[i]), "flavor%zu", i); targets[i] = string(names[i]); }
    for (size_t i = 0; i < 7; ++i) put(&aliases, names[i], &targets[i+1]);
    artifact.values[1] = &targets[7]; r = result(names[0]);
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_OK);
    assert(!strcmp(r.target_flavor, names[7]));
    put(&aliases, names[7], &targets[8]); artifact.values[1] = &targets[8]; r = result(names[0]);
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_ERR_INVALID_RESPONSE);
    aliases.kind = ARRAY; r = result("writerdeck");
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_ERR_INVALID_RESPONSE);
    root.count = 1;
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_ERR_NOT_FOUND);
    artifacts.kind = OBJECT;
    assert(ota_find_artifact_in_index(&root, "https://test", &r) == ESP_ERR_INVALID_RESPONSE);
    assert(ota_find_artifact_in_index(NULL, "https://test", &r) == ESP_ERR_INVALID_ARG);
    puts("OTA flavor aliases passed");
    return 0;
}
