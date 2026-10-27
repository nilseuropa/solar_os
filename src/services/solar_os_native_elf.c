#include "solar_os_native_elf.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
    ELF_HEADER_SIZE = 52,
    ELF_PROGRAM_HEADER_SIZE = 32,
    ELF_SECTION_HEADER_SIZE = 40,
    ELF_SYMBOL_SIZE = 16,
    ELF_REL_SIZE = 8,
    ELF_RELA_SIZE = 12,
    ELF_TYPE_DYNAMIC = 3,
    ELF_SECTION_PROGBITS = 1,
    ELF_SECTION_SYMTAB = 2,
    ELF_SECTION_STRTAB = 3,
    ELF_SECTION_RELA = 4,
    ELF_SECTION_NOBITS = 8,
    ELF_SECTION_REL = 9,
    ELF_SECTION_DYNSYM = 11,
    ELF_SECTION_FLAG_ALLOC = 2,
    ELF_SECTION_FLAG_EXEC = 4,
    ELF_SYMBOL_BIND_GLOBAL = 1,
    ELF_SYMBOL_BIND_WEAK = 2,
    ELF_SYMBOL_TYPE_NOTYPE = 0,
    ELF_SYMBOL_TYPE_FUNC = 2,
    ELF_SECTION_INDEX_UNDEFINED = 0,
};

static const char *const native_host_symbols[] = {
    "solar_os_native_host_v1",
    "solar_os_native_job_host_v1",
    "solar_os_native_driver_host_v1",
    "solar_os_native_library_host_v1",
};

typedef struct {
    uint32_t name;
    uint32_t type;
    uint32_t flags;
    uint32_t address;
    uint32_t offset;
    uint32_t size;
    uint32_t link;
    uint32_t info;
    uint32_t alignment;
    uint32_t entry_size;
} native_section_t;

static uint16_t read_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8U);
}

static uint32_t read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8U) |
        ((uint32_t)data[2] << 16U) |
        ((uint32_t)data[3] << 24U);
}

static void set_detail(char *detail, size_t detail_len, const char *text)
{
    if (detail != NULL && detail_len > 0U) {
        snprintf(detail, detail_len, "%s", text != NULL ? text : "invalid ELF file");
    }
}

static bool range_valid(size_t offset, size_t length, size_t total)
{
    return offset <= total && length <= total - offset;
}

static bool table_valid(uint32_t offset,
                        uint16_t count,
                        uint16_t entry_size,
                        size_t total)
{
    if (count == 0U) {
        return (size_t)offset <= total;
    }
    if (entry_size == 0U) {
        return false;
    }
    if ((size_t)count > SIZE_MAX / (size_t)entry_size) {
        return false;
    }
    return range_valid((size_t)offset, (size_t)count * (size_t)entry_size, total);
}

static bool power_of_two_or_zero(uint32_t value)
{
    return value == 0U || (value & (value - 1U)) == 0U;
}

static native_section_t decode_section(const uint8_t *section)
{
    return (native_section_t){
        .name = read_u32(section),
        .type = read_u32(section + 4U),
        .flags = read_u32(section + 8U),
        .address = read_u32(section + 12U),
        .offset = read_u32(section + 16U),
        .size = read_u32(section + 20U),
        .link = read_u32(section + 24U),
        .info = read_u32(section + 28U),
        .alignment = read_u32(section + 32U),
        .entry_size = read_u32(section + 36U),
    };
}

static native_section_t read_section(const uint8_t *data,
                                     uint32_t section_offset,
                                     uint16_t index)
{
    return decode_section(data + section_offset + (size_t)index * ELF_SECTION_HEADER_SIZE);
}

static bool section_name_valid(const uint8_t *names,
                               size_t names_len,
                               uint32_t name_offset)
{
    return name_offset < names_len &&
        memchr(names + name_offset, '\0', names_len - name_offset) != NULL;
}

static bool section_is_symbol_table(uint32_t type)
{
    return type == ELF_SECTION_SYMTAB || type == ELF_SECTION_DYNSYM;
}

