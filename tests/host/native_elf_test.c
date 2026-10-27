#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_native_elf.h"

enum {
    TEST_FILE_SIZE = 1024,
    TEST_SECTION_OFFSET = 52,
    TEST_SECTION_COUNT = 5,
    TEST_DYNSYM_OFFSET = 256,
    TEST_DYNSTR_OFFSET = 288,
    TEST_TEXT_OFFSET = 320,
    TEST_SHSTRTAB_OFFSET = 324,
};

static void put_u16(uint8_t *data, size_t offset, uint16_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1U] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *data, size_t offset, uint32_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1U] = (uint8_t)(value >> 8U);
    data[offset + 2U] = (uint8_t)(value >> 16U);
    data[offset + 3U] = (uint8_t)(value >> 24U);
}

static size_t section_offset(size_t index)
{
    return TEST_SECTION_OFFSET + index * 40U;
}

static void set_section(uint8_t *data,
                        size_t index,
                        uint32_t name,
                        uint32_t type,
                        uint32_t flags,
                        uint32_t address,
                        uint32_t offset,
                        uint32_t size,
                        uint32_t link,
                        uint32_t alignment,
                        uint32_t entry_size)
{
    const size_t base = section_offset(index);
    put_u32(data, base, name);
    put_u32(data, base + 4U, type);
    put_u32(data, base + 8U, flags);
    put_u32(data, base + 12U, address);
    put_u32(data, base + 16U, offset);
    put_u32(data, base + 20U, size);
    put_u32(data, base + 24U, link);
    put_u32(data, base + 32U, alignment);
    put_u32(data, base + 36U, entry_size);
}

static void make_valid_elf(uint8_t data[TEST_FILE_SIZE])
{
    static const char names[] =
        "\0.dynsym\0.dynstr\0.text\0.shstrtab\0";
    static const char symbols[] =
        "\0solar_os_native_host_v1";
    memset(data, 0, TEST_FILE_SIZE);
    data[0] = 0x7fU;
    data[1] = 'E';
    data[2] = 'L';
    data[3] = 'F';
    data[4] = 1U;
    data[5] = 1U;
    data[6] = 1U;
    put_u16(data, 16U, 3U);
    put_u16(data, 18U, SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA);
    put_u32(data, 20U, 1U);
    put_u32(data, 24U, 0x1000U);
    put_u32(data, 28U, TEST_SECTION_OFFSET);
    put_u32(data, 32U, TEST_SECTION_OFFSET);
    put_u16(data, 40U, 52U);
    put_u16(data, 42U, 0U);
    put_u16(data, 44U, 0U);
    put_u16(data, 46U, 40U);
    put_u16(data, 48U, TEST_SECTION_COUNT);
    put_u16(data, 50U, 4U);

    set_section(data, 1U, 1U, 11U, 2U, 0U,
                TEST_DYNSYM_OFFSET, 32U, 2U, 4U, 16U);
    set_section(data, 2U, 9U, 3U, 2U, 0U,
                TEST_DYNSTR_OFFSET, sizeof(symbols), 0U, 1U, 0U);
    set_section(data, 3U, 17U, 1U, 6U, 0x1000U,
                TEST_TEXT_OFFSET, 4U, 0U, 4U, 0U);
    set_section(data, 4U, 23U, 3U, 0U, 0U,
                TEST_SHSTRTAB_OFFSET, sizeof(names), 0U, 1U, 0U);
    put_u32(data, TEST_DYNSYM_OFFSET + 16U, 1U);
    data[TEST_DYNSYM_OFFSET + 28U] = 0x12U;
    memcpy(data + TEST_DYNSTR_OFFSET, symbols, sizeof(symbols));
    memcpy(data + TEST_SHSTRTAB_OFFSET, names, sizeof(names));
}

static void test_valid_elf(void)
{
    uint8_t data[TEST_FILE_SIZE];
    make_valid_elf(data);
    solar_os_native_elf_info_t info;
    char detail[96];
    assert(solar_os_native_elf_validate(data,
                                        sizeof(data),
                                        SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                        &info,
                                        detail,
                                        sizeof(detail)) == ESP_OK);
    assert(info.file_size == sizeof(data));
    assert(info.entry == 0x1000U);
    assert(info.machine == SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA);
    assert(info.section_count == TEST_SECTION_COUNT);
    assert(!info.foreground);
}

