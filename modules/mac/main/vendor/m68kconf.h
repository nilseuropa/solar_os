/* ======================================================================== */
/* ========================= LICENSING & COPYRIGHT ======================== */
/* ======================================================================== */
/*
 *                                  MUSASHI
 *                                Version 3.4
 *
 * A portable Motorola M680x0 processor emulation engine.
 * Copyright 1998-2001 Karl Stenerud.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */



#ifndef M68KCONF__HEADER
#define M68KCONF__HEADER


/* Configuration switches. */
#define OPT_OFF             0
#define OPT_ON              1
#define OPT_SPECIFY_HANDLER 2


/* ======================================================================== */
/* ============================== MAME STUFF ============================== */
/* ======================================================================== */

#ifndef M68K_COMPILE_FOR_MAME
#define M68K_COMPILE_FOR_MAME      OPT_OFF
#endif


#if M68K_COMPILE_FOR_MAME == OPT_OFF


/* ======================================================================== */
/* ============================= CONFIGURATION ============================ */
/* ======================================================================== */

#define M68K_EMULATE_010            OPT_OFF
#define M68K_EMULATE_EC020          OPT_OFF
#define M68K_EMULATE_020            OPT_OFF

#define M68K_SEPARATE_READS         OPT_ON

#define M68K_SIMULATE_PD_WRITES     OPT_OFF

#define M68K_EMULATE_INT_ACK        OPT_OFF
#define M68K_INT_ACK_CALLBACK(A)    m68k_int_ack(A)

#define M68K_EMULATE_BKPT_ACK       OPT_OFF
#define M68K_BKPT_ACK_CALLBACK()    your_bkpt_ack_handler_function()

#define M68K_EMULATE_TRACE          OPT_OFF

#define M68K_EMULATE_RESET          OPT_OFF
#define M68K_RESET_CALLBACK()       your_reset_handler_function()

#define M68K_EMULATE_FC             OPT_OFF
#define M68K_SET_FC_CALLBACK(A)     your_set_fc_handler_function(A)

#define M68K_MONITOR_PC             OPT_OFF
#define M68K_SET_PC_CALLBACK(A)     m68k_pc_changed_handler_function(A)
void m68k_pc_changed_handler_function(unsigned int addr);

#ifdef SOLAR_OS_MAC_TRACE
#define M68K_INSTRUCTION_HOOK       OPT_SPECIFY_HANDLER
#else
#define M68K_INSTRUCTION_HOOK       OPT_OFF
#endif
#define M68K_INSTRUCTION_CALLBACK() m68k_instruction()
void m68k_instruction();

#define M68K_EMULATE_PREFETCH       OPT_OFF

#define M68K_EMULATE_ADDRESS_ERROR  OPT_OFF

#define M68K_LOG_ENABLE             OPT_OFF
#define M68K_LOG_1010_1111          OPT_OFF
#define M68K_LOG_FILEHANDLE         stderr

#define M68K_USE_64_BIT  OPT_OFF

#ifndef INLINE
#define INLINE static __inline__
#endif

#endif /* M68K_COMPILE_FOR_MAME */


/* ======================================================================== */
/* ============================== END OF FILE ============================= */
/* ======================================================================== */

#include <stdint.h>

/* Route instruction fetches through the checked 24-bit memory map too. */
unsigned int m68k_read_memory_8(unsigned int address);
unsigned int m68k_read_memory_16(unsigned int address);
unsigned int m68k_read_memory_32(unsigned int address);
static inline unsigned int m68k_read_immediate_16(unsigned int a) { return m68k_read_memory_16(a); }
static inline unsigned int m68k_read_immediate_32(unsigned int a) { return m68k_read_memory_32(a); }
static inline unsigned int m68k_read_pcrelative_8(unsigned int a) { return m68k_read_memory_8(a); }
static inline unsigned int m68k_read_pcrelative_16(unsigned int a) { return m68k_read_memory_16(a); }
static inline unsigned int m68k_read_pcrelative_32(unsigned int a) { return m68k_read_memory_32(a); }

#endif /* M68KCONF__HEADER */
