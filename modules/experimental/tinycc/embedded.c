/* Included at the end of the pinned libtcc.c, inside its private namespace. */
#include "solar_os_cc_abi.h"
extern void solar_os_cc_checkpoint(void);
extern void *solar_os_cc_reallocate(void *, unsigned long);
extern void solar_os_cc_diagnostic(void *, const char *);

/* Serialize TinyCC's object sections to memory. The existing restricted
 * module writer then converts this object into the SolarOS ELF subset. */
int solar_os_cc_generate(const solar_os_cc_request_v1_t *request,
                          void **object, size_t *object_bytes)
{
    TCCState *s1;
    Elf32_Ehdr header = {0};
    Elf32_Shdr *table;
    unsigned char *buffer;
    uint32_t length;
    int status;
    tcc_set_realloc(solar_os_cc_reallocate);
    s1 = tcc_new();
    tcc_set_error_func(s1, NULL, solar_os_cc_diagnostic);
    tcc_set_options(s1, "-nostdinc -nostdlib -fno-common -fno-asynchronous-unwind-tables");
    tcc_define_symbol(s1, "__SOLAROS__", "1");
    tcc_define_symbol(s1, "__ESP32S3__", "1");
    tcc_undefine_symbol(s1, "__linux__"); tcc_undefine_symbol(s1, "__linux");
    tcc_undefine_symbol(s1, "linux"); tcc_undefine_symbol(s1, "__unix__");
    tcc_undefine_symbol(s1, "__unix"); tcc_undefine_symbol(s1, "unix");
    tcc_add_include_path(s1, "/sdk");
    for (uint32_t i = 0; i < request->include_count; ++i)
        tcc_add_include_path(s1, request->include_paths[i]);
    tcc_set_output_type(s1, TCC_OUTPUT_OBJ);
    status = tcc_compile_string(s1, request->source);
    if (status) { tcc_delete(s1); return SOLAR_OS_NATIVE_ERROR_FAILED; }
    static const char *const imports[] = {
        "solar_os_native_host_v1", "solar_os_native_job_host_v1", "solar_os_native_driver_host_v1",
        "solar_os_native_library_host_v1", "memcpy", "memset", "memmove", "memcmp", "strlen", "setjmp", "longjmp",
    };
    int host_import = 0;
    for (unsigned i = 1; i < s1->symtab->data_offset / sizeof(Elf32_Sym); ++i) {
        Elf32_Sym *symbol = (Elf32_Sym *)s1->symtab->data + i;
        if (symbol->st_shndx != SHN_UNDEF) continue;
        const char *name = (const char *)s1->symtab->link->data + symbol->st_name;
        unsigned n;
        for (n = 0; n < sizeof(imports)/sizeof(imports[0]); ++n) if (!strcmp(name, imports[n])) break;
        if (n == sizeof(imports)/sizeof(imports[0])) {
            char message[160]; snprintf(message, sizeof(message), "unsupported native import: %s", name);
            solar_os_cc_diagnostic(NULL, message); tcc_delete(s1); return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
        }
        if (n < 4) host_import = 1;
    }
    /* Even a standalone main needs the target's versioned ABI marker. Keep a
     * relocated data anchor when the source itself does not call a host API. */
    if (!host_import) {
        int symbol = set_elf_sym(s1->symtab, 0, 0, ELF32_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0,
                                 SHN_UNDEF, "solar_os_native_host_v1");
        size_t offset = section_add(data_section, 4, 4);
        put_elf_reloc(s1->symtab, data_section, offset, R_XTENSA_32, symbol);
    }
    solar_os_cc_checkpoint();
    alloc_sec_names(s1, 1);
    sort_syms(s1, s1->symtab);
    if (s1->nb_sections > 128) { tcc_delete(s1); return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE; }
    length = sizeof(header) + s1->nb_sections * sizeof(*table);
    for (int i = 1; i < s1->nb_sections; ++i) {
        Section *s = s1->sections[i];
        length = (length + 15) & ~15U;
        s->sh_offset = length;
        if (s->sh_type != SHT_NOBITS) {
            if (s->sh_size > SOLAR_OS_CC_OUTPUT_MAX - length) {
                tcc_delete(s1); return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            }
            length += s->sh_size;
        }
    }
    buffer = tcc_mallocz(length);
    table = (Elf32_Shdr *)(buffer + sizeof(header));
    memcpy(header.e_ident, "\177ELF\1\1\1", 7);
    header.e_type = ET_REL; header.e_machine = EM_XTENSA; header.e_version = EV_CURRENT;
    header.e_ehsize = sizeof(header); header.e_shoff = sizeof(header);
    header.e_shentsize = sizeof(*table); header.e_shnum = s1->nb_sections;
    header.e_shstrndx = s1->nb_sections - 1;
    memcpy(buffer, &header, sizeof(header));
    for (int i = 1; i < s1->nb_sections; ++i) {
        Section *s = s1->sections[i];
        table[i] = (Elf32_Shdr){
            .sh_name = s->sh_name, .sh_type = s->sh_type,
            .sh_flags = s->sh_flags, .sh_addr = s->sh_addr,
            .sh_offset = s->sh_offset, .sh_size = s->sh_size,
            .sh_link = s->link ? s->link->sh_num : 0,
            .sh_info = s->sh_info, .sh_addralign = s->sh_addralign,
            .sh_entsize = s->sh_entsize,
        };
        if (s->sh_type != SHT_NOBITS && s->sh_size)
            memcpy(buffer + s->sh_offset, s->data, s->sh_size);
    }
    tcc_delete(s1);
    *object = buffer; *object_bytes = length;
    return 0;
}