static void make_foreground_elf(uint8_t data[TEST_FILE_SIZE])
{
    static const char names[] = "\0.dynsym\0.dynstr\0.text\0.shstrtab\0.rodata\0";
    static const char strings[] = "\0solar_os_native_host_v1\0solar_os_native_app_manifest";
    make_valid_elf(data);
    put_u16(data, 48U, 6U);
    set_section(data, 1U, 1U, 11U, 2U, 0U, 768U, 48U, 2U, 4U, 16U);
    set_section(data, 2U, 9U, 3U, 2U, 0U, 832U, sizeof(strings), 0U, 1U, 0U);
    set_section(data, 3U, 17U, 1U, 6U, 0x1000U, 924U, 4U, 0U, 4U, 0U);
    set_section(data, 4U, 23U, 3U, 0U, 0U, 928U, sizeof(names), 0U, 1U, 0U);
    set_section(data, 5U, 33U, 1U, 2U, 0x2000U, 512U,
                sizeof(solar_os_native_app_manifest_v2_t), 0U, 4U, 0U);
    memset(data + 768U, 0, 48U);
    put_u32(data, 784U, 1U);
    data[796U] = 0x12U;
    put_u32(data, 800U, sizeof("\0solar_os_native_host_v1"));
    put_u32(data, 804U, 0x2000U);
    put_u32(data, 808U, sizeof(solar_os_native_app_manifest_v2_t));
    data[812U] = 0x11U;
    put_u16(data, 814U, 5U);
    memcpy(data + 832U, strings, sizeof(strings));
    memcpy(data + 928U, names, sizeof(names));
    const solar_os_native_app_manifest_v2_t app = {
        .magic = SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC,
        .lifecycle_abi = 2U, .struct_size = sizeof(app), .app_class = 3U,
        .flags = SOLAR_OS_NATIVE_APP_RESUMABLE, .worker_stack_bytes = 4096U,
        .worker_priority = 2U, .minimum_psram_bytes = 65536U, .tick_interval_ms = 5U,
        .id = "test-app", .summary = "foreground fixture",
        .file_extensions = ".gb .nib",
    };
    memcpy(data + 512U, &app, sizeof(app));
}

