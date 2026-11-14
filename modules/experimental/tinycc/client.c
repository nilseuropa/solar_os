/* Native compiler command. All storage and runtime access uses versioned APIs. */
#include <stdarg.h>
#include <string.h>
#include "solar_os_cc_abi.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_native_storage_abi.h"
#include "solar_os_native_elf.h"
#include "sdk_reader.h"
#include "sdk_cli.h"

static const char library_source[] =
#include "library_probe.inc"
;
static const char scalar_source[] =
#include "scalar_probe.inc"
;
static const solar_os_native_host_api_v1_t *host;
static const solar_os_native_libc_api_v1_t *libc_api;
static const solar_os_cc_api_v1_t *compiler;
static char diagnostics[512];
/* Private helpers for the production ELF validator linked into this client. */
int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
void *memchr(const void *data, int ch, size_t bytes)
{ const unsigned char *p = data; for (size_t i = 0; i < bytes; ++i) if (p[i] == (unsigned char)ch) return (void *)(p+i); return NULL; }
int snprintf(char *buffer, size_t bytes, const char *format, ...)
{ va_list args; va_start(args, format); int n = libc_api->format(buffer, bytes, format, args); va_end(args); return n; }
static void print(const char *format, ...)
{
    char text[256]; va_list args; va_start(args, format);
    int bytes = libc_api->format(text, sizeof(text), format, args); va_end(args);
    if (bytes > 0) host->write_utf8(text, bytes < sizeof(text) ? bytes : sizeof(text)-1);
}
static bool equal(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }
static bool cancel_after(void *user)
{ unsigned *count = user; return ++*count >= 3; }
static solar_os_cc_result_v1_t metrics;
static int compile(solar_os_cc_request_v1_t *request, void *output, unsigned capacity)
{
    metrics = (solar_os_cc_result_v1_t){.struct_size = sizeof(metrics)};
    return compiler->compile(request, output, capacity, &metrics, diagnostics, sizeof(diagnostics));
}
static int self_test(void *output, unsigned capacity)
{
    solar_os_cc_request_v1_t request = {
        .struct_size = sizeof(request), .source = library_source, .source_bytes = sizeof(library_source)-1,
    };
    uint32_t expected_hash = 0, expected_bytes = 0;
    for (unsigned i = 0; i < 5; i++) {
        request.source = library_source; request.source_bytes = sizeof(library_source)-1;
        int code = compile(&request, output, capacity);
        if (code || metrics.output_bytes < 52 || memcmp(output, "\177ELF\1\1\1", 7)) {
            print("CC_ERROR valid compile=%d: %s\n", code, diagnostics); return 1;
        }
        print("CC_COMPILE bytes=%u peak=%u ms=%u\n", metrics.output_bytes, metrics.peak_heap_bytes, metrics.elapsed_ms);
        uint32_t hash = 2166136261U;
        for (unsigned n = 0; n < metrics.output_bytes; n++) hash = (hash ^ ((const unsigned char *)output)[n]) * 16777619U;
        if (!i) { expected_hash = hash; expected_bytes = metrics.output_bytes; }
        else if (hash != expected_hash || metrics.output_bytes != expected_bytes) {
            print("CC_ERROR repeated output changed bytes=%u hash=%x expected=%x\n", metrics.output_bytes, hash, expected_hash); return 11;
        }
        request.source = "int main(void) { return missing + ; }"; request.source_bytes = strlen(request.source);
        code = compile(&request, output, capacity);
        if (code != SOLAR_OS_NATIVE_ERROR_FAILED || metrics.output_bytes) {
            print("CC_ERROR syntax recovery=%d\n", code); return 2;
        }
    }
    request.source = scalar_source; request.source_bytes = sizeof(scalar_source)-1;
    if (compile(&request, output, capacity)) { print("CC_ERROR scalar: %s\n", diagnostics); return 3; }
    request.source = library_source; request.source_bytes = sizeof(library_source)-1;
    request.heap_limit = 65536;
    if (compile(&request, output, capacity) != SOLAR_OS_NATIVE_ERROR_NO_MEMORY || metrics.output_bytes) return 4;
    request.heap_limit = 0;
    if (compile(&request, output, 32) != SOLAR_OS_NATIVE_ERROR_INVALID_SIZE || metrics.output_bytes) return 5;
    unsigned polls = 0; request.cancel = cancel_after; request.cancel_user = &polls;
    if (compile(&request, output, capacity) != SOLAR_OS_NATIVE_ERROR_TIMEOUT || metrics.output_bytes) return 6;
    request.cancel = NULL; request.cancel_user = NULL;
    request.timeout_ms = 1;
    if (compile(&request, output, capacity) != SOLAR_OS_NATIVE_ERROR_TIMEOUT || metrics.output_bytes) return 7;
    request.timeout_ms = 0;
    char nested[800]; unsigned length = 0;
    const char *prefix = "int main(void) { return ";
    while (*prefix) nested[length++] = *prefix++;
    for (unsigned i = 0; i < 300; i++) nested[length++] = '(';
    nested[length++] = '0';
    for (unsigned i = 0; i < 300; i++) nested[length++] = ')';
    nested[length++] = ';'; nested[length++] = '}'; nested[length] = 0;
    request.source = nested; request.source_bytes = length;
    if (compile(&request, output, capacity) != SOLAR_OS_NATIVE_ERROR_INVALID_SIZE || metrics.output_bytes) return 12;
    request.source = "int main(int n) { int a[n]; return a[0]; }"; request.source_bytes = strlen(request.source);
    if (compile(&request, output, capacity) != SOLAR_OS_NATIVE_ERROR_FAILED) return 8;
    const char extra[] = "#define EXTRA 42\n";
    solar_os_cc_header_v1_t header = {"extra.h", extra, sizeof(extra)-1};
    request.source = "#include <solar_os_native_abi.h>\n#include <extra.h>\n"
                     "int main(void) { return solar_os_native_host_v1() ? EXTRA : 1; }";
    request.source_bytes = strlen(request.source); request.headers = &header; request.header_count = 1;
    if (compile(&request, output, capacity)) { print("CC_ERROR header: %s\n", diagnostics); return 9; }
    request.headers = NULL; request.header_count = 0;
    request.source = library_source; request.source_bytes = sizeof(library_source)-1;
    if (compile(&request, output, capacity)) { print("CC_ERROR final recovery: %s\n", diagnostics); return 10; }
    print("CC_SELF_TEST_OK syntax, heap, stack, output, cancellation, deadline, headers, recovery\n");
    return 0;
}