static bool app_manifest_valid(const solar_os_native_app_manifest_v2_t *app)
{
    if (app->magic != SOLAR_OS_NATIVE_APP_MANIFEST_MAGIC ||
        app->lifecycle_abi != SOLAR_OS_NATIVE_APP_LIFECYCLE_ABI ||
        app->struct_size != sizeof(*app) ||
        (app->app_class != SOLAR_OS_NATIVE_APP_CLASS_TUI &&
         app->app_class != SOLAR_OS_NATIVE_APP_CLASS_GUI) ||
        (app->flags & ~(SOLAR_OS_NATIVE_APP_RESUMABLE | SOLAR_OS_NATIVE_APP_POINTER_EVENTS)) != 0U ||
        app->worker_stack_bytes < 2048U || app->worker_stack_bytes > 65536U ||
        app->worker_priority < 1U || app->worker_priority > 4U ||
        app->minimum_internal_bytes > 512U * 1024U ||
        app->minimum_psram_bytes > 16U * 1024U * 1024U ||
        app->tick_interval_ms == 0U || app->tick_interval_ms > 1000U ||
        app->id[0] == '\0' || memchr(app->id, '\0', sizeof(app->id)) == NULL ||
        app->summary[0] == '\0' ||
        memchr(app->summary, '\0', sizeof(app->summary)) == NULL ||
        memchr(app->file_extensions, '\0', sizeof(app->file_extensions)) == NULL) {
        return false;
    }
    for (const char *p = app->id; *p != '\0'; ++p) {
        if (!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9') &&
            *p != '-' && *p != '_') return false;
    }
    const char *extension = app->file_extensions;
    while (*extension) {
        if (*extension++ != '.') return false;
        size_t length = 0U;
        while (*extension && *extension != ' ') {
            if (!(*extension >= 'a' && *extension <= 'z') &&
                !(*extension >= '0' && *extension <= '9')) return false;
            extension++;
            length++;
        }
        if (!length) return false;
        if (*extension == ' ') {
            extension++;
            if (!*extension) return false;
        }
    }
    return true;
}

static void decode_app_manifest(const uint8_t *record,
                                 solar_os_native_app_manifest_v2_t *app)
{
    *app = (solar_os_native_app_manifest_v2_t){
        .magic = read_u32(record), .lifecycle_abi = read_u32(record + 4U),
        .struct_size = read_u32(record + 8U), .app_class = read_u32(record + 12U),
        .flags = read_u32(record + 16U), .worker_stack_bytes = read_u32(record + 20U),
        .worker_priority = read_u32(record + 24U),
        .minimum_internal_bytes = read_u32(record + 28U),
        .minimum_psram_bytes = read_u32(record + 32U),
        .tick_interval_ms = read_u32(record + 36U),
    };
    memcpy(app->id, record + 40U, sizeof(app->id));
    memcpy(app->summary, record + 72U, sizeof(app->summary));
    memcpy(app->file_extensions, record + 168U, sizeof(app->file_extensions));
}

/* All section, symbol and string bounds have already passed ELF validation.
 * Metadata must be a defined object fully contained in file-backed data;
 * a similarly named import, BSS object, or pointer cannot describe an app. */
