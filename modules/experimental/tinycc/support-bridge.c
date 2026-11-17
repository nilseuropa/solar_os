/* Included inside the private compiler, after its ELF definitions. */
struct cc_support_section { unsigned group,alignment,bytes; const unsigned char *data; };
struct cc_support_symbol { const char *name; unsigned section,offset,bytes; };
struct cc_support_relocation { unsigned section,offset,target,addend; };
#include "support.inc"
#define CC_COUNT(array) (sizeof(array)/sizeof((array)[0]))
int solar_os_cc_add_support(TCCState *s1)
{
    unsigned i,j;
    for (i=1;i<s1->symtab->data_offset/sizeof(Elf32_Sym);i++) {
        const Elf32_Sym *sym=(const Elf32_Sym *)s1->symtab->data+i;
        if (sym->st_shndx!=SHN_UNDEF) continue;
        const char *name=(const char *)s1->symtab->link->data+sym->st_name;
        for (j=0;j<CC_COUNT(cc_support_symbols);j++)
            if (!strcmp(name,cc_support_symbols[j].name)) goto needed;
    }
    return 0;
needed:;
    Section *targets[]={text_section,rodata_section,data_section,bss_section};
    unsigned bases[CC_COUNT(cc_support_sections)],anchors[CC_COUNT(cc_support_sections)];
    for (i=0;i<CC_COUNT(cc_support_sections);i++) {
        const struct cc_support_section *s=cc_support_sections+i;
        Section *target=targets[s->group];
        bases[i]=section_add(target,s->bytes,s->alignment);
        if (s->group!=3) memcpy(target->data+bases[i],s->data,s->bytes);
        anchors[i]=put_elf_sym(s1->symtab,bases[i],0,ELF32_ST_INFO(STB_LOCAL,STT_SECTION),0,target->sh_num,NULL);
    }
    for (i=0;i<CC_COUNT(cc_support_relocations);i++) {
        const struct cc_support_relocation *r=cc_support_relocations+i;
        Section *target=targets[cc_support_sections[r->section].group];
        unsigned offset=bases[r->section]+r->offset;
        write32le(target->data+offset,r->addend);
        put_elf_reloc(s1->symtab,target,offset,R_XTENSA_32,anchors[r->target]);
    }
    for (i=0;i<CC_COUNT(cc_support_symbols);i++) {
        const struct cc_support_symbol *sym=cc_support_symbols+i;
        set_elf_sym(s1->symtab,bases[sym->section]+sym->offset,sym->bytes,
            ELF32_ST_INFO(STB_GLOBAL,STT_FUNC),STV_HIDDEN,targets[cc_support_sections[sym->section].group]->sh_num,sym->name);
    }
    return 0;
}
#undef CC_COUNT