static const char *error_text(int code)
{
    switch (code) {
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: return "not found";
    case SOLAR_OS_NATIVE_ERROR_INVALID_SIZE: return "file or output exceeds the supported size";
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: return "not enough memory";
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: return "cancelled or timed out";
    case SOLAR_OS_NATIVE_ERROR_INVALID_STATE: return "file is busy";
    default: return "operation failed";
    }
}
static int read_header(void *user, const char *name, void *buffer, uint32_t capacity, uint32_t *bytes)
{
    const solar_os_native_storage_api_v1_t *storage = user;
    if(strlen(name)>=12 && !memcmp(name,"/module-sdk/",12))
        return solar_os_cc_sdk_read(storage,name,buffer,capacity,bytes);
    char path[SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    int code = storage->resolve_path(storage->context, name, path, sizeof(path));
    if (code) return code;
    if (!buffer) return storage->file_size(storage->context, path, bytes);
    size_t count = 0;
    code = storage->read_file(storage->context, path, 0, buffer, capacity, &count);
    *bytes = count;
    return code;
}
static int compile_file(int argc, char **argv, const solar_os_native_memory_api_v1_t *memory,
                         void *output, unsigned capacity)
{
    const solar_os_native_storage_api_v1_t *storage = host->get_service("storage", 1, SOLAR_OS_NATIVE_STORAGE_BASE_SIZE);
    if (!storage) { print("cc: file compilation requires the native storage service\n"); return -6; }
    const char *inputs[SOLAR_OS_CC_SOURCE_FILES_MAX],*output_name="a.elf";
    unsigned input_count=0;
    bool output_seen = false;
    char include_paths[8][SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    const char *includes[8]; unsigned include_count = 0;
    for (int i = 1; i < argc; ++i) {
        if (equal(argv[i], "-o")) {
            if (output_seen || ++i == argc) { print("cc: -o requires one output path\n"); return -2; }
            output_seen = true; output_name = argv[i];
        } else if (argv[i][0] == '-' && argv[i][1] == 'I') {
            const char *path = argv[i] + 2;
            if (!*path && ++i < argc) path = argv[i];
            if (!*path || include_count == 8) { print("cc: expected an include path (maximum eight)\n"); return -2; }
            int code = storage->resolve_path(storage->context, path, include_paths[include_count], sizeof(include_paths[0]));
            if (code) { print("cc: include path: %s\n", error_text(code)); return code; }
            includes[include_count] = include_paths[include_count]; include_count++;
        } else if (argv[i][0] == '-') { print("cc: unknown option %s\n", argv[i]); return -2; }
        else if(input_count==SOLAR_OS_CC_SOURCE_FILES_MAX) {print("cc: maximum eight source files\n");return -2;}
        else inputs[input_count++]=argv[i];
    }
    if(!input_count) {print("usage: cc input.c [more.c ...] [-I directory] [-o output.elf]\n");return -2;}
    char source_paths[SOLAR_OS_CC_SOURCE_FILES_MAX][SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    solar_os_cc_source_v1_t sources[SOLAR_OS_CC_SOURCE_FILES_MAX]={0};
    char output_path[SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    int code=storage->resolve_path(storage->context,output_name,output_path,sizeof(output_path));
    if (code) { print("cc: path: %s\n", error_text(code)); return code; }
    uint32_t total=0;
    for(unsigned i=0;i<input_count;i++) {
        code=storage->resolve_path(storage->context,inputs[i],source_paths[i],sizeof(source_paths[i]));
        if(code) break;
        if(equal(source_paths[i],output_path)) {print("cc: output must differ from the source file\n");code=-2;break;}
        uint32_t source_bytes=0;
        code=storage->file_size(storage->context,source_paths[i],&source_bytes);
        if(code) {print("cc: %s: %s\n",inputs[i],error_text(code));break;}
        if(!source_bytes || source_bytes>SOLAR_OS_CC_SOURCE_MAX-total) {
            print("cc: source must contain 1..65536 bytes in total\n");code=-10;break;
        }
        char *source=memory->alloc((size_t)source_bytes+1);
        if(!source) {code=-4;break;}
        sources[i]=(solar_os_cc_source_v1_t){source_paths[i],source,source_bytes};
        size_t count=0;
        code=storage->read_file(storage->context,source_paths[i],0,source,source_bytes,&count);
        if(!code && (count!=source_bytes || memchr(source,0,count))) code=SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        source[source_bytes]=0;total+=source_bytes;
        if(code) {print("cc: %s: %s\n",inputs[i],error_text(code));break;}
    }
    if(code) {
        for(unsigned i=0;i<input_count;i++) if(sources[i].text) memory->free((void *)sources[i].text);
        return code;
    }
    solar_os_cc_request_v1_t request = {
        .struct_size = sizeof(request), .sources=sources,.source_count=input_count,
        .timeout_ms=20000,
        .include_paths = includes, .include_count = include_count,
        .read_header = read_header, .header_user = (void *)storage,
        .cancel = storage->stop_requested, .cancel_user = storage->context,
    };
    code = compile(&request, output, capacity);
    for(unsigned i=0;i<input_count;i++) memory->free((void *)sources[i].text);
    if (code) {
        print("cc: compilation failed (%d)\n", code);
        if (*diagnostics) host->write_utf8(diagnostics, strlen(diagnostics));
        return code;
    }
    char detail[160];
    if (solar_os_native_elf_validate(output, metrics.output_bytes, SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                     NULL, detail, sizeof(detail)) != ESP_OK) {
        print("cc: invalid native output: %s\n", detail); return -1;
    }
    code = storage->replace_file(storage->context, output_path, output, metrics.output_bytes);
    if (code) { print("cc: %s: %s\n", output_name, error_text(code)); return code; }
    print("cc: %s (%u bytes, %u ms)\n", output_name, metrics.output_bytes, metrics.elapsed_ms);
    return 0;
}
int app_main(int argc, char **argv)
{
    host = solar_os_native_host_v1();
    if (!host || host->struct_size < sizeof(*host) || !host->get_service) return 1;
    libc_api = host->get_service("libc", 1, offsetof(solar_os_native_libc_api_v1_t,parse_float));
    const solar_os_native_memory_api_v1_t *memory = host->get_service("memory", 1, sizeof(*memory));
    const solar_os_native_library_service_api_v1_t *libraries = host->get_service("library", 1, sizeof(*libraries));
    if (!libc_api || !memory || !libraries) return 2;
    if (argc < 2 || equal(argv[1], "--help")) {
        print("usage: cc input.c [more.c ...] [-I directory] [-o output.elf]\n");
        print("       cc sdk list | show HEADER | export NEW_DIRECTORY\n");
        return argc < 2 ? 2 : 0;
    }
    const void *table = NULL; uint32_t lease = 0;
    int code = libraries->acquire(SOLAR_OS_CC_LIBRARY, SOLAR_OS_CC_ABI, SOLAR_OS_CC_API_BASE_SIZE, &lease, &table);
    if (code) { print("CC_ERROR acquire=%d\n", code); return code; }
    compiler = table;
    if(equal(argv[1],"sdk")) {
        code=solar_os_cc_sdk_command(host,libc_api,compiler,memory,argc,argv);
        libraries->release(lease);return code;
    }
    bool diagnostic_mode = equal(argv[1], "--self-test") || equal(argv[1], "--demo-hex") ||
                           equal(argv[1], "--scalar-hex") || equal(argv[1], "--emit-hex");
    const unsigned capacity = diagnostic_mode ? 256 * 1024 : SOLAR_OS_CC_OUTPUT_MAX;
    void *output = memory->alloc(capacity);
    if (!output) { print("cc: not enough memory for output\n"); libraries->release(lease); return -4; }
    if (equal(argv[1], "--self-test")) {
        code = self_test(output, capacity);
        if (code) print("CC_ERROR self-test=%d: %s\n", code, diagnostics);
    } else if (equal(argv[1], "--demo-hex") || equal(argv[1], "--scalar-hex") ||
               (argc == 3 && equal(argv[1], "--emit-hex"))) {
        const char *source = argc == 3 ? argv[2] : equal(argv[1], "--scalar-hex") ? scalar_source : library_source;
        solar_os_cc_request_v1_t request = {.struct_size = sizeof(request), .source = source, .source_bytes = strlen(source)};
        code = compile(&request, output, capacity);
        if (code) print("CC_ERROR compile=%d: %s\n", code, diagnostics);
        else {
            print("CC_ELF_BEGIN bytes=%u peak=%u ms=%u\n", metrics.output_bytes, metrics.peak_heap_bytes, metrics.elapsed_ms);
            for (unsigned offset = 0; offset < metrics.output_bytes; offset += 16) {
                const unsigned char *bytes = output; char hex[33];
                unsigned count = metrics.output_bytes - offset; if (count > 16) count = 16;
                for (unsigned i = 0; i < count; i++) {
                    hex[2*i] = "0123456789abcdef"[bytes[offset+i] >> 4];
                    hex[2*i+1] = "0123456789abcdef"[bytes[offset+i] & 15];
                }
                hex[count*2] = 0; print("CC_ELF %08x %s\n", offset, hex);
            }
            print("CC_ELF_END\n");
        }
    } else code = compile_file(argc, argv, memory, output, capacity);
    memory->free(output); libraries->release(lease); return code;
}
