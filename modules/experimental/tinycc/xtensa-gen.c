/* SPDX-License-Identifier: LGPL-2.0-or-later
 * Experimental TinyCC backend for the ESP32-S3 windowed ABI.
 * Scalar and aggregate values. Reserved: a7 frame base, a8 comparison/call
 * scratch, a9 address scratch, a10-a15 outgoing arguments. The hardware
 * preserves a2-a7 across call8; return values arrive in the caller's a10.
 */
#ifdef TARGET_DEFS_ONLY
#define NB_REGS 5
#define RC_INT 1
#define RC_FLOAT 2
#define RC_R0 4
#define RC_R1 8
#define RC_R2 16
#define RC_R3 32
#define RC_IRET RC_R0
#define RC_IRE2 RC_R1
#define RC_FRET RC_R0
#define REG_IRET 0
#define REG_IRE2 1
#define REG_FRET 0
#define PTR_SIZE 4
#define LDOUBLE_SIZE 8
#define LDOUBLE_ALIGN 8
#define MAX_ALIGN 16
#define CHAR_IS_UNSIGNED
#define TCC_USING_DOUBLE_FOR_LDOUBLE 1
#define PROMOTE_RET
#else
#define USING_GLOBALS
#include "tcc.h"

ST_DATA const char *const target_machine_defs =
    "__XTENSA__\0__xtensa__\0__XTENSA_WINDOWED_ABI__\0";
ST_DATA const int reg_classes[NB_REGS] = {
    RC_INT | RC_FLOAT | RC_R0, RC_INT | RC_FLOAT | RC_R1,
    RC_INT | RC_FLOAT | RC_R2, RC_INT | RC_FLOAT | RC_R3, RC_INT | RC_FLOAT,
};
static int xt_entry_at, xt_frame_literal, xt_outgoing, xt_va_base, xt_va_index;

static void xt_byte(unsigned value)
{
    if (nocode_wanted) return;
    if (ind + 1 > cur_text_section->data_allocated)
        section_realloc(cur_text_section, ind + 1);
    cur_text_section->data[ind++] = value;
}
static void xt_word(unsigned value)
{ xt_byte(value); xt_byte(value >> 8); xt_byte(value >> 16); }
static void xt_dword(uint32_t value)
{ xt_byte(value); xt_byte(value >> 8); xt_byte(value >> 16); xt_byte(value >> 24); }
static void xt_patch_word(int at, unsigned value)
{
    unsigned char *p = cur_text_section->data + at;
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16;
}
static unsigned xt_jump_word(int at, int to)
{
    int offset = to - at - 4;
    if (offset < -131072 || offset > 131071)
        tcc_error("Xtensa prototype: branch exceeds 128 KiB");
    return 6u | ((unsigned)offset & 0x3ffffu) << 6;
}
static void xt_move(int to, int from)
{ if (to != from) xt_word(0x200000u | to << 12 | from << 8 | from << 4); }
static void xt_movi(int to, int value)
{
    if (value < -2048 || value > 2047) tcc_error("internal Xtensa MOVI range");
    xt_word(2u | to << 4 | ((unsigned)value & 0xf00u) | 0xa000u |
            ((unsigned)value & 0xffu) << 16);
}
/* Inline literals avoid a fixed-size pool and remain reachable by L32R.
 * The unconditional jump skips the aligned word during normal execution. */
