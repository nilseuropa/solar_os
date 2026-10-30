/* Native command probe for an otherwise idle PSRAM device. Build as a PIC ELF
 * with entry app_main and run with `load /sdcard/memory-contract-probe.elf`. */
#include "solar_os_native_abi.h"
#include "solar_os_native_service_abi.h"

static const solar_os_native_host_api_v1_t *host;
static void message(const char *text)
{ size_t n = 0; while (text[n]) n++; host->write_utf8(text, n); }
static void number(size_t value)
{
    char digits[16]; size_t n = 0;
    do { digits[n++] = '0' + value % 10; value /= 10; } while (value);
    for (size_t i = 0; i < n / 2; ++i) {
        char c = digits[i]; digits[i] = digits[n - i - 1]; digits[n - i - 1] = c;
    }
    host->write_utf8(digits, n);
}
int app_main(int argc, char **argv)
{
    (void)argc; (void)argv;
    host = solar_os_native_host_v1();
    if (!host || !host->get_service) return 1;
    const solar_os_native_memory_api_v1_t *m = host->get_service("memory", 1,
        offsetof(solar_os_native_memory_api_v1_t, buffer_destroy) + sizeof(m->buffer_destroy));
    if (!m) return 2;
    enum { COUNT = 6, BYTES = 256 * 1024 };
    void *gaps[COUNT] = {0}, *large = NULL;
    solar_os_native_memory_buffer_t *handles[COUNT] = {0};
    uint8_t *leases[COUNT] = {0};
    int result = 3;
    for (size_t i = 0; i < COUNT; ++i) {
        gaps[i] = m->alloc(BYTES);
        handles[i] = m->buffer_create(BYTES);
        if (!gaps[i] || !handles[i] || !(leases[i] = m->buffer_pin(handles[i]))) goto cleanup;
        for (size_t j = 0; j < BYTES; ++j) leases[i][j] = (uint8_t)(i + j);
    }
    for (size_t i = 0; i < COUNT; ++i) { m->free(gaps[i]); gaps[i] = NULL; }
    size_t largest = m->largest_free_bytes(true), wanted = largest + BYTES;
    message("fragmented largest="); number(largest);
    message(" total="); number(m->free_bytes(true)); message(" request="); number(wanted); message("\n");
    if (wanted >= m->free_bytes(true)) goto cleanup;
    large = m->alloc(wanted);
    if (large) { result = 4; goto cleanup; } /* Pinned barriers must remain. */
    for (size_t i = 0; i < COUNT; ++i) {
        for (size_t j = 0; j < BYTES; ++j)
            if (leases[i][j] != (uint8_t)(i + j)) { result = 5; goto cleanup; }
        if (!m->buffer_unpin(handles[i])) { result = 6; goto cleanup; }
        leases[i] = NULL;
    }
    large = m->alloc(wanted); /* Shared pressure must compact and retry. */
    if (!large) { result = 7; goto cleanup; }
    for (size_t i = 0; i < COUNT; ++i) {
        leases[i] = m->buffer_pin(handles[i]);
        if (!leases[i]) { result = 8; goto cleanup; }
        for (size_t j = 0; j < BYTES; ++j)
            if (leases[i][j] != (uint8_t)(i + j)) { result = 9; goto cleanup; }
    }
    result = 0;
cleanup:
    if (large) m->free(large);
    for (size_t i = 0; i < COUNT; ++i) {
        if (leases[i]) m->buffer_unpin(handles[i]);
        if (handles[i] && !m->buffer_destroy(handles[i])) result = 10;
        if (gaps[i]) m->free(gaps[i]);
    }
    if (!result) message("MEMORY_CONTRACT_OK pinned barriers, automatic compaction, data and cleanup\n");
    else { message("MEMORY_CONTRACT_FAIL "); number(result); message("\n"); }
    return result;
}