static esp_err_t read_app_manifest(const uint8_t *data, size_t data_len,
                                   uint32_t section_offset, uint16_t section_count,
                                   solar_os_native_app_manifest_v2_t *app)
{
    bool found = false;
    for (uint16_t i = 0U; i < section_count; ++i) {
        const native_section_t symbols = read_section(data, section_offset, i);
        if (symbols.type != ELF_SECTION_DYNSYM) continue;
        const native_section_t strings =
            read_section(data, section_offset, (uint16_t)symbols.link);
        for (uint32_t n = 0U; n < symbols.size / ELF_SYMBOL_SIZE; ++n) {
            const uint8_t *symbol = data + symbols.offset + n * ELF_SYMBOL_SIZE;
            const char *name = (const char *)data + strings.offset + read_u32(symbol);
            if (strcmp(name, SOLAR_OS_NATIVE_APP_MANIFEST_SYMBOL) != 0) continue;
            const uint16_t index = read_u16(symbol + 14U);
            if (found || symbol[12U] != 0x11U || index == 0U ||
                index >= section_count || read_u32(symbol + 8U) != sizeof(*app))
                return ESP_ERR_INVALID_RESPONSE;
            const native_section_t section = read_section(data, section_offset, index);
            const uint32_t address = read_u32(symbol + 4U);
            if (section.type != ELF_SECTION_PROGBITS ||
                (section.flags & ELF_SECTION_FLAG_ALLOC) == 0U ||
                (section.flags & ELF_SECTION_FLAG_EXEC) != 0U || address < section.address ||
                !range_valid(address - section.address, sizeof(*app), section.size) ||
                !range_valid(section.offset, section.size, data_len))
                return ESP_ERR_INVALID_RESPONSE;
            const uint8_t *record = data + section.offset + address - section.address;
            decode_app_manifest(record, app);
            if (!app_manifest_valid(app)) return ESP_ERR_INVALID_RESPONSE;
            found = true;
        }
    }
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

typedef struct {
    solar_os_native_elf_read_fn read;
    void *context;
    size_t offset, length;
    uint8_t data[512];
} native_metadata_reader_t;

/* Two independent windows keep sequential symbols cached while looking up
 * their names. Never prefetch beyond the metadata region being inspected. */
static esp_err_t metadata_read(native_metadata_reader_t *reader, size_t offset,
                                void *buffer, size_t length, size_t limit)
{
    if (!range_valid(offset, length, limit)) return ESP_ERR_INVALID_RESPONSE;
    uint8_t *out = buffer;
    while (length) {
        if (offset < reader->offset || offset - reader->offset >= reader->length) {
            reader->length = 0U;
            size_t count = limit - offset;
            if (count > sizeof(reader->data)) count = sizeof(reader->data);
            esp_err_t err = reader->read(reader->context, offset, reader->data, count);
            if (err != ESP_OK) return err;
            reader->offset = offset;
            reader->length = count;
        }
        size_t count = reader->length - (offset - reader->offset);
        if (count > length) count = length;
        memcpy(out, reader->data + offset - reader->offset, count);
        offset += count; out += count; length -= count;
    }
    return ESP_OK;
}

static esp_err_t metadata_section(native_metadata_reader_t *reader,
                                   uint32_t offset, uint16_t count, uint16_t index,
                                   native_section_t *section)
{
    uint8_t data[ELF_SECTION_HEADER_SIZE];
    if (index >= count) return ESP_ERR_INVALID_RESPONSE;
    esp_err_t err = metadata_read(reader, (size_t)offset + (size_t)index * sizeof(data),
        data, sizeof(data), (size_t)offset + (size_t)count * sizeof(data));
    if (err == ESP_OK) *section = decode_section(data);
    return err;
}

esp_err_t solar_os_native_elf_discover(solar_os_native_elf_read_fn read,
                                       void *context, size_t file_size,
                                       uint16_t expected_machine,
                                       solar_os_native_elf_info_t *info,
                                       char *detail, size_t detail_len)
{
    if (detail && detail_len) detail[0] = '\0';
    if (info) memset(info, 0, sizeof(*info));
    if (!read || !info || !expected_machine) return ESP_ERR_INVALID_ARG;
    if (file_size < ELF_HEADER_SIZE) return ESP_ERR_INVALID_SIZE;
    uint8_t header[ELF_HEADER_SIZE];
    esp_err_t err = read(context, 0U, header, sizeof(header));
    if (err != ESP_OK) return err;
    if (memcmp(header, "\177ELF", 4U)) return ESP_ERR_INVALID_RESPONSE;
    if (header[4] != 1U || header[5] != 1U || header[6] != 1U ||
        read_u16(header + 16U) != ELF_TYPE_DYNAMIC ||
        read_u16(header + 18U) != expected_machine) return ESP_ERR_NOT_SUPPORTED;
    const uint32_t entry = read_u32(header + 24U);
    const uint32_t program_offset = read_u32(header + 28U);
    const uint32_t section_offset = read_u32(header + 32U);
    const uint16_t program_size = read_u16(header + 42U);
    const uint16_t program_count = read_u16(header + 44U);
    const uint16_t section_count = read_u16(header + 48U);
    const uint16_t names_index = read_u16(header + 50U);
    if (read_u32(header + 20U) != 1U || read_u16(header + 40U) != ELF_HEADER_SIZE || !entry ||
        (program_count && program_size != ELF_PROGRAM_HEADER_SIZE) ||
        !table_valid(program_offset, program_count, program_size, file_size) ||
        !section_count || read_u16(header + 46U) != ELF_SECTION_HEADER_SIZE ||
        names_index >= section_count ||
        !table_valid(section_offset, section_count, ELF_SECTION_HEADER_SIZE, file_size)) {
        set_detail(detail, detail_len, "invalid discovery header bounds");
        return ESP_ERR_INVALID_RESPONSE;
    }
    native_metadata_reader_t metadata = {.read = read, .context = context};
    native_metadata_reader_t symbols = {.read = read, .context = context};
    native_section_t names;
    err = metadata_section(&metadata, section_offset, section_count, names_index, &names);
    if (err != ESP_OK) goto failed;
    if (names.type != ELF_SECTION_STRTAB || !names.size ||
        !range_valid(names.offset, names.size, file_size)) {
        err = ESP_ERR_INVALID_RESPONSE; goto failed;
    }
    bool found = false, entry_in_text = false, dynamic_symbols = false;
    solar_os_native_app_manifest_v2_t app = {0};
    for (uint16_t i = 0U; i < section_count; ++i) {
        native_section_t section;
        err = metadata_section(&metadata, section_offset, section_count, i, &section);
        if (err != ESP_OK) goto failed;
        if (!power_of_two_or_zero(section.alignment) ||
            section.address > UINT32_MAX - section.size ||
            (section.type != ELF_SECTION_NOBITS &&
             !range_valid(section.offset, section.size, file_size))) {
            err = ESP_ERR_INVALID_RESPONSE; goto failed;
        }
        if (section.type == ELF_SECTION_PROGBITS &&
            (section.flags & (ELF_SECTION_FLAG_ALLOC | ELF_SECTION_FLAG_EXEC)) ==
                (ELF_SECTION_FLAG_ALLOC | ELF_SECTION_FLAG_EXEC) &&
            entry >= section.address && entry - section.address < section.size)
            entry_in_text = true;
        if (section.type != ELF_SECTION_DYNSYM) continue;
        dynamic_symbols = true;
        native_section_t strings;
        err = metadata_section(&metadata, section_offset, section_count,
            section.link < section_count ? (uint16_t)section.link : section_count, &strings);
        if (err != ESP_OK) goto failed;
        if (section.entry_size != ELF_SYMBOL_SIZE || section.size % ELF_SYMBOL_SIZE ||
            strings.type != ELF_SECTION_STRTAB || !strings.size ||
            !range_valid(strings.offset, strings.size, file_size)) {
            err = ESP_ERR_INVALID_RESPONSE; goto failed;
        }
        const size_t symbol_end = (size_t)section.offset + section.size;
        const size_t string_end = (size_t)strings.offset + strings.size;
        for (size_t n = 0U; n < section.size / ELF_SYMBOL_SIZE; ++n) {
            uint8_t symbol[ELF_SYMBOL_SIZE];
            err = metadata_read(&symbols, (size_t)section.offset + n * sizeof(symbol),
                symbol, sizeof(symbol), symbol_end);
            if (err != ESP_OK) goto failed;
            const uint32_t name_offset = read_u32(symbol);
            if (name_offset >= strings.size) { err = ESP_ERR_INVALID_RESPONSE; goto failed; }
            if (strings.size - name_offset < sizeof(SOLAR_OS_NATIVE_APP_MANIFEST_SYMBOL)) continue;
            char name[sizeof(SOLAR_OS_NATIVE_APP_MANIFEST_SYMBOL)];
            err = metadata_read(&metadata, (size_t)strings.offset + name_offset,
                name, sizeof(name), string_end);
            if (err != ESP_OK) goto failed;
            if (memcmp(name, SOLAR_OS_NATIVE_APP_MANIFEST_SYMBOL, sizeof(name))) continue;
            const uint16_t index = read_u16(symbol + 14U);
            if (found || symbol[12U] != 0x11U || !index || index >= section_count ||
                read_u32(symbol + 8U) != sizeof(app)) {
                err = ESP_ERR_INVALID_RESPONSE; goto failed;
            }
            native_section_t record_section;
            err = metadata_section(&metadata, section_offset, section_count, index, &record_section);
            if (err != ESP_OK) goto failed;
            const uint32_t address = read_u32(symbol + 4U);
            if (record_section.type != ELF_SECTION_PROGBITS ||
                !(record_section.flags & ELF_SECTION_FLAG_ALLOC) ||
                (record_section.flags & ELF_SECTION_FLAG_EXEC) || address < record_section.address ||
                !range_valid(address - record_section.address, sizeof(app), record_section.size) ||
                !range_valid(record_section.offset, record_section.size, file_size)) {
                err = ESP_ERR_INVALID_RESPONSE; goto failed;
            }
            uint8_t record[sizeof(app)];
            err = read(context, (size_t)record_section.offset + (address - record_section.address),
                record, sizeof(record));
            if (err != ESP_OK) goto failed;
            decode_app_manifest(record, &app);
            if (!app_manifest_valid(&app)) { err = ESP_ERR_INVALID_RESPONSE; goto failed; }
            found = true;
        }
    }
    if (!entry_in_text || !dynamic_symbols) { err = ESP_ERR_INVALID_RESPONSE; goto failed; }
    *info = (solar_os_native_elf_info_t){
        .file_size = file_size, .entry = entry, .machine = expected_machine,
        .program_count = program_count, .section_count = section_count,
        .foreground = found, .app = app,
    };
    return ESP_OK;
failed:
    set_detail(detail, detail_len, "could not read valid foreground discovery metadata");
    return err;
}

static esp_err_t validate_program_headers(const uint8_t *data,
                                          size_t data_len,
                                          uint32_t offset,
                                          uint16_t count,
                                          char *detail,
                                          size_t detail_len)
{
    for (uint16_t i = 0U; i < count; i++) {
        const uint8_t *program = data + offset +
            (size_t)i * ELF_PROGRAM_HEADER_SIZE;
        const uint32_t file_offset = read_u32(program + 4U);
        const uint32_t virtual_address = read_u32(program + 8U);
        const uint32_t file_size = read_u32(program + 16U);
        const uint32_t memory_size = read_u32(program + 20U);
        const uint32_t alignment = read_u32(program + 28U);

        if (file_size > memory_size ||
            !range_valid(file_offset, file_size, data_len) ||
            virtual_address > UINT32_MAX - memory_size ||
            !power_of_two_or_zero(alignment)) {
            set_detail(detail, detail_len, "invalid program header bounds");
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (alignment > 1U &&
            (file_offset & (alignment - 1U)) !=
                (virtual_address & (alignment - 1U))) {
            set_detail(detail, detail_len, "misaligned program header");
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

static esp_err_t validate_symbol_names(const uint8_t *data,
                                       const native_section_t *symbols,
                                       const native_section_t *strings,
                                       bool *host_import_found,
                                       char *detail,
                                       size_t detail_len)
{
    const uint32_t count = symbols->size / ELF_SYMBOL_SIZE;
    const uint8_t *string_data = data + strings->offset;
    for (uint32_t i = 0U; i < count; i++) {
        const uint8_t *symbol = data + symbols->offset +
            (size_t)i * ELF_SYMBOL_SIZE;
        const uint32_t name = read_u32(symbol);
        if (!section_name_valid(string_data, strings->size, name)) {
            set_detail(detail, detail_len, "symbol name escapes its string table");
            return ESP_ERR_INVALID_RESPONSE;
        }
        bool known_host_symbol = false;
        for (size_t host = 0U;
             host < sizeof(native_host_symbols) / sizeof(native_host_symbols[0]);
             host++) {
            if (strcmp((const char *)string_data + name,
                       native_host_symbols[host]) == 0) {
                known_host_symbol = true;
                break;
            }
        }
        if (host_import_found != NULL && known_host_symbol) {
            const uint8_t symbol_info = symbol[12U];
            const uint8_t binding = symbol_info >> 4U;
            const uint8_t type = symbol_info & 0x0fU;
            const uint16_t section_index = read_u16(symbol + 14U);
            *host_import_found |=
                section_index == ELF_SECTION_INDEX_UNDEFINED &&
                (binding == ELF_SYMBOL_BIND_GLOBAL || binding == ELF_SYMBOL_BIND_WEAK) &&
                (type == ELF_SYMBOL_TYPE_NOTYPE || type == ELF_SYMBOL_TYPE_FUNC);
        }
    }
    return ESP_OK;
}

static esp_err_t validate_relocations(const uint8_t *data,
                                      const native_section_t *relocations,
                                      const native_section_t *symbols,
                                      char *detail,
                                      size_t detail_len)
{
    const uint32_t entry_size = relocations->type == ELF_SECTION_RELA ?
        ELF_RELA_SIZE : ELF_REL_SIZE;
    const uint32_t relocation_count = relocations->size / entry_size;
    const uint32_t symbol_count = symbols->size / ELF_SYMBOL_SIZE;

    for (uint32_t i = 0U; i < relocation_count; i++) {
        const uint8_t *relocation = data + relocations->offset +
            (size_t)i * entry_size;
        const uint32_t symbol_index = read_u32(relocation + 4U) >> 8U;
        if (symbol_index >= symbol_count) {
            set_detail(detail, detail_len, "relocation references an invalid symbol");
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_native_elf_validate(const uint8_t *data,
                                       size_t data_len,
                                       uint16_t expected_machine,
                                       solar_os_native_elf_info_t *info,
                                       char *detail,
                                       size_t detail_len)
{
    if (detail != NULL && detail_len > 0U) {
        detail[0] = '\0';
    }
    if (data == NULL || expected_machine == 0U) {
        set_detail(detail, detail_len, "missing ELF data or target architecture");
        return ESP_ERR_INVALID_ARG;
    }
    if (data_len < ELF_HEADER_SIZE) {
        set_detail(detail, detail_len, "file is smaller than an ELF header");
        return ESP_ERR_INVALID_SIZE;
    }
    if (data[0] != 0x7fU || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
        set_detail(detail, detail_len, "ELF magic is missing");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (data[4] != 1U || data[5] != 1U || data[6] != 1U) {
        set_detail(detail, detail_len, "only ELF32 little-endian version 1 is supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint16_t type = read_u16(data + 16U);
    const uint16_t machine = read_u16(data + 18U);
    const uint32_t version = read_u32(data + 20U);
    const uint32_t entry = read_u32(data + 24U);
    const uint32_t program_offset = read_u32(data + 28U);
    const uint32_t section_offset = read_u32(data + 32U);
    const uint16_t header_size = read_u16(data + 40U);
    const uint16_t program_entry_size = read_u16(data + 42U);
    const uint16_t program_count = read_u16(data + 44U);
    const uint16_t section_entry_size = read_u16(data + 46U);
    const uint16_t section_count = read_u16(data + 48U);
    const uint16_t names_index = read_u16(data + 50U);

    if (type != ELF_TYPE_DYNAMIC) {
        set_detail(detail, detail_len, "ELF must be a position-independent dynamic object");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (machine != expected_machine) {
        set_detail(detail, detail_len, "ELF architecture does not match this firmware");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (version != 1U || header_size != ELF_HEADER_SIZE || entry == 0U) {
        set_detail(detail, detail_len, "invalid ELF header version, size, or entry point");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if ((program_count > 0U && program_entry_size != ELF_PROGRAM_HEADER_SIZE) ||
        !table_valid(program_offset, program_count, program_entry_size, data_len)) {
        set_detail(detail, detail_len, "program header table escapes the file");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (section_count == 0U || section_entry_size != ELF_SECTION_HEADER_SIZE ||
        names_index >= section_count ||
        !table_valid(section_offset, section_count, section_entry_size, data_len)) {
        set_detail(detail, detail_len, "section header table escapes the file");
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = validate_program_headers(data,
                                             data_len,
                                             program_offset,
                                             program_count,
                                             detail,
                                             detail_len);
    if (err != ESP_OK) {
        return err;
    }

    const native_section_t names = read_section(data, section_offset, names_index);
    if (names.type != ELF_SECTION_STRTAB || names.size == 0U ||
        !range_valid(names.offset, names.size, data_len)) {
        set_detail(detail, detail_len, "invalid section-name string table");
        return ESP_ERR_INVALID_RESPONSE;
    }
    const uint8_t *name_data = data + names.offset;
    bool text_found = false;
    bool entry_in_text = false;
    bool dynamic_symbols_found = false;
    bool host_import_found = false;

    for (uint16_t i = 0U; i < section_count; i++) {
        const native_section_t section = read_section(data, section_offset, i);
        if (!section_name_valid(name_data, names.size, section.name) ||
            !power_of_two_or_zero(section.alignment) ||
            (section.type != ELF_SECTION_NOBITS &&
             !range_valid(section.offset, section.size, data_len))) {
            set_detail(detail, detail_len, "invalid section bounds or name");
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (section.address > UINT32_MAX - section.size) {
            set_detail(detail, detail_len, "section address overflows");
            return ESP_ERR_INVALID_RESPONSE;
        }

        const char *name = (const char *)name_data + section.name;
        if (strcmp(name, ".text") == 0) {
            text_found = section.type == ELF_SECTION_PROGBITS &&
                section.size > 0U &&
                (section.flags & (ELF_SECTION_FLAG_ALLOC | ELF_SECTION_FLAG_EXEC)) ==
                    (ELF_SECTION_FLAG_ALLOC | ELF_SECTION_FLAG_EXEC);
            entry_in_text = text_found && entry >= section.address &&
                entry < section.address + section.size;
        }

        if (section_is_symbol_table(section.type)) {
            if (section.entry_size != ELF_SYMBOL_SIZE ||
                section.size % ELF_SYMBOL_SIZE != 0U ||
                section.link >= section_count) {
                set_detail(detail, detail_len, "invalid symbol table");
                return ESP_ERR_INVALID_RESPONSE;
            }
            const native_section_t strings =
                read_section(data, section_offset, (uint16_t)section.link);
            if (strings.type != ELF_SECTION_STRTAB || strings.size == 0U ||
                !range_valid(strings.offset, strings.size, data_len)) {
                set_detail(detail, detail_len, "symbol table has no valid string table");
                return ESP_ERR_INVALID_RESPONSE;
            }
            err = validate_symbol_names(data,
                                        &section,
                                        &strings,
                                        section.type == ELF_SECTION_DYNSYM ?
                                            &host_import_found : NULL,
                                        detail,
                                        detail_len);
            if (err != ESP_OK) {
                return err;
            }
            dynamic_symbols_found |= section.type == ELF_SECTION_DYNSYM;
        }

        if (section.type == ELF_SECTION_REL || section.type == ELF_SECTION_RELA) {
            const uint32_t expected_size = section.type == ELF_SECTION_RELA ?
                ELF_RELA_SIZE : ELF_REL_SIZE;
            if (section.entry_size != expected_size ||
                section.size % expected_size != 0U ||
                section.link >= section_count) {
                set_detail(detail, detail_len, "invalid relocation table");
                return ESP_ERR_INVALID_RESPONSE;
            }
            const native_section_t symbols =
                read_section(data, section_offset, (uint16_t)section.link);
            if (!section_is_symbol_table(symbols.type) ||
                symbols.entry_size != ELF_SYMBOL_SIZE ||
                symbols.size % ELF_SYMBOL_SIZE != 0U) {
                set_detail(detail, detail_len, "relocation table has no valid symbol table");
                return ESP_ERR_INVALID_RESPONSE;
            }
            err = validate_relocations(data,
                                       &section,
                                       &symbols,
                                       detail,
                                       detail_len);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    if (!text_found || !entry_in_text || !dynamic_symbols_found ||
        !host_import_found) {
        set_detail(detail,
                   detail_len,
                   "ELF needs executable .text, an entry within it, .dynsym, and a SolarOS ABI v1 import");
        return ESP_ERR_INVALID_RESPONSE;
    }

    solar_os_native_app_manifest_v2_t app = {0};
    const esp_err_t app_err = read_app_manifest(data, data_len, section_offset,
                                               section_count, &app);
    if (app_err != ESP_OK && app_err != ESP_ERR_NOT_FOUND) {
        set_detail(detail, detail_len, "invalid foreground application manifest");
        return app_err;
    }
    if (info != NULL) {
        *info = (solar_os_native_elf_info_t){
            .file_size = data_len,
            .entry = entry,
            .machine = machine,
            .program_count = program_count,
            .section_count = section_count,
            .foreground = app_err == ESP_OK,
            .app = app,
        };
    }
    return ESP_OK;
}

const char *solar_os_native_elf_machine_name(uint16_t machine)
{
    switch (machine) {
    case SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA:
        return "Xtensa";
    case SOLAR_OS_NATIVE_ELF_MACHINE_RISCV:
        return "RISC-V";
    default:
        return "unknown";
    }
}