static int xt_literal(int to, uint32_t value, Sym *symbol)
{
    int jump = ind, literal, instruction, displacement;
    if (nocode_wanted) return 0;
    xt_word(0);
    while (ind & 3) xt_byte(0);
    literal = ind;
    if (symbol) greloc(cur_text_section, symbol, literal, R_XTENSA_32);
    xt_dword(value);
    instruction = ind;
    displacement = (literal - ((instruction + 3) & ~3)) / 4;
    xt_word(1u | to << 4 | ((unsigned)displacement & 0xffffu) << 8);
    xt_patch_word(jump, xt_jump_word(jump, instruction));
    return literal;
}
static void xt_constant(int to, uint32_t value, Sym *symbol)
{
    if (!symbol && (int32_t)value >= -2048 && (int32_t)value <= 2047)
        xt_movi(to, (int32_t)value);
    else xt_literal(to, value, symbol);
}
static void xt_address(int to, int base, int offset)
{
    if (!offset) { xt_move(to, base); return; }
    if (to == base) tcc_error("internal Xtensa address alias");
    xt_constant(to, (uint32_t)offset, NULL);
    xt_word(0x800000u | to << 12 | base << 8 | to << 4);
}
static void xt_memory(int reg, int base, int offset, int size, int write, int sign)
{
    int scale = size == 4 ? 4 : size == 2 ? 2 : 1;
    int op = write ? (size == 4 ? 6 : size == 2 ? 5 : 4)
                   : (size == 4 ? 2 : size == 2 ? (sign ? 9 : 1) : 0);
    if (offset < 0 || offset % scale || offset / scale > 255) {
        xt_address(9, base, offset); base = 9; offset = 0;
    }
    xt_word(2u | reg << 4 | base << 8 | op << 12 | (unsigned)(offset / scale) << 16);
    if (!write && size == 1 && sign)
        xt_word(0x230000u | reg << 12 | reg << 8); /* sext reg,reg,7 */
}
static int xt_scalar_size(CType *type)
{
    int alignment, size = type_size(type, &alignment);
    if (type->t & VT_ARRAY) return 4; /* array-to-pointer conversion */
    if (size > 8 || (type->t & VT_BTYPE) == VT_STRUCT)
        tcc_error("Xtensa prototype requires scalar values or pointers");
    return size;
}
static void xt_load(int to, SValue *value)
{
    int kind = value->r & VT_VALMASK, type = value->type.t & VT_BTYPE;
    int size = xt_scalar_size(&value->type), base, offset = (int)value->c.i;
    if (size==8) size=4; /* The generic frontend loads/stores pairs separately. */
    if (type == VT_FUNC) size = 4;
    if (value->r & VT_LVAL) {
        if (kind == VT_LOCAL) base = 7;
        else if (kind == VT_LLOCAL) {
            xt_memory(9, 7, offset, 4, 0, 0); base = 9; offset = 0;
        } else if (kind == VT_CONST) {
            xt_constant(9, value->c.i, value->r & VT_SYM ? value->sym : NULL);
            base = 9; offset = 0;
        } else if (kind < NB_REGS) { base = kind + 2; offset = 0; }
        else { tcc_error("unsupported Xtensa lvalue"); return; }
        xt_memory(to, base, offset, size, 0, !(value->type.t & VT_UNSIGNED) && type != VT_BOOL);
    } else if (kind == VT_CONST) {
        xt_constant(to, value->c.i, value->r & VT_SYM ? value->sym : NULL);
    } else if (kind == VT_LOCAL) {
        xt_address(to, 7, offset);
    } else if (kind < NB_REGS) {
        xt_move(to, kind + 2);
    } else if (kind == VT_CMP) {
        if (offset == TOK_NE) xt_move(to, 8);
        else if (offset == TOK_EQ) {
            xt_movi(9, 1); xt_word(0x300000u | to << 12 | 8 << 8 | 9 << 4);
        } else tcc_error("unsupported Xtensa comparison state");
    } else if (kind == VT_JMP || kind == VT_JMPI) {
        int end;
        xt_movi(to, kind & 1);
        end = gjmp(0); gsym(offset); xt_movi(to, (kind & 1) ^ 1); gsym(end);
    } else tcc_error("unsupported Xtensa value");
}
ST_FUNC void load(int reg, SValue *value) { xt_load(reg + 2, value); }
ST_FUNC void store(int reg, SValue *value)
{
    int kind = value->r & VT_VALMASK, size = xt_scalar_size(&value->type);
    if (size==8) size=4;
    int base, offset = (int)value->c.i;
    if (kind == VT_LOCAL) base = 7;
    else if (kind == VT_CONST) {
        xt_constant(9, value->c.i, value->r & VT_SYM ? value->sym : NULL);
        base = 9; offset = 0;
    } else if (kind < NB_REGS) { base = kind + 2; offset = 0; }
    else { tcc_error("unsupported Xtensa store"); return; }
    xt_memory(reg + 2, base, offset, size, 1, 0);
}
ST_FUNC void o(unsigned int word) { xt_word(word); }
ST_FUNC void gen_fill_nops(int bytes) { while (bytes-- > 0) xt_byte(0); }
ST_FUNC int gjmp(int next)
{
    int at = ind;
    if (nocode_wanted) return next;
    xt_word(0); xt_dword(next);
    return at + 3;
}
ST_FUNC void gjmp_addr(int address)
{ if (!nocode_wanted) xt_word(xt_jump_word(ind, address)); }
ST_FUNC void gsym_addr(int list, int address)
{
    while (list) {
        int next = read32le(cur_text_section->data + list);
        xt_patch_word(list - 3, xt_jump_word(list - 3, address)); list = next;
    }
}
ST_FUNC int gjmp_append(int head, int tail)
{
    int at = head;
    if (!at) return tail;
    while (read32le(cur_text_section->data + at))
        at = read32le(cur_text_section->data + at);
    write32le(cur_text_section->data + at, tail);
    return head;
}
ST_FUNC int gjmp_cond(int op, int list)
{
    /* Skip the seven-byte unconditional branch when its predicate is false. */
    if (op != TOK_EQ && op != TOK_NE) tcc_error("unsupported Xtensa branch predicate");
    xt_word(0x16u | 8 << 8 | (op == TOK_NE ? 0 : 1) << 6 | 6 << 12);
    return gjmp(list);
}
ST_FUNC void gen_opi(int op)
{
    int left, right, destination;
    unsigned word = 0;
    gv2(RC_INT, RC_INT);
    left = vtop[-1].r + 2; right = vtop->r + 2; destination = left;
    switch (op) {
    case TOK_ADDC1: case TOK_SUBC1:
        xt_word((op==TOK_ADDC1 ? 0x800000u : 0xc00000u)|9<<12|left<<8|right<<4);
        xt_movi(8,1);
        /* add carry: sum < left; subtract borrow: left < right. */
        xt_word(7u|(op==TOK_ADDC1 ? left : right)<<4|
                (op==TOK_ADDC1 ? 9 : left)<<8|3<<12|2<<16);
        xt_movi(8,0); xt_move(destination,9); vtop--; return;
    case TOK_ADDC2: case TOK_SUBC2:
        word=op==TOK_ADDC2 ? 0x800000u : 0xc00000u;
        xt_word(word|9<<12|left<<8|right<<4);
        xt_word(word|destination<<12|9<<8|8<<4); vtop--; return;
    case '+': word = 0x800000; break;
    case '-': word = 0xc00000; break;
    case '&': word = 0x100000; break;
    case '|': word = 0x200000; break;
    case '^': word = 0x300000; break;
    case '*': word = 0x820000; break;
    case '/': case TOK_PDIV: word = 0xd20000; break;
    case TOK_UDIV: word = 0xc20000; break;
    case '%': word = 0xf20000; break;
    case TOK_UMOD: word = 0xe20000; break;
    case TOK_SHL: case TOK_SHR: case TOK_SAR:
        xt_word(0x400000u | (op == TOK_SHL ? 0x1000 : 0) | right << 8);
        xt_word((op == TOK_SHL ? 0xa10000u : op == TOK_SHR ? 0x910000u : 0xb10000u) |
                destination << 12 | (op == TOK_SHL ? left << 8 : left << 4));
        vtop--; return;
    default:
        if (op >= TOK_ULT && op <= TOK_GT) {
            int condition, swap = 0, temporary;
            switch (op) {
            case TOK_EQ: condition = 1; break;
            case TOK_NE: condition = 9; break;
            case TOK_LT: condition = 2; break;
            case TOK_GE: condition = 10; break;
            case TOK_ULT: condition = 3; break;
            case TOK_UGE: condition = 11; break;
            case TOK_LE: condition = 10; swap = 1; break;
            case TOK_GT: condition = 2; swap = 1; break;
            case TOK_ULE: condition = 11; swap = 1; break;
            case TOK_UGT: condition = 3; swap = 1; break;
            default: tcc_error("unsupported Xtensa comparison"); return;
            }
            if (swap) temporary = left, left = right, right = temporary;
            xt_movi(8, 1);
            xt_word(7u | right << 4 | left << 8 | condition << 12 | 2 << 16);
            xt_movi(8, 0);
            vtop--; vset_VT_CMP(TOK_NE); return;
        }
        tcc_error("unsupported Xtensa integer operation %d", op);
    }
    xt_word(word | destination << 12 | left << 8 | right << 4);
    vtop--;
}
ST_FUNC int gfunc_sret(CType *type, int variadic, CType *ret, int *align, int *size)
{
    int alignment,bytes=type_size(type,&alignment);(void)variadic;
    *align=alignment;*size=4;ret->t=VT_INT;ret->ref=NULL;
    return bytes<=16 ? (bytes+3)/4 : 0;
}
static int xt_type_words(CType *type,unsigned *slot)
{
    int align,size=type_size(type,&align);
    if(type->t&VT_ARRAY) size=align=4;
    if((type->t&VT_BTYPE)!=VT_STRUCT) xt_scalar_size(type);
    unsigned words=(size+3)/4;
    if(align>4) *slot=(*slot+(align/4)-1)&-(unsigned)(align/4);
    if(*slot<6 && *slot+words>6) *slot=6;
    return words;
}
static void xt_argument_word(int to,SValue *value,unsigned word)
{
    SValue part=*value;
    if((value->type.t&VT_BTYPE)==VT_STRUCT) {
        int align,bytes=type_size(&value->type,&align),kind=value->r&VT_VALMASK;
        xt_movi(to,0);
        for(unsigned b=0;b<4 && word*4+b<(unsigned)bytes;b++) {
            int offset=word*4+b;
            if(kind==VT_LLOCAL) {xt_memory(9,7,(int)value->c.i,4,0,0);xt_memory(9,9,offset,1,0,0);}
            else if(kind<NB_REGS) xt_memory(9,kind+2,offset,1,0,0);
            else {
                part=*value;part.type.t=VT_BYTE|VT_UNSIGNED;part.r|=VT_LVAL;part.c.i+=offset;
                xt_load(9,&part);
            }
            /* SLLI a9,a9,8/16/24; immediate encoding uses 32-shift. */
            if(b) {
                unsigned shift=32-b*8;
                xt_word(0x010000u|9<<12|9<<8|(shift&15)<<4|(shift&16)<<16);
            }
            xt_word(0x200000u|to<<12|to<<8|9<<4);
        }
        return;
    }
    if (word) {
        int kind=value->r&VT_VALMASK;
        if (value->r&VT_LVAL) {
            if (kind==VT_LLOCAL) {
                xt_memory(9,7,(int)value->c.i,4,0,0); xt_memory(to,9,4,4,0,0); return;
            }
            if (kind<NB_REGS) { xt_memory(to,kind+2,4,4,0,0); return; }
            part.c.i+=4;
        } else if (kind==VT_CONST) part.c.i>>=32;
        else part.r=value->r2;
    }
    if (xt_scalar_size(&value->type)>4) part.type.t=VT_INT|VT_UNSIGNED;
    xt_load(to,&part);
}
ST_FUNC void gfunc_call(int count)
{
    int argument; unsigned slot=0;
    /* CValue stores floating constants at compiler precision. Let the generic
     * frontend materialize target bits before reading argument words. */
    for(argument=0;argument<count;argument++) {
        SValue *value=vtop-count+1+argument;
        if(is_float(value->type.t) && (value->r&(VT_VALMASK|VT_LVAL|VT_SYM))==VT_CONST) {
            vpushv(value);gv(RC_FLOAT);*value=*vtop;vpop();
        }
    }
    SValue function = *(vtop - count);
    save_regs(count + 1);
    for (argument = 0; argument < count; argument++) {
        SValue *value = vtop - count + 1 + argument;
        int words=xt_type_words(&value->type,&slot);
        for (int word=0;word<words;word++,slot++) {
            if (slot<6) xt_argument_word(10+slot,value,word);
            else {
                int needed=(slot-5)*4;
                xt_argument_word(8,value,word); xt_memory(8,1,needed-4,4,1,0);
                if (needed>xt_outgoing) xt_outgoing=needed;
            }
        }
    }
    xt_load(8, &function);
    xt_word(0x0008e0); /* callx8 a8 */
    xt_move(2, 10); /* the callee's a2 is this window's a10 */
    xt_move(3,11);
    xt_move(4,12);xt_move(5,13);
    vtop -= count + 1;
}
ST_FUNC void gfunc_prolog(Sym *function)
{
    Sym *parameter;
    unsigned slot=0;
    if ((func_vt.t & VT_BTYPE)!=VT_VOID && (func_vt.t&VT_BTYPE)!=VT_STRUCT) xt_scalar_size(&func_vt);
    if (ind & 3) tcc_error("internal Xtensa function alignment");
    xt_entry_at = ind; xt_word(0x36 | 1 << 8); /* entry a1, frame: patched later */
    xt_move(8, 7); /* preserve the sixth incoming argument before using a7 */
    xt_frame_literal = xt_literal(7, 0, NULL);
    xt_word(0x800000u | 7 << 12 | 1 << 8 | 7 << 4);
    loc = 0; func_vc = 0; xt_outgoing = 0;
    if(func_var) {
        loc=-32;xt_va_base=loc;
        for(unsigned n=0;n<6;n++) xt_memory(n==5?8:2+n,7,loc+n*4,4,1,0);
    }
    if((func_vt.t&VT_BTYPE)==VT_STRUCT) {
        CType ret;int align,size;
        if(!gfunc_sret(&func_vt,func_var,&ret,&align,&size)) {
            loc-=4;func_vc=loc;xt_memory(2,7,loc,4,1,0);slot=1;
        }
    }
    for (parameter = function->type.ref->next; parameter; parameter = parameter->next) {
        int align;type_size(&parameter->type,&align);
        int words=xt_type_words(&parameter->type,&slot);
        if(align<4) align=4;
        loc=(loc-words*4)&-align;
        for (int word=0;word<words;word++,slot++) {
            if (slot<6) xt_memory(slot==5 ? 8 : 2+slot,7,loc+word*4,4,1,0);
            else {
                xt_memory(8,7,32+(slot-6)*4,4,0,0);
                xt_memory(8,7,loc+word*4,4,1,0);
            }
        }
        gfunc_set_param(parameter,loc,0);
    }
    xt_va_index=slot<6 ? slot*4 : 32+(slot-6)*4;
}
ST_FUNC void gfunc_epilog(void)
{
    unsigned frame = ((unsigned)(-loc) + xt_outgoing + 32 + 15) & ~15u;
    if (frame > 32752) tcc_error("Xtensa prototype function frame exceeds 32752 bytes");
    if((func_vt.t&VT_BTYPE)==VT_STRUCT && func_vc) xt_memory(2,7,func_vc,4,0,0);
    xt_word(0x000090); /* retw */
    xt_patch_word(xt_entry_at, 0x136u | (frame / 8) << 12);
    write32le(cur_text_section->data + xt_frame_literal, frame - 32);
}
ST_FUNC void ggoto(void)
{ tcc_error("Xtensa prototype does not support computed goto"); }
static void xt_support_call(const char *name,int count,int type)
{
    int token=tok_alloc(name,strlen(name))->tok;
    vpush_helper_func(token); vrott(count+1); gfunc_call(count);
    vpushi(0); vtop->type.t=type; PUT_R_RET(vtop,type);
}
ST_FUNC void gen_opl_runtime(int op)
{
    if(op=='*') {int type=vtop->type.t;xt_support_call("__solar_cc_u64_mul",2,type);return;}
    const char *kind="i64",*operation=NULL;
    switch(op) {
    case TOK_ULT:kind="u64";operation="lt";break;case TOK_ULE:kind="u64";operation="le";break;
    case TOK_UGT:kind="u64";operation="gt";break;case TOK_UGE:kind="u64";operation="ge";break;
    case TOK_LT:operation="lt";break;case TOK_LE:operation="le";break;
    case TOK_GT:operation="gt";break;case TOK_GE:operation="ge";break;
    case TOK_EQ:operation="eq";break;case TOK_NE:operation="ne";break;
    default:tcc_error("unsupported wide comparison");return;
    }
    char name[64];snprintf(name,sizeof(name),"__solar_cc_%s_%s",kind,operation);
    xt_support_call(name,2,VT_INT);xt_move(8,2);vset_VT_CMP(TOK_NE);
}
ST_FUNC void gen_opf(int op)
{
    int type=vtop->type.t;
    const char *operation;
    switch(op) {
    case '+':operation="add";break;case '-':operation="sub";break;
    case '*':operation="mul";break;case '/':operation="div";break;
    case TOK_EQ:operation="eq";break;case TOK_NE:operation="ne";break;
    case TOK_LT:operation="lt";break;case TOK_LE:operation="le";break;
    case TOK_GT:operation="gt";break;case TOK_GE:operation="ge";break;
    default:tcc_error("unsupported floating-point operation");return;
    }
    int comparison=op!= '+' && op!='-' && op!='*' && op!='/';
    char name[64]; snprintf(name,sizeof(name),"__solar_cc_f%d_%s",(type&VT_BTYPE)==VT_FLOAT?32:64,operation);
    xt_support_call(name,2,comparison?VT_INT:type);
    if(comparison) {xt_move(8,2);vset_VT_CMP(TOK_NE);}
}
static const char *xt_numeric_kind(int type)
{
    switch(type&VT_BTYPE) {
    case VT_FLOAT:return "f32";case VT_DOUBLE:case VT_LDOUBLE:return "f64";
    case VT_LLONG:return type&VT_UNSIGNED?"u64":"i64";
    default:return type&VT_UNSIGNED?"u32":"i32";
    }
}
static void xt_convert(int type)
{
    if(!strcmp(xt_numeric_kind(vtop->type.t),xt_numeric_kind(type))) {vtop->type.t=type;return;}
    char name[64];snprintf(name,sizeof(name),"__solar_cc_%s_%s",xt_numeric_kind(vtop->type.t),xt_numeric_kind(type));
    xt_support_call(name,1,type);
}
ST_FUNC void gen_cvt_ftoi(int type) {xt_convert(type);}
ST_FUNC void gen_cvt_itof(int type) {xt_convert(type);}
ST_FUNC void gen_cvt_ftof(int type) {xt_convert(type);}
ST_FUNC void gen_va_start(void)
{
    vpop();int address=gv(RC_INT)+2;
    xt_memory(7,address,0,4,1,0);
    xt_address(8,7,xt_va_base);xt_memory(8,address,4,4,1,0);
    xt_constant(8,xt_va_index,NULL);xt_memory(8,address,8,4,1,0);
    vpop();vpushi(0);vtop->type.t=VT_VOID;
}
ST_FUNC void gen_vla_sp_save(int address)
{ (void)address; tcc_error("Xtensa prototype does not support variable-length arrays"); }
ST_FUNC void gen_vla_sp_restore(int address)
{ (void)address; tcc_error("Xtensa prototype does not support variable-length arrays"); }
ST_FUNC void gen_vla_alloc(CType *type, int align)
{ (void)type; (void)align; tcc_error("Xtensa prototype does not support variable-length arrays"); }
#endif
