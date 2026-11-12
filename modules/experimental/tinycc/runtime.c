/* SPDX-License-Identifier: LGPL-2.0-or-later
 * Private C runtime and bounded, joined compilation worker. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>
#include <time.h>
#include <unistd.h>
#include "solar_os_cc_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_native_thread_abi.h"
#include "solar_os_native_time_abi.h"

static const solar_os_native_memory_api_v1_t *memory_api;
static const solar_os_native_libc_api_v1_t *libc_api;
static const solar_os_native_thread_api_v1_t *threads;
static const solar_os_native_time_api_v1_t *clock_api;
extern unsigned char __cc_data_start[], __cc_data_end[], __cc_bss_start[], __cc_bss_end[];
static void *data_snapshot;
static unsigned busy;
static jmp_buf recovery;
static const solar_os_cc_request_v1_t *request;
static solar_os_cc_result_v1_t *result;
static void *destination;
static uint32_t destination_size;
static char *diagnostics;
static size_t diagnostics_size, diagnostics_used;
static uintptr_t stack_anchor;
static int status, private_errno;
static size_t allocated, peak, heap_limit;
static int64_t began, yielded;
static unsigned polls;
static bool worker_active;
struct allocation { struct allocation *next, *previous; size_t bytes, reserved; };
static struct allocation *allocations;
static const solar_os_cc_header_v1_t sdk[] = {
#include "sdk.inc"
};
struct virtual_file { const char *data; uint32_t bytes, offset; bool open; };
static struct virtual_file files[32];
extern int solar_os_cc_generate(const solar_os_cc_request_v1_t *, void **, size_t *);
extern int solar_tcc_module(const void *, size_t, const char *, void **, size_t *, const char **);

void solar_os_cc_diagnostic(void *unused, const char *text)
{
    (void)unused;
    if (!diagnostics_size) return;
    while (*text && diagnostics_used + 1 < diagnostics_size)
        diagnostics[diagnostics_used++] = *text++;
    if (diagnostics_used + 1 < diagnostics_size) diagnostics[diagnostics_used++] = '\n';
    diagnostics[diagnostics_used] = 0;
}
static __attribute__((noreturn)) void fail(int code, const char *message)
{
    status = code;
    solar_os_cc_diagnostic(NULL, message);
    longjmp(recovery, 1);
}
void solar_os_cc_checkpoint(void)
{
    char stack_position;
    if (!worker_active) return;
    uintptr_t here = (uintptr_t)&stack_position;
    size_t depth = here < stack_anchor ? stack_anchor - here : here - stack_anchor;
    if (depth > 12000) fail(SOLAR_OS_NATIVE_ERROR_INVALID_SIZE, "compiler stack limit exceeded");
    if ((++polls & 63) != 0) return;
    if (request->cancel && request->cancel(request->cancel_user))
        fail(SOLAR_OS_NATIVE_ERROR_TIMEOUT, "compilation cancelled");
    int64_t now = clock_api->now_us();
    if (now - began >= (int64_t)(request->timeout_ms ? request->timeout_ms : 2000) * 1000)
        fail(SOLAR_OS_NATIVE_ERROR_TIMEOUT, "compiler time limit exceeded");
    if (now - yielded >= 1000) { clock_api->sleep_ms(1); yielded = now; }
}
void __cyg_profile_func_enter(void *function, void *caller)
{
    (void)function; (void)caller;
    char position; uintptr_t here = (uintptr_t)&position;
    if (worker_active && here < stack_anchor && stack_anchor - here > 12000)
        fail(SOLAR_OS_NATIVE_ERROR_INVALID_SIZE, "compiler stack limit exceeded");
}
void __cyg_profile_func_exit(void *function, void *caller)
{ (void)function; (void)caller; }
void *malloc(size_t bytes)
{
    solar_os_cc_checkpoint();
    if (!bytes) bytes = 1;
    if (bytes > SIZE_MAX - sizeof(struct allocation) ||
        bytes + sizeof(struct allocation) > heap_limit - allocated)
        fail(SOLAR_OS_NATIVE_ERROR_NO_MEMORY, "compiler heap limit exceeded");
    struct allocation *record = memory_api->alloc(bytes + sizeof(*record));
    if (!record) fail(SOLAR_OS_NATIVE_ERROR_NO_MEMORY, "compiler allocation failed");
    *record = (struct allocation){allocations, NULL, bytes, 0};
    if (allocations) allocations->previous = record;
    allocations = record; allocated += bytes + sizeof(*record);
    if (allocated > peak) peak = allocated;
    return record + 1;
}
void free(void *pointer)
{
    if (!pointer) return;
    struct allocation *record = (struct allocation *)pointer - 1;
    if (record->previous) record->previous->next = record->next;
    else allocations = record->next;
    if (record->next) record->next->previous = record->previous;
    allocated -= record->bytes + sizeof(*record); memory_api->free(record);
}
void *realloc(void *pointer, size_t bytes)
{
    if (!bytes) { free(pointer); return NULL; }
    void *replacement = malloc(bytes);
    if (pointer) {
        size_t before = ((struct allocation *)pointer - 1)->bytes;
        memcpy(replacement, pointer, before < bytes ? before : bytes); free(pointer);
    }
    return replacement;
}
void *calloc(size_t count, size_t bytes)
{
    if (bytes && count > SIZE_MAX / bytes) fail(SOLAR_OS_NATIVE_ERROR_NO_MEMORY, "allocation size overflow");
    void *pointer = malloc(count * bytes); memset(pointer, 0, count * bytes); return pointer;
}
void *solar_os_cc_reallocate(void *pointer, unsigned long bytes) { return realloc(pointer, bytes); }
int *__errno(void) { return &private_errno; }
struct _reent *__getreent(void)
{
    static struct _reent io;
    io._stdout = (FILE *)1; io._stderr = (FILE *)2; return &io;
}
void exit(int code) { (void)code; fail(SOLAR_OS_NATIVE_ERROR_FAILED, "compiler aborted"); }
int vsnprintf(char *buffer, size_t bytes, const char *format, va_list args)
{ return libc_api->format(buffer, bytes, format, args); }
int snprintf(char *buffer, size_t bytes, const char *format, ...)
{ va_list args; va_start(args, format); int n = vsnprintf(buffer, bytes, format, args); va_end(args); return n; }
int sprintf(char *buffer, const char *format, ...)
{ va_list args; va_start(args, format); int n = vsnprintf(buffer, INT_MAX, format, args); va_end(args); return n; }
int fprintf(FILE *stream, const char *format, ...)
{
    (void)stream; char text[512]; va_list args; va_start(args, format);
    int n = vsnprintf(text, sizeof(text), format, args); va_end(args);
    solar_os_cc_diagnostic(NULL, text); return n;
}
int vfprintf(FILE *stream, const char *format, va_list args)
{
    (void)stream; char text[512]; int n = vsnprintf(text, sizeof(text), format, args);
    solar_os_cc_diagnostic(NULL, text); return n;
}
int printf(const char *format, ...)
{
    char text[512]; va_list args; va_start(args, format);
    int n = vsnprintf(text, sizeof(text), format, args); va_end(args);
    solar_os_cc_diagnostic(NULL, text); return n;
}
int fputs(const char *text, FILE *stream) { (void)stream; solar_os_cc_diagnostic(NULL, text); return 0; }
int puts(const char *text) { return fputs(text, NULL); }
int fputc(int ch, FILE *stream) { char text[2] = {ch, 0}; return fputs(text, stream); }
int putchar(int ch) { return fputc(ch, NULL); }
int fflush(FILE *stream) { (void)stream; return 0; }
FILE *fdopen(int fd, const char *mode) { (void)fd; (void)mode; return NULL; }
int fclose(FILE *stream) { (void)stream; return 0; }
size_t fwrite(const void *p, size_t size, size_t count, FILE *stream)
{ (void)p; (void)size; (void)count; (void)stream; fail(-6, "compiler disk output is disabled"); }
char *strerror(int code) { (void)code; return "virtual file unavailable"; }
void *memchr(const void *data, int ch, size_t bytes)
{ const unsigned char *p = data; for (size_t i = 0; i < bytes; i++) if (p[i] == (unsigned char)ch) return (void *)(p+i); return NULL; }
int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n)
{ while (n && *a && *a == *b) { a++; b++; n--; } return n ? (unsigned char)*a - (unsigned char)*b : 0; }
char *strcpy(char *to, const char *from) { char *out = to; while ((*to++ = *from++)); return out; }
char *strchr(const char *text, int ch)
{ for (;;) { if (*text == (char)ch) return (char *)text; if (!*text++) return NULL; } }
char *strrchr(const char *text, int ch)
{ const char *found = NULL; do { if (*text == (char)ch) found = text; } while (*text++); return (char *)found; }
static void swap(unsigned char *a, unsigned char *b, size_t bytes)
{ while (bytes--) { unsigned char t = *a; *a++ = *b; *b++ = t; } }
static void sift(unsigned char *p, size_t count, size_t root, size_t bytes, int (*compare)(const void *, const void *))
{
    while (root < count / 2) {
        size_t child = root * 2 + 1;
        if (child + 1 < count && compare(p + child*bytes, p + (child+1)*bytes) < 0) child++;
        if (compare(p + root*bytes, p + child*bytes) >= 0) return;
        swap(p + root*bytes, p + child*bytes, bytes); root = child;
        solar_os_cc_checkpoint();
    }
}
void qsort(void *data, size_t count, size_t bytes, int (*compare)(const void *, const void *))
{
    unsigned char *p = data;
    for (size_t i = count / 2; i; i--) sift(p, count, i-1, bytes, compare);
    for (size_t i = count; i > 1; i--) { swap(p, p + (i-1)*bytes, bytes); sift(p, i-1, 0, bytes, compare); }
}
static unsigned long long integer(const char *text, char **end, int base, bool *negative)
{
    const char *begin = text; unsigned long long value = 0; bool any = false, overflow = false;
    while (*text == ' ' || (*text >= '\t' && *text <= '\r')) text++;
    *negative = *text == '-'; if (*text == '-' || *text == '+') text++;
    if ((!base || base == 16) && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text += 2; base = 16;
    }
    if (!base) base = *text == '0' ? 8 : 10;
    if (base < 2 || base > 36) { if (end) *end = (char *)begin; private_errno = EINVAL; return 0; }
    for (;;) {
        unsigned digit = *text >= '0' && *text <= '9' ? *text-'0' :
                         *text >= 'a' && *text <= 'z' ? *text-'a'+10 :
                         *text >= 'A' && *text <= 'Z' ? *text-'A'+10 : 36;
        if (digit >= (unsigned)base) break;
        if (value > (ULLONG_MAX - digit) / base) overflow = true;
        value = value * base + digit; any = true; text++; solar_os_cc_checkpoint();
    }
    if (end) *end = (char *)(any ? text : begin);
    if (overflow) { private_errno = ERANGE; return ULLONG_MAX; }
    return value;
}
unsigned long long strtoull(const char *text, char **end, int base)
{ bool negative; unsigned long long value = integer(text, end, base, &negative); return negative ? -value : value; }
unsigned long strtoul(const char *text, char **end, int base)
{
    bool negative; unsigned long long value = integer(text, end, base, &negative);
    if (value > ULONG_MAX) { private_errno = ERANGE; return ULONG_MAX; }
    return negative ? -(unsigned long)value : value;
}
long strtol(const char *text, char **end, int base)
{
    bool negative; unsigned long long value = integer(text, end, base, &negative);
    if (value > (unsigned long)LONG_MAX + negative) { private_errno = ERANGE; return negative ? LONG_MIN : LONG_MAX; }
    return negative ? (long)(0UL - (unsigned long)value) : (long)value;
}
int atoi(const char *text) { return strtol(text, NULL, 10); }
float strtof(const char *text, char **end)
{ (void)text; (void)end; fail(-6, "floating literals are unsupported by the embedded compiler"); }
double strtod(const char *text, char **end)
{ (void)text; (void)end; fail(-6, "floating literals are unsupported by the embedded compiler"); }
long double strtold(const char *text, char **end)
{ (void)text; (void)end; fail(-6, "floating literals are unsupported by the embedded compiler"); }
long double ldexpl(long double value, int exponent)
{ (void)value; (void)exponent; fail(-6, "floating literals are unsupported by the embedded compiler"); }
char *getcwd(char *buffer, size_t bytes)
{ if (bytes < 5) { private_errno = ERANGE; return NULL; } return strcpy(buffer, "/sdk"); }
char *realpath(const char *name, char *buffer)
{ if (!buffer) buffer = malloc(strlen(name) + 1); return strcpy(buffer, name); }
time_t time(time_t *value) { if (value) *value = 0; return 0; }
struct tm *localtime(const time_t *value)
{ (void)value; static struct tm epoch = {.tm_mday=1, .tm_year=70, .tm_wday=4}; return &epoch; }
int open(const char *path, int flags, ...)
{
    if (flags != O_RDONLY) { private_errno = EACCES; return -1; }
    while (!strncmp(path, "./", 2)) path += 2;
    if (!strncmp(path, "/sdk/", 5)) path += 5;
    const solar_os_cc_header_v1_t *header = NULL;
    for (unsigned i = 0; i < request->header_count; i++)
        if (!strcmp(path, request->headers[i].name)) header = request->headers + i;
    if (!header) for (unsigned i = 0; i < sizeof(sdk)/sizeof(sdk[0]); i++)
        if (!strcmp(path, sdk[i].name)) header = sdk + i;
    if (!header) { private_errno = ENOENT; return -1; }
    for (unsigned i = 0; i < 32; i++) if (!files[i].open) {
        files[i] = (struct virtual_file){header->text, header->bytes, 0, true}; return i + 3;
    }
    private_errno = EMFILE; return -1;
}
ssize_t read(int fd, void *buffer, size_t bytes)
{
    solar_os_cc_checkpoint();
    if (fd < 3 || fd >= 35 || !files[fd-3].open) { private_errno = EBADF; return -1; }
    struct virtual_file *file = files + fd - 3;
    if (bytes > file->bytes - file->offset) bytes = file->bytes - file->offset;
    memcpy(buffer, file->data + file->offset, bytes); file->offset += bytes; return bytes;
}
int close(int fd)
{ if (fd < 3 || fd >= 35 || !files[fd-3].open) return -1; files[fd-3].open = false; return 0; }
off_t lseek(int fd, off_t offset, int whence)
{
    if (fd < 3 || fd >= 35 || !files[fd-3].open) return -1;
    struct virtual_file *file = files + fd - 3;
    int64_t position = offset + (whence == SEEK_SET ? 0 : whence == SEEK_CUR ? file->offset : file->bytes);
    if (position < 0 || position > file->bytes) return -1;
    file->offset = position; return position;
}
int unlink(const char *path) { (void)path; private_errno = EACCES; return -1; }

static void reset_core(void)
{
    memcpy(__cc_data_start, data_snapshot, __cc_data_end - __cc_data_start);
    memset(__cc_bss_start, 0, __cc_bss_end - __cc_bss_start);
}
static void compile_worker(void *unused)
{
    (void)unused; char position;
    stack_anchor = (uintptr_t)&position; began = yielded = clock_api->now_us();
    worker_active = true;
    if (!setjmp(recovery)) {
        void *object = NULL, *elf = NULL; size_t object_bytes = 0, elf_bytes = 0;
        const char *error = NULL;
        status = solar_os_cc_generate(request, &object, &object_bytes);
        if (!status && solar_tcc_module(object, object_bytes, request->entry ? request->entry : "main", &elf, &elf_bytes, &error)) {
            solar_os_cc_diagnostic(NULL, error); status = SOLAR_OS_NATIVE_ERROR_FAILED;
        }
        if (!status && elf_bytes > destination_size) status = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        if (!status) { memcpy(destination, elf, elf_bytes); result->output_bytes = elf_bytes; }
    }
    worker_active = false;
    result->peak_heap_bytes = peak;
    result->elapsed_ms = (clock_api->now_us() - began) / 1000;
    /* No compiler pointer escapes. Sweeping and restoring private globals also
     * handles OOM/cancel during partially initialized TinyCC data structures. */
    while (allocations) free(allocations + 1);
    memset(files, 0, sizeof(files)); reset_core();
}
static size_t bounded_length(const char *text, size_t maximum)
{ size_t n = 0; if (text) while (n < maximum && text[n]) n++; return n; }
static int compile(const solar_os_cc_request_v1_t *input, void *output, uint32_t capacity,
                   solar_os_cc_result_v1_t *metrics, char *messages, uint32_t message_bytes)
{
    if (metrics && metrics->struct_size >= sizeof(*metrics))
        *metrics = (solar_os_cc_result_v1_t){.struct_size = sizeof(*metrics)};
    if (!input || input->struct_size < sizeof(*input) || !metrics || metrics->struct_size < sizeof(*metrics) ||
        !input->source || !input->source_bytes || input->source_bytes > SOLAR_OS_CC_SOURCE_MAX ||
        bounded_length(input->source, input->source_bytes + 1) != input->source_bytes ||
        input->header_count > 32 || (input->header_count && !input->headers) ||
        !output || !capacity || capacity > SOLAR_OS_CC_OUTPUT_MAX ||
        !messages || !message_bytes || message_bytes > 4096 ||
        (input->heap_limit && (input->heap_limit < 65536 || input->heap_limit > SOLAR_OS_CC_HEAP_MAX)) ||
        input->timeout_ms > 5000 || (input->entry && bounded_length(input->entry, 64) >= 64))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    size_t header_bytes = 0;
    for (unsigned i = 0; i < input->header_count; i++) {
        const solar_os_cc_header_v1_t *h = input->headers + i;
        if (!h->name || !*h->name || bounded_length(h->name, 64) >= 64 || !h->text ||
            h->bytes > 32768 || memchr(h->text, 0, h->bytes)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        header_bytes += h->bytes;
    }
    if (header_bytes > 262144) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    request = input; destination = output; destination_size = capacity; result = metrics;
    diagnostics = messages; diagnostics_size = message_bytes; diagnostics_used = 0; messages[0] = 0;
    allocated = peak = polls = 0; heap_limit = input->heap_limit ? input->heap_limit : SOLAR_OS_CC_HEAP_MAX;
    status = 0; private_errno = 0; reset_core();
    void *worker = NULL;
    int error = threads->worker_start(&worker, compile_worker, NULL, 16384, 0);
    if (!error) error = threads->worker_join(worker);
    request = NULL; diagnostics = NULL; diagnostics_size = 0;
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    return error ? SOLAR_OS_NATIVE_ERROR_NO_MEMORY : status;
}
static int shutdown(void)
{
    if (busy) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    memory_api->free(data_snapshot); data_snapshot = NULL; return 0;
}
static const solar_os_cc_api_v1_t api = {SOLAR_OS_CC_ABI, sizeof(api), compile};
int app_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const solar_os_native_library_host_api_v1_t *host = solar_os_native_library_host_v1();
    if (!host || host->abi_version != 1 || host->struct_size < sizeof(*host) ||
        !host->get_service || !host->register_library) return -6;
    memory_api = host->get_service("memory", 1, sizeof(*memory_api));
    libc_api = host->get_service("libc", 1, sizeof(*libc_api));
    threads = host->get_service("thread", 1, sizeof(*threads));
    clock_api = host->get_service("time", 1, sizeof(*clock_api));
    if (!memory_api || !libc_api || !threads || !clock_api || !memory_api->alloc || !memory_api->free ||
        !libc_api->format || !threads->worker_start || !threads->worker_join ||
        !clock_api->now_us || !clock_api->sleep_ms) return -6;
    size_t bytes = __cc_data_end - __cc_data_start;
    data_snapshot = memory_api->alloc(bytes ? bytes : 1);
    if (!data_snapshot) return -4;
    memcpy(data_snapshot, __cc_data_start, bytes);
    static const solar_os_native_library_descriptor_v1_t descriptor = {
        .abi_version = 1, .struct_size = sizeof(descriptor), .id = SOLAR_OS_CC_LIBRARY,
        .summary = "Bounded on-device C compiler prototype", .api = (const void *)&api, .shutdown = shutdown,
    };
    int code = host->register_library(&descriptor);
    if (code) shutdown();
    return code;
}
