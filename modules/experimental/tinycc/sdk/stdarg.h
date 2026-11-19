#ifndef SOLAR_CC_STDARG_H
#define SOLAR_CC_STDARG_H
/* Xtensa windowed ABI: stack base, saved argument registers, byte index.
 * Indices 0..24 address registers; stack arguments start at index 32. */
typedef __builtin_va_list va_list;
typedef va_list __gnuc_va_list;
#define _VA_LIST_DEFINED
#undef __builtin_va_start
#define va_start(ap,last) __builtin_va_start(&(ap),0)
#define va_end(ap) ((void)0)
#define va_copy(to,from) ((to)=(from))
#define va_arg(ap,type) __extension__ ({ \
    unsigned __cc_size=(sizeof(type)+3U)&~3U; \
    unsigned __cc_align=__alignof__(type)>4?__alignof__(type):4; \
    unsigned __cc_old=((ap).__va_ndx+__cc_align-1)&-__cc_align; \
    unsigned __cc_next=__cc_old+__cc_size; \
    char *__cc_base=(ap).__va_reg; \
    if(__cc_next>24) { \
        if(__cc_old<=24) __cc_next=32+__cc_size; \
        __cc_base=(ap).__va_stk; \
    } \
    (ap).__va_ndx=__cc_next; \
    *(type *)(__cc_base+__cc_next-__cc_size); \
})
#endif
