#include "manager.h"
#include "solar_os_native_sscma_abi.h"
#include "solar_os_native_data_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_native_time_abi.h"
#include "solar_os_json_scan.h"
#include <stdarg.h>
#include <string.h>

#define BAD SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT
#define FAILED SOLAR_OS_NATIVE_ERROR_FAILED
#define UNSUPPORTED SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED
#define CANCELLED SOLAR_OS_NATIVE_ERROR_TIMEOUT
#define SLOT_BYTES 0x200000U
#define PATH_MAXIMUM 160U
typedef const solar_os_native_json_value_t value_t;
typedef struct {
    const solar_os_native_memory_api_v1_t *memory;
    const solar_os_native_sscma_api_v1_t *camera;
    const solar_os_native_files_api_v1_t *files;
    const solar_os_native_json_api_v1_t *json;
    const solar_os_native_libc_api_v1_t *libc;
    const solar_os_native_time_api_v1_t *time;
    solar_os_native_cancel_fn cancel;
    solar_os_processor_progress_fn progress;
    void *user;
    uint32_t token, timeout, previous, slot, bytes, written;
    char source[32], firmware[11], device[129], id[101], version[101];
    char folder[PATH_MAXIMUM], record[PATH_MAXIMUM], sha[65];
    char result[16], packet[264];
    solar_os_native_file_t *model;
    solar_os_native_json_value_t *manifest;
    bool maintenance, recorded;
} manager_t;
static bool stopped(manager_t *c) { return c->cancel && c->cancel(c->user); }
static int format(manager_t *c, char *out, size_t size, const char *fmt, ...)
{
    va_list args; va_start(args, fmt); int n = c->libc->format(out, size, fmt, args); va_end(args);
    return n < 0 || (size_t)n >= size ? SOLAR_OS_NATIVE_ERROR_INVALID_SIZE : 0;
}
static void progress(manager_t *c, const char *stage, uint32_t n, uint32_t total)
{ if (c->progress) c->progress(stage, n, total, c->user); }
static int bind(manager_t *c, solar_os_processor_service_fn services, const char *source, uint32_t timeout)
{
    if (!services || !source || !*source || strlen(source) >= sizeof(c->source) || !timeout || timeout > 60000) return BAD;
    for (const char *s = source; *s; ++s)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) return BAD;
    c->memory = services("memory", 1, sizeof(*c->memory));
    c->camera = services("camera.sscma", 1, SOLAR_OS_NATIVE_SSCMA_BASE_SIZE);
    c->libc = services("libc", 1, sizeof(*c->libc));
    c->time = services("time", 1, sizeof(*c->time));
    if (!c->memory || !c->camera || !c->libc || !c->time) return UNSUPPORTED;
    c->timeout = timeout; memcpy(c->source, source, strlen(source) + 1); return 0;
}
static int exchange(manager_t *c, const char *command, const char *name, bool event, char **reply)
{ return c->camera->exchange(c->token, c->source, command, name, event, c->timeout, c->cancel, c->user, reply); }
static int active(manager_t *c, uint32_t *slot)
{
    char *reply = NULL; const char *data; size_t n; uint64_t id;
    int error = exchange(c, "AT+MODEL?\r\n", "MODEL?", false, &reply);
    if (!error && (!solar_os_json_scan_object_value(reply, "data", &data, &n) ||
        !solar_os_json_scan_object_uint64(data, "id", &id) || id < 1 || id > 4)) error = FAILED;
    if (!error) *slot = id;
    c->memory->free(reply); return error;
}
static int select_slot(manager_t *c, uint32_t slot)
{
    if (slot < 1 || slot > 4) return BAD;
    char command[32], *reply = NULL; uint32_t selected = 0;
    int error = format(c, command, sizeof(command), "AT+MODEL=%lu\r\n", (unsigned long)slot);
    if (!error) error = exchange(c, command, "MODEL", false, &reply);
    c->memory->free(reply);
    if (!error) error = active(c, &selected);
    return error ? error : selected == slot ? 0 : FAILED;
}
static void close_owner(manager_t *c)
{
    if (c->token) {
        while (c->camera->close(c->token)) c->time->sleep_ms(20);
        c->token = 0;
    }
}
int solar_os_himax_model_select(solar_os_processor_service_fn services, const char *source,
    uint32_t slot, uint32_t timeout, solar_os_native_cancel_fn cancel, void *user, char **out)
{
    if (!out) return BAD;
    *out = NULL; manager_t c = {.cancel = cancel, .user = user};
    int error = bind(&c, services, source, timeout);
    if (!error && (slot < 1 || slot > 4)) error = BAD;
    if (!error) { *out = c.memory->alloc(128); if (!*out) error = SOLAR_OS_NATIVE_ERROR_NO_MEMORY; }
    if (!error && stopped(&c)) error = CANCELLED;
    if (!error) error = c.camera->open(source, "himax-select", &c.token);
    if (!error) error = active(&c, &c.previous);
    if (!error && c.previous != slot) error = select_slot(&c, slot);
    if (error && c.token && c.previous) {
        c.cancel = NULL;
        if (c.camera->struct_size >= sizeof(*c.camera) && c.camera->reset) (void)c.camera->reset(c.token);
        (void)select_slot(&c, c.previous);
    }
    close_owner(&c);
    if (!error) {
        error = format(&c, *out, 128, "{\"kind\":\"himax\",\"slot\":%lu,\"previous_slot\":%lu}",
            (unsigned long)slot, (unsigned long)c.previous);
    }
    if (error && *out) { c.memory->free(*out); *out = NULL; }
    return error;
}
static value_t *get(manager_t *c, value_t *v, const char *key) { return c->json->get(v, key); }
static const char *string(manager_t *c, value_t *v) { return c->json->string(v); }
static bool eq(manager_t *c, value_t *v, const char *s)
{ const char *p = string(c, v); return p && !strcmp(p, s); }
static bool uint_range(manager_t *c, value_t *v, uint64_t min, uint64_t max, uint32_t *out)
{
    uint64_t n; if (!c->json->uint_value(v, &n) || n < min || n > max) return false;
    if (out) *out = n;
    return true;
}
static bool identity(const char *s)
{
    if (!s || !*s || strlen(s) > 100 || s[strlen(s)-1] == '.') return false;
    for (const char *p = s; *p; ++p) {
        bool alnum = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9');
        if (!alnum && (p == s || (*p != '-' && *p != '_' && *p != '.'))) return false;
    }
    return true;
}
static bool relative(const char *s)
{
    if (!s || !*s || *s == '/' || strlen(s) >= PATH_MAXIMUM) return false;
    const char *part = s;
    for (;; ++s) {
        if (*s && ((unsigned char)*s < 32 || (unsigned char)*s == 127 || strchr("\\:<>\"|?*", *s))) return false;
        if (!*s || *s == '/') {
            size_t n = s - part;
            if (!n || (n == 1 && *part == '.') || (n == 2 && !memcmp(part, "..", 2)) || s[-1] == '.' || s[-1] == ' ') return false;
            if (!*s) return true;
            part = s + 1;
        }
    }
}
static bool hash(const char *p)
{
    if (!p || strlen(p) != 64) return false;
    for (; *p; ++p) if (!strchr("0123456789abcdef", *p)) return false;
    return true;
}
static int path(manager_t *c, const char *name, char out[PATH_MAXIMUM])
{ return relative(name) ? format(c, out, PATH_MAXIMUM, "%s/%s", c->folder, name) : BAD; }
static bool keys(manager_t *c, value_t *object, const char *allowed)
{
    if (!object || c->json->kind(object) != SOLAR_OS_NATIVE_JSON_OBJECT) return false;
    for (size_t i = 0; i < c->json->count(object); ++i) {
        const char *key = c->json->key(c->json->at(object, i));
        if (!key) return false;
        bool found = false; const char *p = allowed;
        while (*p) {
            const char *end = strchr(p, ','); size_t n = end ? (size_t)(end - p) : strlen(p);
            if (strlen(key) == n && !memcmp(p, key, n)) found = true;
            p += n; if (*p) ++p;
        }
        if (!found) return false;
    }
    return true;
}
static bool date(const char *s)
{
    if (!s || strlen(s) != 10 || s[4] != '.' || s[7] != '.') return false;
    for (size_t i = 0; i < 10; ++i) if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return false;
    unsigned y = (s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';
    unsigned m = (s[5]-'0')*10+s[6]-'0', d = (s[8]-'0')*10+s[9]-'0';
    static const unsigned days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    return y && m >= 1 && m <= 12 && d >= 1 && d <= days[m] + (m == 2 && y%4 == 0 && (y%100 != 0 || y%400 == 0));
}
static int read_json(manager_t *c, const char *name, solar_os_native_json_value_t **out)
{
    solar_os_native_file_t *file = NULL; uint32_t size = 0; size_t read = 0;
    int error = c->files->open(name, &file, &size);
    if (!error && (!size || size > 65536)) error = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    char *data = !error ? c->memory->alloc(size + 1) : NULL;
    if (!error && !data) error = SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    if (!error) error = c->files->read(file, data, size, &read);
    if (!error && read != size) error = FAILED;
    if (!error) { data[size] = 0; error = c->json->parse(data, size, out); }
    c->memory->free(data); c->files->close(file); return error;
}
static int check_file(manager_t *c, const char *name, const char *expected, solar_os_native_file_t **keep, uint32_t *bytes)
{
    char full[PATH_MAXIMUM], sha[65]; solar_os_native_file_t *file = NULL; uint32_t size = 0;
    int error = hash(expected) ? path(c, name, full) : BAD;
    if (!error) error = c->files->open(full, &file, &size);
    if (!error) error = c->files->sha256(file, sha, c->cancel, c->user);
    if (!error && strcmp(sha, expected)) error = FAILED;
    if (!error && bytes) *bytes = size;
    if (!error && keep) { *keep = file; file = NULL; }
    c->files->close(file); return error;
}
static int labels(manager_t *c, const char *name, uint32_t classes)
{
    char full[PATH_MAXIMUM]; solar_os_native_file_t *file = NULL; uint32_t size = 0;
    int error = path(c, name, full);
    if (!error) error = c->files->open(full, &file, &size);
    if (!error && (!size || size > 256*1024)) error = BAD;
    uint32_t lines = 0, line_bytes = 0, cp = 0, minimum = 0;
    unsigned remaining = 0; bool nonspace = false, cr = false;
    while (!error) {
        if (stopped(c)) { error = CANCELLED; break; }
        size_t n = 0; error = c->files->read(file, c->packet, 256, &n);
        for (size_t i = 0; !error && i < n; ++i) {
            unsigned ch = (unsigned char)c->packet[i];
            if (cr && ch != 10) { error = BAD; break; }
            cr = false;
            if (ch == 13 && !remaining) { cr = true; continue; }
            if (ch == 10 && !remaining) {
                if (!nonspace || ++lines > classes) { error = BAD; break; }
                line_bytes = 0; nonspace = false; continue;
            }
            if (ch < 32 || ch == 127 || ++line_bytes > 255) { error = BAD; break; }
            if (ch != ' ') nonspace = true;
            if (remaining) {
                if ((ch & 0xc0) != 0x80) { error = BAD; break; }
                cp = (cp << 6) | (ch & 63);
                if (!--remaining && (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))) error = BAD;
            } else if (ch >= 0x80) {
                if (ch >= 0xc2 && ch <= 0xdf) { remaining = 1; minimum = 0x80; cp = ch & 31; }
                else if (ch >= 0xe0 && ch <= 0xef) { remaining = 2; minimum = 0x800; cp = ch & 15; }
                else if (ch >= 0xf0 && ch <= 0xf4) { remaining = 3; minimum = 0x10000; cp = ch & 7; }
                else error = BAD;
            }
        }
        if (n < 256) break;
    }
    if (line_bytes) { if (!nonspace) error = BAD; else ++lines; }
    if (remaining || cr || lines != classes) error = BAD;
    c->files->close(file); return error;
}
static int read_at(manager_t *c, uint32_t offset, void *out, size_t size)
{
    if ((uint64_t)offset + size > c->bytes) return BAD;
    size_t n = 0; int error = c->files->seek(c->model, offset);
    if (!error) error = c->files->read(c->model, out, size, &n);
    return error ? error : n == size ? 0 : FAILED;
}
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static int word(manager_t *c, uint32_t offset, uint32_t *out)
{ uint8_t p[4]; int e = read_at(c, offset, p, 4); if (!e) *out = le32(p); return e; }
static int indirect(manager_t *c, uint32_t pos, uint32_t *out)
{
    uint32_t delta; int error = word(c, pos, &delta);
    if (!error && (delta < 4 || (uint64_t)pos + delta + 4 > c->bytes)) error = BAD;
    if (!error) *out = pos + delta;
    return error;
}
static int field(manager_t *c, uint32_t table, unsigned index, uint32_t *out)
{
    uint32_t back, header; int error = word(c, table, &back);
    if (error) return error;
    int64_t vt = (int64_t)table - (int32_t)back;
    if (error || vt < 0 || vt > c->bytes - 4) return BAD;
    error = word(c, vt, &header); if (error) return error;
    uint32_t len = header & 0xffff, object = header >> 16;
    if (len < 4 || len % 2 || (uint64_t)vt + len > c->bytes || object < 4 || (uint64_t)table + object > c->bytes) return BAD;
    *out = 0; if (4 + index*2 >= len) return 0;
    uint8_t bytes[2]; error = read_at(c, vt + 4 + index*2, bytes, 2);
    if (error) return error;
    uint32_t off = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8);
    if (!error && off && (off < 4 || off >= object)) error = BAD;
    if (!error && off) *out = table + off;
    return error;
}
static int vector(manager_t *c, uint32_t table, unsigned index, uint32_t maximum, uint32_t *start, uint32_t *count)
{
    uint32_t f = 0, v = 0; int error = field(c, table, index, &f);
    if (!error && !f) return BAD;
    if (!error) error = indirect(c, f, &v);
    if (!error) error = word(c, v, count);
    if (!error && (*count > maximum || (uint64_t)v + 4 + *count * UINT64_C(4) > c->bytes)) error = BAD;
    if (!error) *start = v + 4;
    return error;
}
/* Public-interface checks, not a verifier for the Ethos-U command stream. */
static int model_interface(manager_t *c, uint32_t width, uint32_t height, uint32_t classes)
{
    uint8_t head[8]; uint32_t model, f, v, count, graph, tensors, tensor_count, tensor, input;
    int error = read_at(c, 0, head, 8);
    if (error || memcmp(head + 4, "TFL3", 4)) return BAD;
    model = le32(head);
    if ((error = field(c, model, 0, &f)) || !f || (error = word(c, f, &v)) || v != 3) return BAD;
    if ((error = vector(c, model, 2, 16, &v, &count)) || count != 1 || (error = indirect(c, v, &graph))) return BAD;
    if ((error = vector(c, graph, 0, 4096, &tensors, &tensor_count))) return error;
    if ((error = vector(c, graph, 1, 16, &v, &count)) || count != 1 || (error = word(c, v, &input)) || input >= tensor_count) return BAD;
    if ((error = indirect(c, tensors + 4*input, &tensor)) || (error = field(c, tensor, 1, &f)) || !f) return BAD;
    uint8_t dtype; if (read_at(c, f, &dtype, 1) || (dtype != 9 && dtype != 3)) return BAD;
    if ((error = vector(c, tensor, 0, 8, &v, &count)) || count != 4) return BAD;
    const uint32_t shape[] = {1, height, width, 3};
    for (unsigned i = 0; i < 4; ++i) if (word(c, v + i*4, &input) || input != shape[i]) return BAD;
    if (vector(c, graph, 2, 16, &v, &count) || count != 1 || word(c, v, &input) || input >= tensor_count ||
        indirect(c, tensors + 4*input, &tensor) || field(c, tensor, 1, &f) || !f ||
        read_at(c, f, &dtype, 1) || (dtype != 9 && dtype != 3) || vector(c, tensor, 0, 8, &v, &count)) return BAD;
    uint32_t a, last;
    if (word(c, v, &a) || a != 1) return BAD;
    if (!strcmp(c->result, "classes")) {
        if (count != 2 || word(c, v+4, &last) || last != classes) return BAD;
    } else if (!strcmp(c->result, "boxes")) {
        if (count != 3 || word(c, v+4, &a) || !a || a > 4194304 || word(c, v+8, &last) || last != classes+5) return BAD;
    } else return UNSUPPORTED;
    uint32_t opcodes, code_count, operations, operation_count;
    if (vector(c, model, 1, 512, &opcodes, &code_count) || vector(c, graph, 3, 4096, &operations, &operation_count)) return BAD;
    bool ethos = false;
    for (uint32_t i = 0; i < operation_count; ++i) {
        if (stopped(c)) return CANCELLED;
        uint32_t operation, code = 0, opcode, builtin = 0;
        if (indirect(c, operations + i*4, &operation) || field(c, operation, 0, &f) || (f && word(c, f, &code)) || code >= code_count ||
            indirect(c, opcodes + code*4, &opcode) || field(c, opcode, 3, &f)) return BAD;
        if (f) { if (word(c, f, &builtin)) return BAD; }
        else {
            if (field(c, opcode, 0, &f)) return BAD;
            if (f) { uint8_t b; if (read_at(c, f, &b, 1)) return BAD; builtin = b; }
        }
        if (builtin == 32) {
            uint32_t s, len;
            if (field(c, opcode, 1, &f) || !f || indirect(c, f, &s) || word(c, s, &len)) return BAD;
            if (len == 7) { uint8_t name[8]; if (read_at(c, s + 4, name, 8)) return BAD;
                if (!memcmp(name, "ethos-u", 8)) ethos = true; }
        }
    }
    return ethos ? c->files->seek(c->model, 0) : UNSUPPORTED;
}
static int bundle(manager_t *c, const char *file)
{
    if (!file || *file != '/' || strlen(file) >= PATH_MAXIMUM) return BAD;
    const char *slash = strrchr(file, '/'); if (!slash || slash == file) return BAD;
    size_t base = slash - file; memcpy(c->folder, file, base); c->folder[base] = 0;
    int error = read_json(c, file, &c->manifest); if (error) return error;
    value_t *b = c->manifest, *runtime = get(c, b, "runtime"), *model = get(c, b, "model"), *assets = get(c, b, "assets");
    value_t *provider = get(c, runtime, "provider"), *input = get(c, get(c, b, "inputs"), "camera");
    value_t *result = get(c, b, "result"), *options = get(c, result, "options"), *license = get(c, b, "license");
    const char *id = string(c, get(c, b, "id")), *version = string(c, get(c, b, "version"));
    const char *model_file = string(c, get(c, model, "file")), *sha = string(c, get(c, model, "sha256"));
    const char *label_file = string(c, get(c, options, "labels")), *license_file = string(c, get(c, license, "file"));
    uint32_t width, height, classes;
    if (!keys(c, b, "schema,id,version,runtime,model,inputs,outputs,result,assets,license") ||
        !uint_range(c, get(c, b, "schema"), 2, 2, NULL) || !identity(id) || !identity(version) ||
        !keys(c, runtime, "backend,version,target,provider,firmware") || !eq(c, get(c, runtime, "backend"), "himax") ||
        !eq(c, get(c, runtime, "version"), "sscma-v0") || !eq(c, get(c, runtime, "target"), "hx6538") ||
        !keys(c, provider, "id,abi,capabilities") || !eq(c, get(c, provider, "id"), "processor-himax") ||
        !uint_range(c, get(c, provider, "abi"), 1, 1, NULL) ||
        !keys(c, model, "file,sha256,format") || !eq(c, get(c, model, "format"), "tflite-vela") ||
        !relative(model_file) || strlen(model_file) < 7 || strcmp(model_file + strlen(model_file)-7, ".tflite") || !hash(sha) ||
        !keys(c, get(c, b, "inputs"), "camera") || !keys(c, input, "adapter,width,height,color") ||
        !keys(c, get(c, input, "adapter"), "type") || !eq(c, get(c, get(c, input, "adapter"), "type"), "camera") ||
        !eq(c, get(c, input, "color"), "RGB") || !uint_range(c, get(c, input, "width"), 1, 4096, &width) ||
        !uint_range(c, get(c, input, "height"), 1, 4096, &height) || !keys(c, result, "type,options") ||
        !keys(c, options, "labels,class_count") || !uint_range(c, get(c, options, "class_count"), 1, 1024, &classes) ||
        !relative(label_file) || !relative(license_file) || !keys(c, license, "id,file") || !string(c, get(c, license, "id"))) return BAD;
    value_t *caps = get(c, provider, "capabilities");
    if (c->json->kind(caps) != SOLAR_OS_NATIVE_JSON_ARRAY) return BAD;
    unsigned mask = 0;
    for (size_t i = 0; i < c->json->count(caps); ++i) {
        value_t *v = c->json->at(caps, i);
        unsigned bit = eq(c, v, "capture") ? 1 : eq(c, v, "model-select") ? 2 : eq(c, v, "model-upload") ? 4 : 0;
        if (!bit || (mask & bit)) return BAD;
        mask |= bit;
    }
    if (mask != 7 || c->json->count(caps) != 3) return BAD;
    const char *type = string(c, get(c, result, "type"));
    if (!type) return BAD;
    const char *field = !strcmp(type, "sscma-detection") ? "boxes" : !strcmp(type, "sscma-classification") ? "classes" : !strcmp(type, "sscma-points") ? "points" : NULL;
    value_t *outputs = get(c, b, "outputs"); char expected[32];
    if (!field || c->json->count(outputs) != 1 || !keys(c, outputs, field) || !keys(c, get(c, outputs, field), "format")) return BAD;
    if (format(c, expected, sizeof(expected), "sscma-%s", field) || !eq(c, get(c, get(c, outputs, field), "format"), expected)) return BAD;
    memcpy(c->result, field, strlen(field) + 1); memcpy(c->id, id, strlen(id)+1); memcpy(c->version, version, strlen(version)+1); memcpy(c->sha, sha, 65);
    value_t *bounds = get(c, runtime, "firmware");
    if (bounds) {
        const char *min = string(c, get(c, bounds, "minimum")), *max = string(c, get(c, bounds, "maximum"));
        if (!keys(c, bounds, "minimum,maximum") || !c->json->count(bounds) ||
            (get(c, bounds, "minimum") && !min) || (get(c, bounds, "maximum") && !max) ||
            (min && !date(min)) || (max && !date(max)) || (min && max && strcmp(min, max) > 0)) return BAD;
    }
    if (c->json->kind(assets) != SOLAR_OS_NATIVE_JSON_OBJECT || c->json->count(assets) < 1 || c->json->count(assets) > 32 ||
        !hash(string(c, get(c, assets, label_file))) || !hash(string(c, get(c, assets, license_file)))) return BAD;
    for (size_t i = 0; i < c->json->count(assets); ++i) {
        value_t *asset = c->json->at(assets, i); const char *name = c->json->key(asset);
        if (!relative(name) || !strcmp(name, model_file) || !strcmp(name, "bundle.json") || !strcmp(name, "zoo.json") || !strncmp(name, ".himax-", 7)) return BAD;
        error = check_file(c, name, string(c, asset), NULL, NULL); if (error) return error;
    }
    error = labels(c, label_file, classes);
    if (!error) error = check_file(c, model_file, sha, &c->model, &c->bytes);
    if (!error && (c->bytes < 16 || c->bytes > SLOT_BYTES)) error = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (!error) error = model_interface(c, width, height, classes);
    if (!error) error = format(c, c->record, sizeof(c->record), "%s/.himax-%s-%lu.json", c->folder, c->source, (unsigned long)c->slot);
    return error;
}
static int request_json(manager_t *c, const char *command, const char *name, solar_os_native_json_value_t **out)
{
    char *reply = NULL; int error = exchange(c, command, name, false, &reply);
    if (!error) error = c->json->parse(reply, strlen(reply), out);
    c->memory->free(reply); return error;
}
static int device_layout(manager_t *c)
{
    solar_os_native_json_value_t *root = NULL;
    int error = request_json(c, "AT+VER?\r\n", "VER?", &root);
    value_t *v = get(c, root, "data"); const char *fw = string(c, get(c, v, "software"));
    if (!error && (!date(fw) || strcmp(fw, "2024.06.03") < 0 || !eq(c, get(c, v, "at_api"), "v0"))) error = UNSUPPORTED;
    if (!error) memcpy(c->firmware, fw, 11);
    value_t *bounds = get(c, get(c, c->manifest, "runtime"), "firmware");
    if (!error && bounds) {
        const char *min = string(c, get(c, bounds, "minimum")), *max = string(c, get(c, bounds, "maximum"));
        if ((min && strcmp(fw, min) < 0) || (max && strcmp(fw, max) > 0)) error = UNSUPPORTED;
    }
    c->json->free(root); root = NULL;
    if (!error) error = request_json(c, "AT+ID?\r\n", "ID?", &root);
    const char *id = string(c, get(c, root, "data"));
    if (!error && (!id || !*id || strlen(id) >= sizeof(c->device))) error = UNSUPPORTED;
    if (!error) {
        for (const char *p = id; *p; ++p) if (!( (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= 'a' && *p <= 'z') || strchr("-_.:", *p))) { error = UNSUPPORTED; break; }
        if (!error) memcpy(c->device, id, strlen(id)+1);
    }
    c->json->free(root); root = NULL;
    if (!error) error = request_json(c, "AT+MODELS?\r\n", "MODELS?", &root);
    value_t *models = get(c, root, "data"); unsigned found = 0;
    if (!error && c->json->count(models) != 4) error = UNSUPPORTED;
    for (size_t i = 0; !error && i < 4; ++i) {
        value_t *m = c->json->at(models, i); uint32_t slot, address;
        if (!uint_range(c, get(c, m, "id"), 1, 4, &slot) ||
            !uint_range(c, get(c, m, "address"), 0, UINT32_MAX, &address) ||
            address != 0x400000U + (slot-1)*SLOT_BYTES || (found & (1U << slot))) error = UNSUPPORTED;
        else found |= 1U << slot;
    }
    c->json->free(root);
    if (!error) error = active(c, &c->previous);
    if (!error && c->previous == c->slot) error = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    return error;
}
static int spi(manager_t *c, const void *tx, void *rx, size_t size)
{ return c->camera->spi(c->token, tx, rx, size); }
static int reg(manager_t *c, uint8_t address, uint8_t value)
{ uint8_t tx[] = {0xf2, address, value}; return spi(c, tx, NULL, 3); }
static void put32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = v >> (i*8); }
static int address(manager_t *c, uint32_t addr)
{ uint8_t tx[6] = {0xf2,0}; put32(tx+2,addr); return spi(c,tx,NULL,6); }
static int page_done(manager_t *c, uint32_t count)
{
    uint64_t start = c->time->now_us();
    for (;;) {
        if (stopped(c)) return CANCELLED;
        int error = address(c, 0x51010040);
        if (!error) error = reg(c, 0x0c, 0);
        uint8_t tx[7] = {0xf3,8,0,0,0,0,0}, rx[7] = {0};
        if (!error) error = spi(c, tx, rx, 7);
        if (error) return error;
        uint32_t status = le32(rx+3), state = status >> 28;
        if (state == 1 || state == 3) return FAILED;
        if ((status & 0xfffff) == count) return 0;
        if ((uint64_t)c->time->now_us() - start >= 3000000) return CANCELLED;
        c->time->sleep_ms(1);
    }
}
static int program(manager_t *c)
{
    /* Match the vendor's fresh-reset entry to the ISP writer. A reset may
     * restore a firmware default, so verify/reselect the saved working slot
     * before entering OTA or writing the inactive destination. */
    int error = c->camera->reset(c->token);
    uint32_t selected = 0;
    if (!error) error = active(c, &selected);
    if (!error && selected != c->previous) error = select_slot(c, c->previous);
    char *reply = NULL;
    if (!error) error = exchange(c, "AT+BREAK\r\n", "BREAK", false, &reply);
    c->memory->free(reply);
    if (!error && stopped(c)) error = CANCELLED;
    if (!error) {
        c->maintenance = true; /* Reset even if entering OTA only partly succeeds. */
        error = c->camera->send(c->token, c->source, "AT+OTA\r\n");
    }
    if (!error) c->time->sleep_ms(100);
    if (!error) error = reg(c, 0xd8, 7);
    if (!error) error = reg(c, 0x13, 0x31);
    if (!error) error = reg(c, 0x0d, 0x11);
    uint8_t init[10] = {0xf2,0}; put32(init+2, 0x51010000); put32(init+6, 0x028c208b);
    if (!error) error = spi(c, init, NULL, 10);
    uint32_t offset = 0x400000U + (c->slot-1)*SLOT_BYTES, pages = 0;
    while (!error && c->written < c->bytes) {
        if (stopped(c)) { error = CANCELLED; break; }
        uint8_t *tx = (uint8_t *)c->packet;
        memset(tx, 0xff, 262); tx[0] = 0xf2; tx[1] = 0;
        put32(tx+2, 0x38000000U + offset);
        size_t n = c->bytes-c->written; if (n > 256) n = 256;
        size_t read = 0; error = c->files->read(c->model, tx+6, n, &read);
        if (!error && read != n) error = FAILED;
        if (!error) error = spi(c, tx, NULL, 262);
        if (!error) error = page_done(c, ++pages);
        if (!error) {
            c->written += n; offset += 256; progress(c, "program", c->written, c->bytes);
            if (!(pages % 16)) c->time->sleep_ms(1);
        }
    }
    if (!error) error = reg(c, 0xd8, 0);
    return error;
}
static int smoke_test(manager_t *c)
{
    progress(c, "test inference", c->bytes, c->bytes);
    int error = select_slot(c, c->slot);
    if (!error) error = c->camera->start(c->token);
    char *reply = NULL; const char *data, *result; size_t n;
    if (!error) error = exchange(c, "AT+INVOKE=1,0,0\r\n", "INVOKE", true, &reply);
    if (!error && (!solar_os_json_scan_object_value(reply, "data", &data, &n) ||
        !solar_os_json_scan_object_value(data, c->result, &result, &n) || !n || *result != '[')) error = FAILED;
    solar_os_native_json_value_t *parsed = NULL;
    if (!error) error = c->json->parse(result, n, &parsed);
    if (!error && c->json->kind(parsed) != SOLAR_OS_NATIVE_JSON_ARRAY) error = FAILED;
    c->json->free(parsed);
    c->memory->free(reply); return error;
}
static int receipt(manager_t *c, const char *state, int error, char **out)
{
    char *text = c->memory->alloc(1024); if (!text) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    int result = format(c, text, 1024,
        "{\"schema\":1,\"kind\":\"himax\",\"provider_version\":\"" SOLAR_OS_HIMAX_PROVIDER_VERSION "\",\"device\":\"%s\",\"source\":\"%s\",\"firmware\":\"%s\","
        "\"id\":\"%s\",\"version\":\"%s\",\"sha256\":\"%s\",\"slot\":%lu,\"previous_slot\":%lu,"
        "\"bytes\":%lu,\"written_bytes\":%lu,\"state\":\"%s\",\"error\":%d}",
        c->device,c->source,c->firmware,c->id,c->version,c->sha,(unsigned long)c->slot,
        (unsigned long)c->previous,(unsigned long)c->bytes,(unsigned long)c->written,state,error);
    if (!result) result = c->files->replace(c->record, text, strlen(text));
    if (!result && out) { *out = text; text = NULL; }
    c->memory->free(text); return result;
}
int solar_os_himax_model_upload(solar_os_processor_service_fn services, const char *source,
    const char *file, uint32_t slot, uint32_t flags, uint32_t timeout,
    solar_os_native_cancel_fn cancel, solar_os_processor_progress_fn report, void *user, char **out)
{
    if (!out) return BAD;
    *out = NULL;
    manager_t s = {.cancel = cancel, .progress = report, .user = user, .slot = slot};
    manager_t *c = &s;
    int error = bind(c, services, source, timeout);
    /* Keep the first recovery slot and the currently active slot intact.
     * Unknown/zero slot sizes do not authorize replacement implicitly. */
    if (!error && (slot < 2 || slot > 4 || flags != SOLAR_OS_PROCESSOR_UPLOAD_REPLACE)) error = BAD;
    if (!error && (c->camera->struct_size < sizeof(*c->camera) || !c->camera->spi || !c->camera->send || !c->camera->reset)) error = UNSUPPORTED;
    if (!error) {
        c->files = services(SOLAR_OS_NATIVE_FILES_SERVICE, 1, sizeof(*c->files));
        c->json = services(SOLAR_OS_NATIVE_JSON_SERVICE, 1, sizeof(*c->json));
        if (!c->files || !c->json) error = UNSUPPORTED;
    }
    if (!error && stopped(c)) error = CANCELLED;
    if (!error) { progress(c, "verify bundle", 0, 0); error = bundle(c, file); }
    if (!error) error = c->camera->open(source, "himax-upload", &c->token);
    if (!error) error = c->camera->spi(c->token, NULL, NULL, 0);
    if (!error) error = device_layout(c);
    if (!error && stopped(c)) error = CANCELLED;
    if (!error) { error = receipt(c, "uploading", 0, NULL); c->recorded = !error; }
    if (!error) error = program(c);
    if (!error) {
        progress(c, "restart", c->written, c->bytes);
        error = c->camera->reset(c->token);
        c->maintenance = error != 0;
        if (!error) error = smoke_test(c);
    }
    if (c->token && c->recorded) {
        /* Cancellation cannot skip returning the device to its previous model. */
        c->cancel = NULL;
        progress(c, "restore selection", c->written, c->bytes);
        int recovered = c->camera->reset(c->token);
        if (!recovered) recovered = select_slot(c, c->previous);
        if (!error) error = recovered;
    } else if (c->token && c->maintenance) {
        int recovered = c->camera->reset(c->token); if (!error) error = recovered;
    }
    if (!error) {
        char sha[65]; error = c->files->sha256(c->model, sha, cancel, user);
        if (!error && strcmp(sha, c->sha)) error = FAILED;
    }
    if (c->recorded) {
        int saved = receipt(c, error ? "failed" : "programmed", error, error ? NULL : out);
        if (!error) error = saved;
    }
    close_owner(c);
    if (c->files) c->files->close(c->model);
    if (c->json) c->json->free(c->manifest);
    if (!error) progress(c, "programmed", c->bytes, c->bytes);
    return error;
}
