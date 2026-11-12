/* SPDX-License-Identifier: LGPL-2.0-or-later
 * Experimental TinyCC backend for the ESP32-S3 windowed ABI.
 * Scalar 32-bit values only. Reserved: a7 frame base, a8 comparison/call
 * scratch, a9 address scratch, a10-a15 outgoing arguments. The hardware
 * preserves a2-a7 across call8; return values arrive in the caller's a10.
 */
#ifdef TARGET_DEFS_ONLY
#define NB_REGS 5
#define RC_INT 1
#define RC_FLOAT 2
#define RC_R0 4
#define RC_R1 8
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
    RC_INT | RC_FLOAT, RC_INT | RC_FLOAT, RC_INT | RC_FLOAT,
};
static int xt_entry_at, xt_frame_literal, xt_outgoing;

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
    if (is_float(type->t) || size > 4 || (type->t & VT_BTYPE) == VT_STRUCT)
        tcc_error("Xtensa prototype supports scalar integers and pointers up to 32 bits");
    return size;
}
static void xt_load(int to, SValue *value)
{
    int kind = value->r & VT_VALMASK, type = value->type.t & VT_BTYPE;
    int size = xt_scalar_size(&value->type), base, offset = (int)value->c.i;
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
    (void)type; (void)variadic; (void)ret; (void)align; (void)size;
    tcc_error("Xtensa prototype does not support structure returns"); return 0;
}
ST_FUNC void gfunc_call(int count)
{
    int argument;
    SValue function = *(vtop - count);
    save_regs(count + 1);
    for (argument = 0; argument < count; argument++) {
        SValue *value = vtop - count + 1 + argument;
        xt_scalar_size(&value->type);
        if (argument < 6) xt_load(10 + argument, value);
        else {
            int needed = (argument - 5) * 4;
            xt_load(8, value);
            xt_memory(8, 1, needed - 4, 4, 1, 0);
            if (needed > xt_outgoing) xt_outgoing = needed;
        }
    }
    xt_load(8, &function);
    xt_word(0x0008e0); /* callx8 a8 */
    xt_move(2, 10); /* the callee's a2 is this window's a10 */
    vtop -= count + 1;
}
ST_FUNC void gfunc_prolog(Sym *function)
{
    Sym *parameter;
    int argument = 0;
    if (func_var) tcc_error("Xtensa prototype does not support variadic definitions");
    if ((func_vt.t & VT_BTYPE) != VT_VOID) xt_scalar_size(&func_vt);
    if (ind & 3) tcc_error("internal Xtensa function alignment");
    xt_entry_at = ind; xt_word(0x36 | 1 << 8); /* entry a1, frame: patched later */
    xt_move(8, 7); /* preserve the sixth incoming argument before using a7 */
    xt_frame_literal = xt_literal(7, 0, NULL);
    xt_word(0x800000u | 7 << 12 | 1 << 8 | 7 << 4);
    loc = 0; func_vc = 0; xt_outgoing = 0;
    for (parameter = function->type.ref->next; parameter; parameter = parameter->next) {
        xt_scalar_size(&parameter->type); loc -= 4;
        if (argument < 6) xt_memory(argument == 5 ? 8 : 2 + argument, 7, loc, 4, 1, 0);
        else {
            xt_memory(8, 7, 32 + (argument - 6) * 4, 4, 0, 0);
            xt_memory(8, 7, loc, 4, 1, 0);
        }
        gfunc_set_param(parameter, loc, 0); argument++;
    }
}
ST_FUNC void gfunc_epilog(void)
{
    unsigned frame = ((unsigned)(-loc) + xt_outgoing + 32 + 15) & ~15u;
    if (frame > 32752) tcc_error("Xtensa prototype function frame exceeds 32752 bytes");
    xt_word(0x000090); /* retw */
    xt_patch_word(xt_entry_at, 0x136u | (frame / 8) << 12);
    write32le(cur_text_section->data + xt_frame_literal, frame - 32);
}
ST_FUNC void ggoto(void)
{ tcc_error("Xtensa prototype does not support computed goto"); }
ST_FUNC void gen_opf(int op)
{ (void)op; tcc_error("Xtensa prototype does not support floating-point operations"); }
ST_FUNC void gen_cvt_ftoi(int type)
{ (void)type; tcc_error("Xtensa prototype does not support floating-point conversions"); }
ST_FUNC void gen_cvt_itof(int type)
{ (void)type; tcc_error("Xtensa prototype does not support floating-point conversions"); }
ST_FUNC void gen_cvt_ftof(int type)
{ (void)type; tcc_error("Xtensa prototype does not support floating-point conversions"); }
ST_FUNC void gen_vla_sp_save(int address)
{ (void)address; tcc_error("Xtensa prototype does not support variable-length arrays"); }
ST_FUNC void gen_vla_sp_restore(int address)
{ (void)address; tcc_error("Xtensa prototype does not support variable-length arrays"); }
ST_FUNC void gen_vla_alloc(CType *type, int align)
{ (void)type; (void)align; tcc_error("Xtensa prototype does not support variable-length arrays"); }
#endif
