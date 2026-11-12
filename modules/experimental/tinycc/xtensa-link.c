/* SPDX-License-Identifier: LGPL-2.0-or-later
 * The prototype emits ET_REL with word relocations only. module.c supplies
 * the SolarOS ET_DYN writer; unsupported generic linker paths fail closed.
 */
#ifdef TARGET_DEFS_ONLY
#define EM_TCC_TARGET EM_XTENSA
#ifndef R_XTENSA_32
#define R_XTENSA_32 1
#endif
#define R_DATA_32 R_XTENSA_32
#define R_DATA_PTR R_XTENSA_32
#define R_JMP_SLOT 4
#define R_GLOB_DAT 3
#define R_COPY 6
#define R_RELATIVE 5
#define R_NUM 256
#define ELF_START_ADDR 0x1000
#define ELF_PAGE_SIZE 0x1000
#define PCRELATIVE_DLLPLT 0
#define RELOCATE_DLLPLT 0
#else
#include "tcc.h"
ST_FUNC int code_reloc(int type) { return type == 1 ? 0 : -1; }
ST_FUNC int gotplt_entry_type(int type)
{ return type == 1 ? NO_GOTPLT_ENTRY : -1; }
#ifdef NEED_BUILD_GOT
ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned offset, struct sym_attr *attr)
{ (void)offset; (void)attr; tcc_error_noabort("use the SolarOS module writer"); return 0; }
ST_FUNC void relocate_plt(TCCState *s1)
{ tcc_error_noabort("use the SolarOS module writer"); }
#endif
ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type,
                      unsigned char *data, addr_t address, addr_t value)
{
    (void)rel; (void)address;
    if (type != 1) { tcc_error_noabort("unsupported Xtensa object relocation %d", type); return; }
    add32le(data, value);
}
#endif
