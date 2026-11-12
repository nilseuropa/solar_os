/* SPDX-License-Identifier: LGPL-2.0-or-later
 * Restricted ELF writer for this backend. No GNU assembler or linker needed.
 * TinyCC's portable elf.h supplies the on-disk ELF32 record definitions.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elf.h"

#define LIMIT (2U * 1024U * 1024U)
#define NSECTIONS 9
enum { TEXT = 1, RODATA, DATA, BSS, DYNSYM, DYNSTR, RELA, SHSTR };
static const char names[] = "\0.text\0.rodata\0.data\0.bss\0.dynsym\0.dynstr\0.rela.dyn\0.shstrtab\0";
static const uint32_t name_offsets[] = {0, 1, 7, 15, 21, 26, 34, 42, 52};

static int bounds(size_t at, size_t bytes, size_t total)
{ return at <= total && bytes <= total - at; }
static uint32_t align16(uint32_t value) { return (value + 15) & ~15U; }

/* The caller owns *output on success. Input must be an aligned ELF32 buffer.
 * Bounded sizes deliberately exclude general ELF linking and huge C objects. */
int solar_tcc_module(const void *input, size_t bytes, const char *entry,
                     void **output, size_t *output_bytes, const char **error)
{
    const unsigned char *in = input;
    const Elf32_Ehdr *eh;
    const Elf32_Shdr *sh, *symbols = NULL, *strings;
    const Elf32_Sym *syms;
    Elf32_Shdr out[NSECTIONS] = {{0}};
    Elf32_Ehdr header = {0};
    uint16_t *groups = NULL;
    uint32_t *offsets = NULL, symcount, reloc_count = 0, reloc_at = 0;
    uint32_t cursor = 0x100, file_cursor, entry_address = 0, sym_section = 0;
    unsigned char *image = NULL;
    const uint16_t endian = 1;
    int status = -1;
#define REQUIRE(test, why) do { if (!(test)) { *error = why; goto failed; } } while (0)
    *output = NULL; *output_bytes = 0; *error = NULL;
    REQUIRE(*(const unsigned char *)&endian == 1, "writer requires little endian host");
    REQUIRE(bytes >= sizeof(*eh) && bytes <= LIMIT && !((uintptr_t)input & 3), "ELF input size or alignment");
    eh = (const Elf32_Ehdr *)in;
    REQUIRE(!memcmp(eh->e_ident, "\177ELF\1\1\1", 7) && eh->e_type == ET_REL &&
            eh->e_machine == EM_XTENSA && eh->e_ehsize == sizeof(*eh), "expected Xtensa ELF32 object");
    REQUIRE(eh->e_shnum && eh->e_shnum <= 256 && eh->e_shentsize == sizeof(*sh) &&
            !(eh->e_shoff & 3) && bounds(eh->e_shoff, eh->e_shnum * sizeof(*sh), bytes), "section table bounds");
    sh = (const Elf32_Shdr *)(in + eh->e_shoff);
    REQUIRE(eh->e_shstrndx < eh->e_shnum, "section names index");
    strings = sh + eh->e_shstrndx;
    REQUIRE(strings->sh_type == SHT_STRTAB && bounds(strings->sh_offset, strings->sh_size, bytes), "section name bounds");
    groups = calloc(eh->e_shnum, sizeof(*groups));
    offsets = calloc(eh->e_shnum, sizeof(*offsets));
    REQUIRE(groups && offsets, "out of memory");
    for (unsigned i = 1; i < eh->e_shnum; i++) {
        const Elf32_Shdr *s = sh + i;
        const char *name;
        unsigned group = 0;
        REQUIRE(s->sh_size <= LIMIT && (s->sh_type == SHT_NOBITS || bounds(s->sh_offset, s->sh_size, bytes)), "section contents bounds");
        REQUIRE(s->sh_name < strings->sh_size && memchr(in + strings->sh_offset + s->sh_name, 0,
                strings->sh_size - s->sh_name), "section name bounds");
        name = (const char *)in + strings->sh_offset + s->sh_name;
        if (s->sh_type == SHT_SYMTAB) {
            REQUIRE(!symbols, "multiple symbol tables");
            symbols = s; sym_section = i;
        }
        if (!(s->sh_flags & SHF_ALLOC)) continue;
        REQUIRE(!(s->sh_flags & SHF_TLS) && s->sh_addralign <= 16 &&
                !(s->sh_addralign & (s->sh_addralign - 1)), "unsupported section flags or alignment");
        if (!strcmp(name, ".text")) group = TEXT;
        else if (!strcmp(name, ".rodata") || !strcmp(name, ".data.ro")) group = RODATA;
        else if (!strcmp(name, ".data") || !strcmp(name, ".data.rel.ro")) group = DATA;
        else if (!strcmp(name, ".bss")) group = BSS;
        REQUIRE(group && s->sh_type == (group == BSS ? SHT_NOBITS : SHT_PROGBITS), "unsupported allocated section");
        groups[i] = group;
        offsets[i] = align16(out[group].sh_size);
        REQUIRE(offsets[i] <= LIMIT - s->sh_size, "module section exceeds size limit");
        out[group].sh_size = offsets[i] + s->sh_size;
    }
    REQUIRE(symbols && symbols->sh_entsize == sizeof(*syms) && !(symbols->sh_offset & 3) &&
            symbols->sh_size % sizeof(*syms) == 0 && symbols->sh_link < eh->e_shnum, "symbol table format");
    strings = sh + symbols->sh_link;
    REQUIRE(strings->sh_type == SHT_STRTAB && strings->sh_size &&
            bounds(strings->sh_offset, strings->sh_size, bytes), "symbol strings bounds");
    syms = (const Elf32_Sym *)(in + symbols->sh_offset);
    symcount = symbols->sh_size / sizeof(*syms);
    REQUIRE(symcount && symbols->sh_info <= symcount, "symbol table count");
    for (unsigned i = TEXT; i <= BSS; i++) {
        out[i].sh_addr = cursor;
        cursor = align16(cursor + out[i].sh_size);
        REQUIRE(cursor <= LIMIT, "module virtual size limit");
    }
    for (unsigned i = 1; i < eh->e_shnum; i++) {
        const Elf32_Shdr *s = sh + i;
        REQUIRE(s->sh_type != SHT_RELA, "input requires REL word relocations");
        if (s->sh_type != SHT_REL) continue;
        REQUIRE(s->sh_entsize == sizeof(Elf32_Rel) && s->sh_size % sizeof(Elf32_Rel) == 0 &&
                !(s->sh_offset & 3) && s->sh_link == sym_section &&
                s->sh_info < eh->e_shnum && groups[s->sh_info], "unsupported relocation table");
        reloc_count += s->sh_size / sizeof(Elf32_Rel);
        REQUIRE(reloc_count <= LIMIT / sizeof(Elf32_Rela), "relocation count limit");
    }
    out[DYNSYM].sh_size = symbols->sh_size;
    out[DYNSTR].sh_size = strings->sh_size;
    out[RELA].sh_size = reloc_count * sizeof(Elf32_Rela);
    out[SHSTR].sh_size = sizeof(names);
    file_cursor = align16(sizeof(header));
    for (unsigned i = 1; i < NSECTIONS; i++) {
        out[i].sh_offset = file_cursor;
        if (i != BSS) file_cursor = align16(file_cursor + out[i].sh_size);
        REQUIRE(file_cursor <= LIMIT - sizeof(out), "module file size limit");
        out[i].sh_name = name_offsets[i]; out[i].sh_addralign = 4;
        out[i].sh_type = SHT_PROGBITS;
    }
    out[TEXT].sh_flags = SHF_ALLOC | SHF_EXECINSTR;
    out[RODATA].sh_flags = SHF_ALLOC;
    out[DATA].sh_flags = out[BSS].sh_flags = SHF_ALLOC | SHF_WRITE;
    out[BSS].sh_type = SHT_NOBITS;
    out[DYNSYM].sh_type = SHT_DYNSYM; out[DYNSYM].sh_link = DYNSTR;
    out[DYNSYM].sh_info = symbols->sh_info; out[DYNSYM].sh_entsize = sizeof(*syms);
    out[DYNSTR].sh_type = out[SHSTR].sh_type = SHT_STRTAB;
    out[RELA].sh_type = SHT_RELA; out[RELA].sh_link = DYNSYM;
    out[RELA].sh_entsize = sizeof(Elf32_Rela);
    image = calloc(1, file_cursor + sizeof(out));
    REQUIRE(image, "out of memory");
    for (unsigned i = 1; i < eh->e_shnum; i++)
        if (groups[i] && groups[i] != BSS)
            memcpy(image + out[groups[i]].sh_offset + offsets[i], in + sh[i].sh_offset, sh[i].sh_size);
    memcpy(image + out[DYNSTR].sh_offset, in + strings->sh_offset, strings->sh_size);
    for (unsigned i = 0; i < symcount; i++) {
        Elf32_Sym symbol = syms[i];
        REQUIRE(symbol.st_name < strings->sh_size && memchr(in + strings->sh_offset + symbol.st_name,
                0, strings->sh_size - symbol.st_name), "symbol name bounds");
        if (symbol.st_shndx && symbol.st_shndx != SHN_ABS) {
            unsigned section = symbol.st_shndx;
            REQUIRE(section < eh->e_shnum && groups[section] &&
                    bounds(symbol.st_value, symbol.st_size, sh[section].sh_size), "unsupported symbol section");
            symbol.st_value += out[groups[section]].sh_addr + offsets[section];
            symbol.st_shndx = groups[section];
        }
        if (!strcmp((const char *)in + strings->sh_offset + symbol.st_name, entry)) {
            REQUIRE(symbol.st_shndx == TEXT && ELF32_ST_TYPE(symbol.st_info) == STT_FUNC &&
                    symbol.st_size && !(symbol.st_value & 3), "entry is not an aligned text function");
            entry_address = symbol.st_value;
        }
        memcpy(image + out[DYNSYM].sh_offset + i * sizeof(symbol), &symbol, sizeof(symbol));
    }
    REQUIRE(entry_address, "entry function missing");
    for (unsigned i = 1; i < eh->e_shnum; i++) {
        const Elf32_Shdr *s = sh + i;
        if (s->sh_type != SHT_REL) continue;
        for (unsigned j = 0; j < s->sh_size / sizeof(Elf32_Rel); j++) {
            Elf32_Rel rel;
            Elf32_Rela dynamic = {0};
            Elf32_Sym symbol;
            uint32_t value, patch_at, symbol_index;
            memcpy(&rel, in + s->sh_offset + j * sizeof(rel), sizeof(rel));
            symbol_index = ELF32_R_SYM(rel.r_info);
            REQUIRE(ELF32_R_TYPE(rel.r_info) == 1 && symbol_index < symcount &&
                    !(rel.r_offset & 3) && groups[s->sh_info] != BSS &&
                    bounds(rel.r_offset, 4, sh[s->sh_info].sh_size), "unsupported word relocation");
            patch_at = out[groups[s->sh_info]].sh_offset + offsets[s->sh_info] + rel.r_offset;
            memcpy(&value, image + patch_at, 4);
            memcpy(&symbol, image + out[DYNSYM].sh_offset + symbol_index * sizeof(symbol), sizeof(symbol));
            dynamic.r_offset = out[groups[s->sh_info]].sh_addr + offsets[s->sh_info] + rel.r_offset;
            if (symbol.st_shndx == SHN_UNDEF) {
                REQUIRE(symbol_index && !value, "external relocation addends unsupported");
                dynamic.r_info = ELF32_R_INFO(symbol_index, 4); /* JMP_SLOT */
            } else {
                value += symbol.st_value;
                dynamic.r_info = ELF32_R_INFO(0, 5); /* RELATIVE: loader maps the stored word */
                REQUIRE(symbol.st_shndx != SHN_ABS, "absolute symbol relocations unsupported");
            }
            memcpy(image + patch_at, &value, 4);
            memcpy(image + out[RELA].sh_offset + reloc_at++ * sizeof(dynamic), &dynamic, sizeof(dynamic));
        }
    }
    memcpy(header.e_ident, "\177ELF\1\1\1", 7);
    header.e_type = ET_DYN; header.e_machine = EM_XTENSA; header.e_version = EV_CURRENT;
    header.e_entry = entry_address; header.e_ehsize = sizeof(header);
    header.e_shoff = file_cursor; header.e_shentsize = sizeof(out[0]);
    header.e_shnum = NSECTIONS; header.e_shstrndx = SHSTR;
    memcpy(image, &header, sizeof(header));
    memcpy(image + out[SHSTR].sh_offset, names, sizeof(names));
    memcpy(image + file_cursor, out, sizeof(out));
    *output = image; *output_bytes = file_cursor + sizeof(out); image = NULL;
    status = 0;
failed:
    free(groups); free(offsets); free(image);
    return status;
#undef REQUIRE
}

#ifdef SOLAR_TCC_MODULE_TOOL
int main(int argc, char **argv)
{
    FILE *file;
    long length;
    void *input, *output;
    size_t output_bytes;
    const char *error;
    int result;
    if (argc != 4) { fprintf(stderr, "usage: module INPUT.o OUTPUT.elf ENTRY\n"); return 2; }
    file = fopen(argv[1], "rb");
    if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 ||
        length > LIMIT || fseek(file, 0, SEEK_SET)) return 2;
    input = malloc(length);
    if (!input || fread(input, 1, length, file) != (size_t)length) return 2;
    fclose(file);
    result = solar_tcc_module(input, length, argv[3], &output, &output_bytes, &error);
    free(input);
    if (result) { fprintf(stderr, "%s\n", error); return 1; }
    file = fopen(argv[2], "wb");
    if (!file || fwrite(output, 1, output_bytes, file) != output_bytes || fclose(file)) return 2;
    free(output);
    return 0;
}
#endif