static void test_foreground_manifest(void)
{
    uint8_t data[TEST_FILE_SIZE];
    solar_os_native_elf_info_t info;
    make_foreground_elf(data);
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, &info, NULL, 0U) == ESP_OK);
    assert(info.foreground && !strcmp(info.app.id, "test-app"));
    assert(!strcmp(info.app.file_extensions, ".gb .nib"));
    assert(info.app.worker_stack_bytes == 4096U && info.app.worker_priority == 2U);
    put_u32(data, 528U, SOLAR_OS_NATIVE_APP_RESUMABLE | SOLAR_OS_NATIVE_APP_POINTER_EVENTS);
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, &info, NULL, 0U) == ESP_OK);
    assert(info.app.flags & SOLAR_OS_NATIVE_APP_POINTER_EVENTS);
    const char *bad_extensions[] = {"gb", ".", ".GB", ".gb/", ".gb ", ".gb  .nib"};
    for (size_t i = 0U; i < sizeof(bad_extensions) / sizeof(bad_extensions[0]); ++i) {
        make_foreground_elf(data);
        strcpy((char *)data + 680U, bad_extensions[i]);
        assert(solar_os_native_elf_validate(data, sizeof(data), 94U, NULL, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
    }
    make_foreground_elf(data); memset(data + 680U, 'x', 64U);
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, NULL, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
    make_foreground_elf(data); data[680U] = 0U;
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, &info, NULL, 0U) == ESP_OK);
    assert(info.app.file_extensions[0] == 0);
    const size_t corruptions[] = {512U, 516U, 520U, 524U, 528U, 532U, 536U, 540U, 544U, 548U};
    for (size_t i = 0U; i < sizeof(corruptions) / sizeof(corruptions[0]); ++i) {
        make_foreground_elf(data);
        put_u32(data, corruptions[i], UINT32_MAX);
        assert(solar_os_native_elf_validate(data, sizeof(data), 94U, NULL, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
    }
    make_foreground_elf(data); memset(data + 552U, 'x', 32U);
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, NULL, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
    make_foreground_elf(data); data[552U] = '/';
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, NULL, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
    make_foreground_elf(data); memset(data + 584U, 'x', 96U);
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, NULL, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
    make_foreground_elf(data); put_u32(data, 804U, 0x2001U);
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, NULL, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
    make_foreground_elf(data); put_u16(data, 814U, 0U);
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, NULL, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
    make_foreground_elf(data); put_u32(data, section_offset(5U) + 4U, 8U);
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, NULL, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
}

static void test_rejects_wrong_machine(void)
{
    uint8_t data[TEST_FILE_SIZE];
    make_valid_elf(data);
    assert(solar_os_native_elf_validate(data,
                                        sizeof(data),
                                        SOLAR_OS_NATIVE_ELF_MACHINE_RISCV,
                                        NULL,
                                        NULL,
                                        0U) == ESP_ERR_NOT_SUPPORTED);
}

static void test_rejects_truncated_section_table(void)
{
    uint8_t data[TEST_FILE_SIZE];
    make_valid_elf(data);
    put_u32(data, 32U, TEST_FILE_SIZE - 20U);
    assert(solar_os_native_elf_validate(data,
                                        sizeof(data),
                                        SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                        NULL,
                                        NULL,
                                        0U) == ESP_ERR_INVALID_RESPONSE);
}

static void test_rejects_entry_outside_text(void)
{
    uint8_t data[TEST_FILE_SIZE];
    make_valid_elf(data);
    put_u32(data, 24U, 0x2000U);
    assert(solar_os_native_elf_validate(data,
                                        sizeof(data),
                                        SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                        NULL,
                                        NULL,
                                        0U) == ESP_ERR_INVALID_RESPONSE);
}

static void test_rejects_invalid_symbol_name(void)
{
    uint8_t data[TEST_FILE_SIZE];
    make_valid_elf(data);
    put_u32(data, TEST_DYNSYM_OFFSET + 16U, UINT32_MAX);
    assert(solar_os_native_elf_validate(data,
                                        sizeof(data),
                                        SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                        NULL,
                                        NULL,
                                        0U) == ESP_ERR_INVALID_RESPONSE);
}

static void test_rejects_missing_solaros_abi_import(void)
{
    uint8_t data[TEST_FILE_SIZE];
    make_valid_elf(data);
    put_u32(data, TEST_DYNSYM_OFFSET + 16U, 0U);
    assert(solar_os_native_elf_validate(data,
                                        sizeof(data),
                                        SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
                                        NULL,
                                        NULL,
                                        0U) == ESP_ERR_INVALID_RESPONSE);
}

typedef struct {
    const uint8_t *data;
    size_t bytes, calls, fail_call;
} discovery_fixture_t;

static esp_err_t fixture_read(void *context, size_t offset, void *buffer, size_t length)
{
    discovery_fixture_t *fixture = context;
    fixture->calls++;
    if (fixture->calls == fixture->fail_call) return ESP_FAIL;
    assert(offset <= TEST_FILE_SIZE && length <= TEST_FILE_SIZE - offset);
    /* The executable section must never be read by discovery. */
    const uint8_t *text = fixture->data + section_offset(3U);
    const size_t text_offset = (size_t)text[16] | ((size_t)text[17] << 8U) |
        ((size_t)text[18] << 16U) | ((size_t)text[19] << 24U);
    assert(offset + length <= text_offset || offset >= text_offset + 4U);
    memcpy(buffer, fixture->data + offset, length);
    fixture->bytes += length;
    return ESP_OK;
}

static void test_discovery(void)
{
    uint8_t data[TEST_FILE_SIZE];
    make_foreground_elf(data);
    discovery_fixture_t fixture = {.data = data};
    solar_os_native_elf_info_t full, discovered;
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, &full, NULL, 0U) == ESP_OK);
    assert(solar_os_native_elf_discover(fixture_read, &fixture, sizeof(data), 94U,
        &discovered, NULL, 0U) == ESP_OK);
    assert(discovered.foreground && !memcmp(&discovered.app, &full.app, sizeof(full.app)));
    assert(discovered.entry == full.entry && discovered.section_count == full.section_count);
    const size_t calls = fixture.calls;
    for (size_t i = 1U; i <= calls; ++i) {
        fixture = (discovery_fixture_t){.data = data, .fail_call = i};
        assert(solar_os_native_elf_discover(fixture_read, &fixture, sizeof(data), 94U,
            &discovered, NULL, 0U) == ESP_FAIL);
        assert(!discovered.foreground);
    }
    /* Discovery is not permission to execute: unrelated symbol imports are
     * checked by the complete validator again when the app is launched. */
    data[833U] = 'x';
    fixture = (discovery_fixture_t){.data = data};
    assert(solar_os_native_elf_discover(fixture_read, &fixture, sizeof(data), 94U,
        &discovered, NULL, 0U) == ESP_OK && discovered.foreground);
    assert(solar_os_native_elf_validate(data, sizeof(data), 94U, NULL, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
    make_foreground_elf(data);
    /* Large executable payloads do not increase discovery reads or require
     * an allocation the size of the app. The fixture provides metadata only. */
    put_u32(data, section_offset(3U) + 16U, 4096U);
    put_u32(data, section_offset(3U) + 20U, 4U * 1024U * 1024U);
    fixture = (discovery_fixture_t){.data = data};
    assert(solar_os_native_elf_discover(fixture_read, &fixture, 8U * 1024U * 1024U,
        94U, &discovered, NULL, 0U) == ESP_OK);
    assert(fixture.bytes < 2048U && discovered.foreground);
    assert(solar_os_native_elf_discover(fixture_read, &fixture, sizeof(data), 243U,
        &discovered, NULL, 0U) == ESP_ERR_NOT_SUPPORTED);
    const size_t corruptions[] = {32U, section_offset(1U) + 16U, section_offset(1U) + 20U,
        section_offset(1U) + 24U, section_offset(2U) + 16U, section_offset(4U) + 16U,
        section_offset(5U) + 16U, 800U, 804U, 808U,
        512U, 516U, 520U, 524U, 528U, 532U, 536U, 540U, 544U, 548U};
    for (size_t i = 0; i < sizeof(corruptions) / sizeof(corruptions[0]); ++i) {
        make_foreground_elf(data); put_u32(data, corruptions[i], UINT32_MAX);
        fixture = (discovery_fixture_t){.data = data};
        assert(solar_os_native_elf_discover(fixture_read, &fixture, sizeof(data), 94U,
            &discovered, NULL, 0U) == ESP_ERR_INVALID_RESPONSE);
    }
    make_foreground_elf(data);
    put_u32(data, section_offset(1U) + 20U, 64U);
    memcpy(data + 816U, data + 800U, 16U);
    fixture = (discovery_fixture_t){.data = data};
    assert(solar_os_native_elf_discover(fixture_read, &fixture, sizeof(data), 94U,
        &discovered, NULL, 0U) == ESP_ERR_INVALID_RESPONSE); /* Duplicate manifest. */
    make_valid_elf(data); fixture = (discovery_fixture_t){.data = data};
    assert(solar_os_native_elf_discover(fixture_read, &fixture, sizeof(data), 94U,
        &discovered, NULL, 0U) == ESP_OK && !discovered.foreground);
}

typedef struct { FILE *file; size_t bytes; } file_reader_t;

static esp_err_t file_read(void *context, size_t offset, void *buffer, size_t length)
{
    file_reader_t *reader = context;
    if (fseek(reader->file, (long)offset, SEEK_SET) ||
        fread(buffer, 1U, length, reader->file) != length) return ESP_FAIL;
    reader->bytes += length;
    return ESP_OK;
}

static void test_real_elf_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0L, SEEK_END) == 0);
    const long file_size = ftell(file);
    assert(file_size > 0L);
    assert(fseek(file, 0L, SEEK_SET) == 0);

    uint8_t *data = malloc((size_t)file_size);
    assert(data != NULL);
    assert(fread(data, 1U, (size_t)file_size, file) == (size_t)file_size);
    char detail[128];
    solar_os_native_elf_info_t full, discovered;
    const esp_err_t err = solar_os_native_elf_validate(
        data,
        (size_t)file_size,
        SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,
        &full,
        detail,
        sizeof(detail));
    if (err != ESP_OK) {
        fprintf(stderr, "%s: %s\n", path, detail);
    }
    assert(err == ESP_OK);
    file_reader_t reader = {.file = file};
    assert(solar_os_native_elf_discover(file_read, &reader, (size_t)file_size, 94U,
        &discovered, detail, sizeof(detail)) == ESP_OK);
    assert(discovered.foreground == full.foreground);
    assert(!memcmp(&discovered.app, &full.app, sizeof(full.app)));
    printf("%s: discovery %zu bytes / %ld-byte ELF\n", path, reader.bytes, file_size);
    assert(fclose(file) == 0);
    free(data);
}

int main(int argc, char **argv)
{
    assert(argc == 1 || argc == 2);
    test_valid_elf();
    test_foreground_manifest();
    test_discovery();
    test_rejects_wrong_machine();
    test_rejects_truncated_section_table();
    test_rejects_entry_outside_text();
    test_rejects_invalid_symbol_name();
    test_rejects_missing_solaros_abi_import();
    if (argc == 2) {
        test_real_elf_file(argv[1]);
    }
    puts("native ELF validation tests passed");
    return 0;
}
