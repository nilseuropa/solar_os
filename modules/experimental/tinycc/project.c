/* A deliberately bounded, declarative on-device build description. */
#include <stdio.h>
#include <string.h>
#include "project.h"
#include "sdk_reader.h"

typedef struct {
    const char *text;
    size_t at, bytes;
    unsigned line;
    const char *error;
} parser_t;

static bool fail(parser_t *p, const char *message)
{ if (!p->error) p->error = message; return false; }
static char peek(const parser_t *p)
{ return p->at < p->bytes ? p->text[p->at] : 0; }
static void horizontal(parser_t *p)
{ while (peek(p) == ' ' || peek(p) == '\t') p->at++; }
static bool newline(parser_t *p)
{
    if (peek(p) == '\r' && p->at + 1 < p->bytes && p->text[p->at + 1] == '\n') p->at++;
    if (peek(p) != '\n') return false;
    p->at++; p->line++; return true;
}
static void comment(parser_t *p)
{ if (peek(p) == '#') while (p->at < p->bytes && peek(p) != '\r' && peek(p) != '\n') p->at++; }
static void trivia(parser_t *p)
{ do { horizontal(p); comment(p); } while (newline(p)); }
static bool take(parser_t *p, char c)
{ if (peek(p) != c) return false; p->at++; return true; }
static bool key(parser_t *p, char *out, size_t capacity)
{
    size_t n = 0;
    while (p->at < p->bytes) {
        unsigned char c = peek(p);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) break;
        if (n + 1 >= capacity) return fail(p, "key is too long");
        out[n++] = c; p->at++;
    }
    out[n] = 0;
    return n ? true : fail(p, "expected a bare key");
}
static bool string(parser_t *p, char *out, size_t capacity)
{
    char quote = peek(p); size_t n = 0;
    if (quote != '\'' && quote != '"') return fail(p, "expected a quoted string");
    p->at++;
    while (p->at < p->bytes) {
        unsigned char c = p->text[p->at++];
        if (c == (unsigned char)quote) {
            out[n] = 0; return n ? true : fail(p, "strings must not be empty");
        }
        if (quote == '"' && c == '\\') {
            if (p->at == p->bytes) return fail(p, "unfinished string escape");
            c = p->text[p->at++];
            if (c != '"' && c != '\\') return fail(p, "only escaped quotes and backslashes are supported");
        }
        if (c < 32 || c == 127) return fail(p, "control characters and multiline strings are not supported");
        if (n + 1 >= capacity) return fail(p, "string exceeds the supported size");
        out[n++] = c;
    }
    return fail(p, "unterminated string");
}
static bool string_array(parser_t *p, char (*out)[SOLAR_OS_NATIVE_STORAGE_PATH_MAX], unsigned *count)
{
    if (!take(p, '[')) return fail(p, "expected an array of strings");
    trivia(p);
    if (take(p, ']')) return true;
    for (;;) {
        if (*count == 8) return fail(p, "maximum eight paths per array");
        if (!string(p, out[*count], sizeof(out[0]))) return false;
        (*count)++; trivia(p);
        if (take(p, ']')) return true;
        if (!take(p, ',')) return fail(p, "expected ',' or ']' after a path");
        trivia(p);
        if (take(p, ']')) return true;
    }
}
static bool name_valid(const char *name, bool version)
{
    if (!*name) return false;
    for (const unsigned char *s = (const unsigned char *)name; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-' ||
              (version && ((*s >= 'A' && *s <= 'Z') || *s == '.')))) return false;
    return true;
}
static bool dependency(parser_t *p, solar_os_cc_project_dependency_t *dep)
{
    unsigned seen = 0;
    if (!take(p, '{')) return fail(p, "expected a dependency table with id and version");
    for (;;) {
        char field[16]; horizontal(p);
        if (!key(p, field, sizeof(field))) return false;
        horizontal(p);
        if (!take(p, '=')) return fail(p, "expected '=' after dependency key");
        horizontal(p);
        unsigned bit = !strcmp(field, "id") ? 1 : !strcmp(field, "version") ? 2 : 0;
        if (!bit) return fail(p, "unknown dependency key (expected id or version)");
        if (seen & bit) return fail(p, "duplicate dependency key");
        seen |= bit;
        if (!string(p, bit == 1 ? dep->id : dep->version, bit == 1 ? sizeof(dep->id) : sizeof(dep->version))) return false;
        horizontal(p);
        if (take(p, '}')) break;
        if (!take(p, ',')) return fail(p, "expected ',' or '}' in dependency");
    }
    if (seen != 3) return fail(p, "dependency requires both id and version");
    if (!name_valid(dep->id, false) || !name_valid(dep->version, true)) return fail(p, "invalid dependency id or exact version");
    return true;
}
static bool dependencies(parser_t *p, solar_os_cc_project_t *project)
{
    if (!take(p, '[')) return fail(p, "expected an array of dependency tables");
    trivia(p);
    if (take(p, ']')) return true;
    for (;;) {
        if (project->dependency_count == SOLAR_OS_CC_PROJECT_DEPENDENCIES_MAX) return fail(p, "maximum eight dependencies");
        solar_os_cc_project_dependency_t *dep = &project->dependencies[project->dependency_count];
        if (!dependency(p, dep)) return false;
        for (unsigned i = 0; i < project->dependency_count; ++i)
            if (!strcmp(dep->id, project->dependencies[i].id)) return fail(p, "duplicate dependency id");
        project->dependency_count++; trivia(p);
        if (take(p, ']')) return true;
        if (!take(p, ',')) return fail(p, "expected ',' or ']' after dependency");
        trivia(p);
        if (take(p, ']')) return true;
    }
}
int solar_os_cc_project_parse(const char *text, size_t bytes, solar_os_cc_project_t *project,
    char *detail, size_t capacity)
{
    if (!detail || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    detail[0] = 0;
    if (!text || !project) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(project, 0, sizeof(*project)); memcpy(project->output, "a.elf", 6);
    if (!bytes || bytes > SOLAR_OS_CC_PROJECT_BYTES_MAX || memchr(text, 0, bytes)) {
        snprintf(detail, capacity, "project must contain 1..8192 bytes without NUL characters");
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    parser_t parser = {.text = text, .bytes = bytes, .line = 1}; parser_t *p = &parser;
    unsigned seen = 0; trivia(p);
    while (p->at < p->bytes) {
        char field[24];
        if (!key(p, field, sizeof(field))) break;
        horizontal(p);
        if (!take(p, '=')) { fail(p, "expected '=' after project key"); break; }
        horizontal(p);
        unsigned bit = !strcmp(field, "sources") ? 1 : !strcmp(field, "include_dirs") ? 2 :
            !strcmp(field, "output") ? 4 : !strcmp(field, "dependencies") ? 8 : 0;
        if (!bit) { fail(p, "unknown project key (expected sources, include_dirs, output or dependencies)"); break; }
        if (seen & bit) { fail(p, "duplicate project key"); break; }
        seen |= bit;
        bool ok = bit == 1 ? string_array(p, project->sources, &project->source_count) :
            bit == 2 ? string_array(p, project->include_dirs, &project->include_count) :
            bit == 4 ? string(p, project->output, sizeof(project->output)) : dependencies(p, project);
        if (!ok) break;
        horizontal(p); comment(p);
        if (p->at < p->bytes && !newline(p)) { fail(p, "expected a newline after project value"); break; }
        trivia(p);
    }
    if (!p->error && !project->source_count) fail(p, "sources must list at least one C file");
    if (p->error) {
        snprintf(detail, capacity, "line %u: %s", p->line, p->error);
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    return 0;
}

bool solar_os_cc_project_same_path(const char *a, const char *b)
{
    /* The preferred volume may use case-insensitive FAT. */
    while (*a && *b) {
        unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return *a == *b;
}
typedef struct {
    char manifest[SOLAR_OS_NATIVE_STORAGE_PATH_MAX], root[SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    char joined[2 * SOLAR_OS_NATIVE_STORAGE_PATH_MAX], resolved[SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    char text[SOLAR_OS_CC_PROJECT_BYTES_MAX];
    solar_os_cc_sdk_index_t index;
} project_work_t;
static int resolve(const solar_os_native_storage_api_v1_t *s, project_work_t *w, char *path)
{
    int n = snprintf(w->joined, sizeof(w->joined), "%s%s%s", path[0] == '/' ? "" : w->root,
        path[0] == '/' ? "" : "/", path);
    if (n < 0 || (size_t)n >= sizeof(w->joined)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    int code = s->resolve_path(s->context, w->joined, w->resolved, sizeof(w->resolved));
    if (!code) memcpy(path, w->resolved, strlen(w->resolved) + 1);
    return code;
}
static int prepare(const solar_os_native_storage_api_v1_t *s, const solar_os_native_time_api_v1_t *clock,
    const char *manifest, solar_os_cc_project_t *p, project_work_t *w, char *detail, size_t capacity)
{
    int code = s->resolve_path(s->context, manifest, w->manifest, sizeof(w->manifest));
    uint32_t bytes = 0;
    if (!code) code = s->file_size(s->context, w->manifest, &bytes);
    if (code) { snprintf(detail, capacity, "cannot read project %s (%d)", manifest, code); return code; }
    if (!bytes || bytes > sizeof(w->text)) {
        snprintf(detail, capacity, "project must contain 1..8192 bytes"); return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    size_t actual = 0;
    code = s->read_file(s->context, w->manifest, 0, w->text, bytes, &actual);
    if (!code && actual != bytes) code = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (code) return code;
    code = solar_os_cc_project_parse(w->text, bytes, p, detail, capacity);
    if (code) return code;
    memcpy(w->root, w->manifest, strlen(w->manifest) + 1);
    size_t n = strlen(w->root);
    while (n && w->root[n - 1] != '/') n--;
    if (!n) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    w->root[n == 1 ? 1 : n - 1] = 0;
    code = resolve(s, w, p->output);
    if (code) return code;
    if (solar_os_cc_project_same_path(p->output, w->manifest)) {
        snprintf(detail, capacity, "output must differ from the project file"); return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    for (unsigned i = 0; i < p->source_count; ++i) {
        code = resolve(s, w, p->sources[i]); if (code) return code;
        if (solar_os_cc_project_same_path(p->sources[i], p->output)) {
            snprintf(detail, capacity, "output must differ from every source file"); return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        }
        for (unsigned j = 0; j < i; ++j) if (solar_os_cc_project_same_path(p->sources[i], p->sources[j])) {
            snprintf(detail, capacity, "duplicate source path: %s", p->sources[i]); return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        }
    }
    for (unsigned i = 0; i < p->include_count; ++i) {
        code = resolve(s, w, p->include_dirs[i]); if (code) return code;
        for (unsigned j = 0; j < i; ++j) if (solar_os_cc_project_same_path(p->include_dirs[i], p->include_dirs[j])) {
            snprintf(detail, capacity, "duplicate include path: %s", p->include_dirs[i]); return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        }
    }
    for (unsigned i = 0; i < p->dependency_count; ++i) {
        if (clock) clock->sleep_ms(1);
        if (s->stop_requested(s->context)) return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
        solar_os_cc_project_dependency_t *dep = &p->dependencies[i];
        snprintf(w->joined, sizeof(w->joined), "/modules/libraries/%s.elf", dep->id);
        code = s->resolve_path(s->context, w->joined, w->resolved, sizeof(w->resolved));
        uint32_t file_bytes = 0;
        if (!code) code = s->file_size(s->context, w->resolved, &file_bytes);
        if (code == SOLAR_OS_NATIVE_ERROR_NOT_FOUND) {
            snprintf(detail, capacity, "missing library %s (need SDK %s)\ninstall: pkg install %s", dep->id, dep->version, dep->id);
            return code;
        }
        if (code) return code;
        code = solar_os_cc_sdk_library_index(s, w->resolved, dep->id, &w->index);
        if (code) {
            if (code == SOLAR_OS_NATIVE_ERROR_TIMEOUT) return code;
            snprintf(detail, capacity, "library %s has no valid SDK metadata (%d)", dep->id, code); return code;
        }
        if (strcmp(w->index.version, dep->version)) {
            snprintf(detail, capacity, "%s requires SDK %s; installed %s\nuse pkg install %s when the repository provides %s",
                dep->id, dep->version, w->index.version, dep->id, dep->version);
            return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
        }
    }
    return s->stop_requested(s->context) ? SOLAR_OS_NATIVE_ERROR_TIMEOUT : 0;
}
int solar_os_cc_project_load(const solar_os_native_storage_api_v1_t *storage,
    const solar_os_native_memory_api_v1_t *memory, const solar_os_native_time_api_v1_t *clock,
    const char *manifest, solar_os_cc_project_t *project, char *detail, size_t capacity)
{
    if (!detail || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    detail[0] = 0;
    if (!storage || !memory || !manifest || !project) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    project_work_t *work = memory->alloc(sizeof(*work));
    if (!work) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    int code = prepare(storage, clock, manifest, project, work, detail, capacity);
    memory->free(work);
    if (code && !*detail) snprintf(detail, capacity, code == SOLAR_OS_NATIVE_ERROR_TIMEOUT ?
        "project cancelled or timed out (%d)" : "cannot prepare project (%d)", code);
    return code;
}
