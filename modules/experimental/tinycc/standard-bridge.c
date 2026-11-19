/* Included inside the private libtcc namespace for both compiler hosts. */
int solar_os_cc_add_standard(TCCState *s1,const char *source,const char **entry)
{
    static const char *const names[]={
        "malloc","calloc","realloc","free","abs","labs","atoi","atof",
        "strcmp","strncmp","strcpy","strncpy","strcat","strchr","strrchr","strstr","memchr",
        "printf","vprintf","fprintf","vfprintf","snprintf","vsnprintf","puts","fputs","putchar","fputc","fgetc",
        "fopen","fclose","fflush","fread","fwrite","fseek","ftell","feof","ferror","clearerr","rewind","stdout","stderr",
        "floor","fabs","ldexp","expf","floorf","fmodf","logf","powf","sqrtf","fabsf",
        "solar_os_c_isnan","solar_os_c_isinf","solar_os_c_isfinite","solar_os_c_signbit",
    };
    int needed=0;
    for(unsigned i=1;i<s1->symtab->data_offset/sizeof(Elf32_Sym);i++) {
        const Elf32_Sym *symbol=(const Elf32_Sym *)s1->symtab->data+i;
        if(symbol->st_shndx!=SHN_UNDEF) continue;
        const char *name=(const char *)s1->symtab->link->data+symbol->st_name;
        for(unsigned j=0;j<sizeof(names)/sizeof(*names);j++) if(!strcmp(name,names[j])) needed=1;
    }
    if(!needed) return 0;
    int wrap_main=!strcmp(*entry,"main");
    if(wrap_main) tcc_define_symbol(s1,"SOLAR_OS_C_WRAP_MAIN","1");
    if(tcc_compile_string(s1,source)) return -1;
    if(wrap_main) *entry="solar_os_c_main";
    return 0;
}
