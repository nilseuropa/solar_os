/* SolarOS adapter for the umac/Basilisk II Sony driver. GPL-2.0-or-later. */
#pragma once
#include "port.h"
#include "m68k.h"
#define ADR24(x) ((x) & 0xffffffU)
#define RAM_RD8(a) m68k_read_memory_8(a)
#define RAM_RD16(a) m68k_read_memory_16(a)
#define RAM_RD32(a) m68k_read_memory_32(a)
#define RAM_WR8(a,v) m68k_write_memory_8(a,v)
#define RAM_WR16(a,v) m68k_write_memory_16(a,v)
#define RAM_WR32(a,v) m68k_write_memory_32(a,v)
#define WRITE_BYTE(p,o,v) (((uint8_t *)(p))[o] = (uint8_t)(v))
#define WRITE_WORD(p,o,v) do { WRITE_BYTE(p,o,(v)>>8); WRITE_BYTE(p,(o)+1,v); } while (0)
#define WRITE_LONG(p,o,v) do { WRITE_WORD(p,o,(v)>>16); WRITE_WORD(p,(o)+2,v); } while (0)
#define fprintf(...) ((void)0)
