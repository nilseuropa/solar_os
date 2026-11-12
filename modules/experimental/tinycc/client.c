/* Experimental compiler client: no filesystem or kernel-private imports. */
#include <stdarg.h>
#include <string.h>
#include "solar_os_cc_abi.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_native_libc_abi.h"

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
    request.source = "double x=1.5; int main(void) { return 0; }"; request.source_bytes = strlen(request.source);
    if (compile(&request, output, capacity) != SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED) return 8;
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
int app_main(int argc, char **argv)
{
    host = solar_os_native_host_v1();
    if (!host || host->struct_size < sizeof(*host) || !host->get_service) return 1;
    libc_api = host->get_service("libc", 1, sizeof(*libc_api));
    const solar_os_native_memory_api_v1_t *memory = host->get_service("memory", 1, sizeof(*memory));
    const solar_os_native_library_service_api_v1_t *libraries = host->get_service("library", 1, sizeof(*libraries));
    if (!libc_api || !memory || !libraries) return 2;
    if (argc != 2 && argc != 3) { print("usage: cc --self-test | --demo-hex | --emit-hex SOURCE\n"); return 2; }
    const void *table = NULL; uint32_t lease = 0;
    int code = libraries->acquire(SOLAR_OS_CC_LIBRARY, SOLAR_OS_CC_ABI, sizeof(*compiler), &lease, &table);
    if (code) { print("CC_ERROR acquire=%d\n", code); return code; }
    compiler = table;
    const unsigned capacity = 256 * 1024;
    void *output = memory->alloc(capacity);
    if (!output) { libraries->release(lease); return -4; }
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
    } else { code = 2; print("usage: cc --self-test | --demo-hex | --emit-hex SOURCE\n"); }
    memory->free(output); libraries->release(lease); return code;
}
