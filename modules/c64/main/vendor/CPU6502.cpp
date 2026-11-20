/*
 Copyright (C) 2024-2026 retroelec <retroelec42@gmail.com>

 This program is free software; you can redistribute it and/or modify it
 under the terms of the GNU General Public License as published by the
 Free Software Foundation; either version 3 of the License, or (at your
 option) any later version.

 This program is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 for more details.

 For the complete text of the GNU General Public License see
 http://www.gnu.org/licenses/.
*/
#include "C64Sys.h"

// This module has one bus implementation. Resolve it directly so the native
// compiler can inline RAM accesses instead of dispatching through a vtable
// for every opcode, operand and stack byte.
inline uint8_t CPU6502::getMem(uint16_t addr) {
  return static_cast<C64Sys *>(this)->getMem(addr);
}
inline void CPU6502::setMem(uint16_t addr, uint8_t value) {
  static_cast<C64Sys *>(this)->setMem(addr, value);
}

void CPU6502::modeZeropage() {
  zl = getMem(pc++);
  z = zl;
}

void CPU6502::modeZeropageX() {
  zl = getMem(pc++);
  zl += x;
  z = zl;
}

void CPU6502::modeZeropageY() {
  zl = getMem(pc++);
  zl += y;
  z = zl;
}

void CPU6502::modeAbsolute() {
  zl = getMem(pc++);
  zh = getMem(pc++);
  z = (zl + (zh << 8));
}

void CPU6502::modeAbsoluteX() {
  zl = getMem(pc++);
  zh = getMem(pc++);
  z = (x + zl + (zh << 8));
}

void CPU6502::modeAbsoluteY() {
  zl = getMem(pc++);
  zh = getMem(pc++);
  z = (y + zl + (zh << 8));
}

void CPU6502::modeIndirectX() {
  uint8_t ql = getMem(pc++);
  ql += x;
  zl = getMem(ql++);
  zh = getMem(ql);
  z = (zl + (zh << 8));
}

void CPU6502::modeIndirectY() {
  uint16_t q = getMem(pc++);
  zl = getMem(q++);
  zh = getMem(q);
  z = (y + (zl | (zh << 8)));
}

void CPU6502::setNZ(uint8_t r) {
  zflag = !r;
  nflag = r & 0x80;
}

void CPU6502::atestandsetNZ() { setNZ(a); }

void CPU6502::xtestandsetNZ() { setNZ(x); }

void CPU6502::ytestandsetNZ() { setNZ(y); }

void CPU6502::branchbase(bool flag) {
  int8_t r = getMem(pc++);
  if (flag) {
    uint16_t oldPC = pc;
    pc += r;
    numofcycles++;
    if ((oldPC & 0xFF00) != (pc & 0xFF00)) {
      numofcycles++;
    }
  }
  numofcycles += 2;
}

void CPU6502::adcbase(uint8_t r) {
  if (!dflag) {
    uint16_t a1 = a + r;
    if (cflag) {
      a1++;
    }
    uint8_t a2 = a1;
    vflag = (a ^ a2) & (r ^ a2) & 0x80;
    cflag = a1 >> 8;
    zflag = !a2;
    nflag = a2 & 0x80;
    a = a2;
  } else {
    uint16_t a2 = a + r;
    uint8_t al = (a & 0x0F) + (r & 0x0F);
    if (cflag) {
      al++;
      a2++;
    }
    zflag = !(a2 & 0xff);
    if (al >= 0x0A) {
      al = ((al + 0x06) & 0x0F) + 0x10;
    }
    uint16_t a1 = (a & 0xF0) + (r & 0xF0) + al;
    vflag = (a ^ a1) & (r ^ a1) & 0x80;
    nflag = a1 & 0x80;
    cflag = (a1 >= 0xA0);
    if (a1 >= 0xA0) {
      a1 += 0x60;
    }
    a = a1;
  }
}

void CPU6502::sbcbase(uint8_t r1) {
  if (!dflag) {
    adcbase(~r1);
  } else {
    uint8_t r = ~r1;
    uint16_t a2 = a + r;
    uint8_t al = (a & 0x0F) + (r & 0x0F);
    if (cflag) {
      al++;
      a2++;
    }
    zflag = !(a2 & 0xff);
    if (al < 0x10) {
      al = ((al + 0x0a) & 0x0F);
    }
    uint16_t a1 = (a & 0xF0) + (r & 0xF0) + al;
    vflag = (a ^ a1) & (r ^ a1) & 0x80;
    nflag = a1 & 0x80;
    cflag = a1 >> 8;
    if (a1 < 0x100) {
      a1 += 0xA0;
    }
    a = a1;
  }
}

void CPU6502::incbase() {
  uint8_t r = getMem(z);
  r++;
  setMem(z, r);
  setNZ(r);
}

void CPU6502::decbase() {
  uint8_t r = getMem(z);
  r--;
  setMem(z, r);
  setNZ(r);
}

void CPU6502::cmpbase(uint8_t r1, uint8_t r2) {
  int16_t r = r1 - r2;
  cflag = true;
  if (r < 0) {
    r += 0x100;
    cflag = false;
  }
  setNZ(r);
}

uint8_t CPU6502::aslbase0(uint8_t r) {
  uint16_t r1 = r << 1;
  cflag = false;
  if (r1 & 0x100) {
    cflag = true;
    r1 &= 0xFF;
  }
  setNZ(r1);
  return r1;
}

void CPU6502::aslbase() {
  uint8_t r = getMem(z);
  r = aslbase0(r);
  setMem(z, r);
}

uint8_t CPU6502::lsrbase0(uint8_t r) {
  cflag = r & 0x01;
  r >>= 1;
  setNZ(r);
  return r;
}

void CPU6502::lsrbase() {
  uint8_t r = getMem(z);
  r = lsrbase0(r);
  setMem(z, r);
}

uint8_t CPU6502::rolbase0(uint8_t r) {
  uint16_t r1 = r << 1;
  if (cflag) {
    r1 |= 1;
  }
  cflag = false;
  if (r1 & 0x100) {
    cflag = true;
    r1 &= 0xFF;
  }
  setNZ(r1);
  return r1;
}

void CPU6502::rolbase() {
  uint8_t r = getMem(z);
  r = rolbase0(r);
  setMem(z, r);
}

uint8_t CPU6502::rorbase0(uint8_t r) {
  uint16_t r1 = r;
  if (cflag) {
    r1 |= 0x100;
  }
  cflag = r1 & 0x01;
  r1 >>= 1;
  setNZ(r1);
  return r1;
}

void CPU6502::rorbase() {
  uint8_t r = getMem(z);
  r = rorbase0(r);
  setMem(z, r);
}

void CPU6502::bitBase() {
  uint8_t r = getMem(z);
  nflag = r & 128;
  vflag = r & 64;
  r &= a;
  zflag = r == 0;
}

void CPU6502::srfromflags() {
  sr = 32;
  if (cflag) {
    sr |= 1;
  }
  if (zflag) {
    sr |= 2;
  }
  if (iflag) {
    sr |= 4;
  }
  if (dflag) {
    sr |= 8;
  }
  if (bflag) {
    sr |= 16;
  }
  if (vflag) {
    sr |= 64;
  }
  if (nflag) {
    sr |= 128;
  }
}

void CPU6502::flagsfromsr() {
  cflag = sr & 1;
  zflag = sr & 2;
  iflag = sr & 4;
  dflag = sr & 8;
  bflag = sr & 16;
  vflag = sr & 64;
  nflag = sr & 128;
}

void CPU6502::pushtostack(uint8_t r) {
  uint16_t z1 = sp + 0x100;
  setMem(z1, r);
  sp--;
}

uint8_t CPU6502::pullfromstack() {
  sp++;
  uint16_t z1 = sp + 0x100;
  return getMem(z1);
}

void CPU6502::cmd6502halt() { cpuhalted = true; }

void CPU6502::cmd6502brk() {
  pc++;
  setPCToIntVec(getMem(0xfffe) + (getMem(0xffff) << 8), true);
}

void CPU6502::cmd6502oraIndirectX() {
  modeIndirectX();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502oraZeropage() {
  modeZeropage();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502aslZeropage() {
  modeZeropage();
  aslbase();
  numofcycles += 5;
}

void CPU6502::php() {
  srfromflags();
  pushtostack(sr);
}

void CPU6502::cmd6502php() {
  bflag = true;
  php();
}

void CPU6502::cmd6502oraImmediate() {
  uint8_t r = getMem(pc++);
  a |= r;
  atestandsetNZ();
  numofcycles += 3;
}

void CPU6502::cmd6502aslA() {
  a = aslbase0(a);
  numofcycles += 2;
}

void CPU6502::cmd6502oraAbsolute() {
  modeAbsolute();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502aslAbsolute() {
  modeAbsolute();
  aslbase();
  numofcycles += 6;
}

void CPU6502::cmd6502bpl() { branchbase(!nflag); }

void CPU6502::cmd6502oraIndirectY() {
  uint16_t zold = z;
  modeIndirectY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 5;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502oraZeropageX() {
  modeZeropageX();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502aslZeropageX() {
  modeZeropageX();
  aslbase();
  numofcycles += 6;
}

void CPU6502::cmd6502clc() {
  cflag = false;
  numofcycles += 2;
}

void CPU6502::cmd6502oraAbsoluteY() {
  uint16_t zold = z;
  modeAbsoluteY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502oraAbsoluteX() {
  uint16_t zold = z;
  modeAbsoluteX();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502aslAbsoluteX() {
  modeAbsoluteX();
  aslbase();
  numofcycles += 7;
}

void CPU6502::cmd6502jsr() {
  uint8_t ql = getMem(pc++);
  // push actual address to 6502 stack
  uint8_t pcl = pc & 0xFF;
  uint8_t pch = (pc >> 8);
  pushtostack(pch);
  pushtostack(pcl);
  // set destination address
  uint8_t qh = getMem(pc);
  uint16_t q = (ql + (qh << 8));
  pc = q;
  numofcycles += 6;
}

void CPU6502::cmd6502andIndirectX() {
  modeIndirectX();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502bitZeropage() {
  modeZeropage();
  bitBase();
  numofcycles += 3;
}

void CPU6502::cmd6502andZeropage() {
  modeZeropage();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 3;
}

void CPU6502::cmd6502rolZeropage() {
  modeZeropage();
  rolbase();
  numofcycles += 5;
}

void CPU6502::plp() {
  sr = pullfromstack();
  flagsfromsr();
}

void CPU6502::cmd6502plp() {
  plp();
  numofcycles += 4;
}

void CPU6502::cmd6502andImmediate() {
  uint8_t r = getMem(pc++);
  a &= r;
  atestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502ancImmediate() {
  uint8_t r = getMem(pc++);
  a &= r;
  atestandsetNZ();
  cflag = nflag;
  numofcycles += 2;
}

void CPU6502::cmd6502rolA() {
  a = rolbase0(a);
  numofcycles += 2;
}

void CPU6502::cmd6502bitAbsolute() {
  modeAbsolute();
  bitBase();
  numofcycles += 4;
}

void CPU6502::cmd6502andAbsolute() {
  modeAbsolute();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502rolAbsolute() {
  modeAbsolute();
  rolbase();
  numofcycles += 6;
}

void CPU6502::cmd6502bmi() { branchbase(nflag); }

void CPU6502::cmd6502andIndirectY() {
  uint16_t zold = z;
  modeIndirectY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 5;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502andZeropageX() {
  modeZeropageX();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502rolZeropageX() {
  modeZeropageX();
  rolbase();
  numofcycles += 6;
}

void CPU6502::cmd6502sec() {
  cflag = true;
  numofcycles += 2;
}

void CPU6502::cmd6502andAbsoluteY() {
  uint16_t zold = z;
  modeAbsoluteY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502andAbsoluteX() {
  uint16_t zold = z;
  modeAbsoluteX();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502rolAbsoluteX() {
  modeAbsoluteX();
  rolbase();
  numofcycles += 7;
}

void CPU6502::cmd6502rti() {
  // get status register from stack
  plp();
  // get return address from stack
  uint8_t pcl = pullfromstack();
  uint8_t pch = pullfromstack();
  pc = (pcl + (pch << 8));
  numofcycles += 6;
}

void CPU6502::cmd6502eorIndirectX() {
  modeIndirectX();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502eorZeropage() {
  modeZeropage();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 3;
}

void CPU6502::cmd6502lsrZeropage() {
  modeZeropage();
  lsrbase();
  numofcycles += 5;
}

void CPU6502::cmd6502pha() {
  pushtostack(a);
  numofcycles += 3;
}

void CPU6502::cmd6502eorImmediate() {
  uint8_t r = getMem(pc++);
  a ^= r;
  atestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502lsrA() {
  a = lsrbase0(a);
  numofcycles += 2;
}

void CPU6502::cmd6502jmpAbsolute() {
  modeAbsolute();
  pc = z;
  numofcycles += 3;
}

void CPU6502::cmd6502eorAbsolute() {
  modeAbsolute();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502lsrAbsolute() {
  modeAbsolute();
  lsrbase();
  numofcycles += 6;
}

void CPU6502::cmd6502bvc() { branchbase(!vflag); }

void CPU6502::cmd6502eorIndirectY() {
  uint16_t zold = z;
  modeIndirectY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 5;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502eorZeropageX() {
  modeZeropageX();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502lsrZeropageX() {
  modeZeropageX();
  lsrbase();
  numofcycles += 6;
}

void CPU6502::cmd6502cli() {
  iflag = false;
  numofcycles += 2;
}

void CPU6502::cmd6502eorAbsoluteY() {
  uint16_t zold = z;
  modeAbsoluteY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502eorAbsoluteX() {
  uint16_t zold = z;
  modeAbsoluteX();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502lsrAbsoluteX() {
  modeAbsoluteX();
  lsrbase();
  numofcycles += 7;
}

void CPU6502::cmd6502rts() {
  uint8_t pcl = pullfromstack();
  uint8_t pch = pullfromstack();
  pc = (pcl + 1 + (pch << 8));
  numofcycles += 6;
}

void CPU6502::cmd6502adcIndirectX() {
  modeIndirectX();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 6;
}

void CPU6502::cmd6502adcZeropage() {
  modeZeropage();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 3;
}

void CPU6502::cmd6502rorZeropage() {
  modeZeropage();
  rorbase();
  numofcycles += 5;
}

void CPU6502::cmd6502pla() {
  a = pullfromstack();
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502adcImmediate() {
  uint8_t r = getMem(pc++);
  adcbase(r);
  numofcycles += 2;
}

void CPU6502::cmd6502rorA() {
  a = rorbase0(a);
  numofcycles += 2;
}

void CPU6502::cmd6502jmpIndirect() {
  modeAbsolute();
  uint8_t r1 = getMem(z);
  zl++;
  z = (zl + (zh << 8));
  uint8_t r2 = getMem(z);
  pc = (r1 + (r2 << 8));
  numofcycles += 5;
}

void CPU6502::cmd6502adcAbsolute() {
  modeAbsolute();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 4;
}

void CPU6502::cmd6502rorAbsolute() {
  modeAbsolute();
  rorbase();
  numofcycles += 6;
}

void CPU6502::cmd6502bvs() { branchbase(vflag); }

void CPU6502::cmd6502adcIndirectY() {
  uint16_t zold = z;
  modeIndirectY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 5;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502adcZeropageX() {
  modeZeropageX();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 4;
}

void CPU6502::cmd6502rorZeropageX() {
  modeZeropageX();
  rorbase();
  numofcycles += 6;
}

void CPU6502::cmd6502sei() {
  iflag = true;
  numofcycles += 2;
}

void CPU6502::cmd6502adcAbsoluteY() {
  uint16_t zold = z;
  modeAbsoluteY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502adcAbsoluteX() {
  uint16_t zold = z;
  modeAbsoluteX();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502rorAbsoluteX() {
  modeAbsoluteX();
  rorbase();
  numofcycles += 7;
}

void CPU6502::cmd6502staIndirectX() {
  modeIndirectX();
  setMem(z, a);
  numofcycles += 6;
}

void CPU6502::cmd6502styZeropage() {
  modeZeropage();
  setMem(z, y);
  numofcycles += 3;
}

void CPU6502::cmd6502staZeropage() {
  modeZeropage();
  setMem(z, a);
  numofcycles += 3;
}

void CPU6502::cmd6502stxZeropage() {
  modeZeropage();
  setMem(z, x);
  numofcycles += 3;
}

void CPU6502::cmd6502dey() {
  y--;
  setNZ(y);
  numofcycles += 2;
}

void CPU6502::cmd6502txa() {
  a = x;
  atestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502styAbsolute() {
  modeAbsolute();
  setMem(z, y);
  numofcycles += 4;
}

void CPU6502::cmd6502staAbsolute() {
  modeAbsolute();
  setMem(z, a);
  numofcycles += 4;
}

void CPU6502::cmd6502stxAbsolute() {
  modeAbsolute();
  setMem(z, x);
  numofcycles += 4;
}

void CPU6502::cmd6502bcc() { branchbase(!cflag); }

void CPU6502::cmd6502staIndirectY() {
  modeIndirectY();
  setMem(z, a);
  numofcycles += 6;
}

void CPU6502::cmd6502styZeropageX() {
  modeZeropageX();
  setMem(z, y);
  numofcycles += 4;
}

void CPU6502::cmd6502staZeropageX() {
  modeZeropageX();
  setMem(z, a);
  numofcycles += 4;
}

void CPU6502::cmd6502stxZeropageY() {
  modeZeropageY();
  setMem(z, x);
  numofcycles += 4;
}

void CPU6502::cmd6502tya() {
  a = y;
  atestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502staAbsoluteY() {
  modeAbsoluteY();
  setMem(z, a);
  numofcycles += 5;
}

void CPU6502::cmd6502txs() {
  sp = x;
  numofcycles += 2;
}

void CPU6502::cmd6502staAbsoluteX() {
  modeAbsoluteX();
  setMem(z, a);
  numofcycles += 5;
}

void CPU6502::cmd6502ldyImmediate() {
  y = getMem(pc++);
  ytestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502ldaIndirectX() {
  modeIndirectX();
  a = getMem(z);
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502laxIndirectX() {
  modeIndirectX();
  a = getMem(z);
  x = a;
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502ldxImmediate() {
  x = getMem(pc++);
  xtestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502ldyZeropage() {
  modeZeropage();
  y = getMem(z);
  ytestandsetNZ();
  numofcycles += 3;
}

void CPU6502::cmd6502ldaZeropage() {
  modeZeropage();
  a = getMem(z);
  atestandsetNZ();
  numofcycles += 3;
}

void CPU6502::cmd6502laxZeropage() {
  modeZeropage();
  a = getMem(z);
  x = a;
  atestandsetNZ();
  numofcycles += 3;
}

void CPU6502::cmd6502lxaImmediate() {
  a = getMem(pc++);
  x = a;
  atestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502ldxZeropage() {
  modeZeropage();
  x = getMem(z);
  xtestandsetNZ();
  numofcycles += 3;
}

void CPU6502::cmd6502tay() {
  y = a;
  ytestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502ldaImmediate() {
  a = getMem(pc++);
  atestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502tax() {
  x = a;
  xtestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502ldyAbsolute() {
  modeAbsolute();
  y = getMem(z);
  ytestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502ldaAbsolute() {
  modeAbsolute();
  a = getMem(z);
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502laxAbsolute() {
  modeAbsolute();
  a = getMem(z);
  x = a;
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502ldxAbsolute() {
  modeAbsolute();
  x = getMem(z);
  xtestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502bcs() { branchbase(cflag); }

void CPU6502::cmd6502ldaIndirectY() {
  uint16_t zold = z;
  modeIndirectY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  a = getMem(z);
  atestandsetNZ();
  numofcycles += 5;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502laxIndirectY() {
  modeIndirectY();
  a = getMem(z);
  x = a;
  atestandsetNZ();
  numofcycles += 5;
}

void CPU6502::cmd6502ldyZeropageX() {
  modeZeropageX();
  y = getMem(z);
  ytestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502ldaZeropageX() {
  modeZeropageX();
  a = getMem(z);
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502laxZeropageY() {
  modeZeropageX();
  a = getMem(z);
  x = a;
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502ldxZeropageY() {
  modeZeropageY();
  x = getMem(z);
  xtestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502clv() {
  vflag = false;
  numofcycles += 2;
}

void CPU6502::cmd6502ldaAbsoluteY() {
  uint16_t zold = z;
  modeAbsoluteY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  a = getMem(z);
  atestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502laxAbsoluteY() {
  uint16_t zold = z;
  modeAbsoluteY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  a = getMem(z);
  x = a;
  atestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502tsx() {
  x = sp;
  xtestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502ldyAbsoluteX() {
  uint16_t zold = z;
  modeAbsoluteX();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  y = getMem(z);
  ytestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502ldaAbsoluteX() {
  uint16_t zold = z;
  modeAbsoluteX();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  a = getMem(z);
  atestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502ldxAbsoluteY() {
  uint16_t zold = z;
  modeAbsoluteY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  x = getMem(z);
  xtestandsetNZ();
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502cpyImmediate() {
  uint8_t r = getMem(pc++);
  cmpbase(y, r);
  numofcycles += 2;
}

void CPU6502::cmd6502cmpIndirectX() {
  modeIndirectX();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 6;
}

void CPU6502::cmd6502cpyZeropage() {
  modeZeropage();
  uint8_t r = getMem(z);
  cmpbase(y, r);
  numofcycles += 3;
}

void CPU6502::cmd6502cmpZeropage() {
  modeZeropage();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 3;
}

void CPU6502::cmd6502decZeropage() {
  modeZeropage();
  decbase();
  numofcycles += 5;
}

void CPU6502::cmd6502iny() {
  y++;
  setNZ(y);
  numofcycles += 2;
}

void CPU6502::cmd6502cmpImmediate() {
  uint8_t r = getMem(pc++);
  cmpbase(a, r);
  numofcycles += 2;
}

void CPU6502::cmd6502dex() {
  x--;
  setNZ(x);
  numofcycles += 2;
}

void CPU6502::cmd6502cpyAbsolute() {
  modeAbsolute();
  uint8_t r = getMem(z);
  cmpbase(y, r);
  numofcycles += 4;
}

void CPU6502::cmd6502cmpAbsolute() {
  modeAbsolute();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 4;
}

void CPU6502::cmd6502decAbsolute() {
  modeAbsolute();
  decbase();
  numofcycles += 6;
}

void CPU6502::cmd6502bne() { branchbase(!zflag); }

void CPU6502::cmd6502cmpIndirectY() {
  uint16_t zold = z;
  modeIndirectY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 5;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502cmpZeropageX() {
  modeZeropageX();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 4;
}

void CPU6502::cmd6502decZeropageX() {
  modeZeropageX();
  decbase();
  numofcycles += 6;
}

void CPU6502::cmd6502cld() {
  dflag = false;
  numofcycles += 2;
}

void CPU6502::cmd6502cmpAbsoluteY() {
  uint16_t zold = z;
  modeAbsoluteY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502cmpAbsoluteX() {
  uint16_t zold = z;
  modeAbsoluteX();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502decAbsoluteX() {
  modeAbsoluteX();
  decbase();
  numofcycles += 7;
}

void CPU6502::cmd6502cpxImmediate() {
  uint8_t r = getMem(pc++);
  cmpbase(x, r);
  numofcycles += 2;
}

void CPU6502::cmd6502sbcIndirectX() {
  modeIndirectX();
  uint8_t r = getMem(z);
  sbcbase(r);
  numofcycles += 6;
}

void CPU6502::cmd6502cpxZeropage() {
  modeZeropage();
  uint8_t r = getMem(z);
  cmpbase(x, r);
  numofcycles += 3;
}

void CPU6502::cmd6502sbcZeropage() {
  modeZeropage();
  uint8_t r = getMem(z);
  sbcbase(r);
  numofcycles += 3;
}

void CPU6502::cmd6502incZeropage() {
  modeZeropage();
  incbase();
  numofcycles += 5;
}

void CPU6502::cmd6502inx() {
  x++;
  setNZ(x);
  numofcycles += 2;
}

void CPU6502::cmd6502sbcImmediate() {
  uint8_t r = getMem(pc++);
  sbcbase(r);
  numofcycles += 2;
}

void CPU6502::cmd6502nop() { numofcycles += 2; }

void CPU6502::cmd6502cpxAbsolute() {
  modeAbsolute();
  uint8_t r = getMem(z);
  cmpbase(x, r);
  numofcycles += 4;
}

void CPU6502::cmd6502sbcAbsolute() {
  modeAbsolute();
  uint8_t r = getMem(z);
  sbcbase(r);
  numofcycles += 4;
}

void CPU6502::cmd6502incAbsolute() {
  modeAbsolute();
  incbase();
  numofcycles += 6;
}

void CPU6502::cmd6502beq() { branchbase(zflag); }

void CPU6502::cmd6502sbcIndirectY() {
  uint16_t zold = z;
  modeIndirectY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  sbcbase(r);
  numofcycles += 6;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502sbcZeropageX() {
  modeZeropageX();
  uint8_t r = getMem(z);
  sbcbase(r);
  numofcycles += 4;
}

void CPU6502::cmd6502incZeropageX() {
  modeZeropageX();
  incbase();
  numofcycles += 6;
}

void CPU6502::cmd6502sed() {
  dflag = true;
  numofcycles += 2;
}

void CPU6502::cmd6502sbcAbsoluteY() {
  uint16_t zold = z;
  modeAbsoluteY();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  sbcbase(r);
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502sbcAbsoluteX() {
  uint16_t zold = z;
  modeAbsoluteX();
  bool pageCrossed = ((zold & 0xFF00) != (z & 0xFF00));
  uint8_t r = getMem(z);
  sbcbase(r);
  numofcycles += 4;
  if (pageCrossed) {
    numofcycles++;
  }
}

void CPU6502::cmd6502incAbsoluteX() {
  modeAbsoluteX();
  incbase();
  numofcycles += 7;
}

void CPU6502::cmd6502nopImmediate() {
  getMem(pc++);
  numofcycles += 2;
}

void CPU6502::cmd6502nopZeropage() {
  modeZeropage();
  numofcycles += 3;
}

void CPU6502::cmd6502nopZeropageX() {
  modeZeropageX();
  numofcycles += 4;
}

void CPU6502::cmd6502nop1a() { numofcycles += 2; }

void CPU6502::cmd6502nop3a() { numofcycles += 2; }

void CPU6502::cmd6502nop5a() { numofcycles += 2; }

void CPU6502::cmd6502nop7a() { numofcycles += 2; }

void CPU6502::cmd6502nopda() { numofcycles += 2; }

void CPU6502::cmd6502nopfa() { numofcycles += 2; }

void CPU6502::cmd6502alrImmediate() {
  uint8_t r = getMem(pc++);
  a &= r;
  a = lsrbase0(a);
  numofcycles += 2;
}

void CPU6502::cmd6502saxZeropage() {
  modeZeropage();
  setMem(z, a & x);
  numofcycles += 3;
}

void CPU6502::cmd6502saxZeropageY() {
  modeZeropageY();
  setMem(z, a & x);
  numofcycles += 4;
}

void CPU6502::cmd6502saxAbsolute() {
  modeAbsolute();
  setMem(z, a & x);
  numofcycles += 4;
}

void CPU6502::cmd6502saxIndirectX() {
  modeIndirectX();
  setMem(z, a & x);
  numofcycles += 6;
}

uint8_t CPU6502::isbincbase() {
  uint8_t r = getMem(z);
  r++;
  setMem(z, r);
  return r;
}

void CPU6502::cmd6502isbZeropage() {
  modeZeropage();
  uint8_t r = isbincbase();
  sbcbase(r);
  numofcycles += 5;
}

void CPU6502::cmd6502isbZeropageX() {
  modeZeropageX();
  uint8_t r = isbincbase();
  sbcbase(r);
  numofcycles += 6;
}

void CPU6502::cmd6502isbIndirectX() {
  modeIndirectX();
  uint8_t r = isbincbase();
  sbcbase(r);
  numofcycles += 8;
}

void CPU6502::cmd6502isbIndirectY() {
  modeIndirectY();
  uint8_t r = isbincbase();
  sbcbase(r);
  numofcycles += 8;
}

void CPU6502::cmd6502isbAbsolute() {
  modeAbsolute();
  uint8_t r = isbincbase();
  sbcbase(r);
  numofcycles += 6;
}

void CPU6502::cmd6502isbAbsoluteX() {
  modeAbsoluteX();
  uint8_t r = isbincbase();
  sbcbase(r);
  numofcycles += 7;
}

void CPU6502::cmd6502isbAbsoluteY() {
  modeAbsoluteY();
  uint8_t r = isbincbase();
  sbcbase(r);
  numofcycles += 7;
}

void CPU6502::cmd6502skwAbsolute() {
  modeAbsolute();
  numofcycles += 4;
}

void CPU6502::cmd6502skwAbsoluteX() {
  modeAbsoluteX();
  numofcycles += 4;
}

void CPU6502::cmd6502shaZeropageY() {
  modeZeropageY();
  uint8_t r = a & x & zh;
  setMem(z, r);
  numofcycles += 6;
}

void CPU6502::cmd6502shaAbsoluteY() {
  modeAbsoluteY();
  uint8_t r = a & x & zh;
  setMem(z, r);
  numofcycles += 5;
}

void CPU6502::cmd6502shxAbsoluteY() {
  modeAbsoluteY();
  uint8_t r = x & zh;
  setMem(z, r);
  numofcycles += 5;
}

void CPU6502::cmd6502rraZeropage() {
  modeZeropage();
  rorbase();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 5;
}

void CPU6502::cmd6502rraZeropageX() {
  modeZeropageX();
  rorbase();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 6;
}

void CPU6502::cmd6502rraIndirectX() {
  modeIndirectX();
  rorbase();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 8;
}

void CPU6502::cmd6502rraIndirectY() {
  modeIndirectY();
  rorbase();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 8;
}

void CPU6502::cmd6502rraAbsolute() {
  modeAbsolute();
  rorbase();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 6;
}

void CPU6502::cmd6502rraAbsoluteX() {
  modeAbsoluteX();
  rorbase();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 7;
}

void CPU6502::cmd6502rraAbsoluteY() {
  modeAbsoluteY();
  rorbase();
  uint8_t r = getMem(z);
  adcbase(r);
  numofcycles += 7;
}

void CPU6502::cmd6502asoZeropage() {
  modeZeropage();
  aslbase();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 5;
}

void CPU6502::cmd6502asoZeropageX() {
  modeZeropageX();
  aslbase();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502asoIndirectX() {
  modeIndirectX();
  aslbase();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 8;
}

void CPU6502::cmd6502asoIndirectY() {
  modeIndirectY();
  aslbase();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 8;
}

void CPU6502::cmd6502asoAbsolute() {
  modeAbsolute();
  aslbase();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502asoAbsoluteX() {
  modeAbsoluteX();
  aslbase();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 7;
}

void CPU6502::cmd6502asoAbsoluteY() {
  modeAbsoluteY();
  aslbase();
  uint8_t r = getMem(z);
  a |= r;
  atestandsetNZ();
  numofcycles += 7;
}

void CPU6502::cmd6502sreZeropage() {
  modeZeropage();
  lsrbase();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 5;
}

void CPU6502::cmd6502sreZeropageX() {
  modeZeropageX();
  lsrbase();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502sreIndirectX() {
  modeIndirectX();
  lsrbase();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 8;
}

void CPU6502::cmd6502sreIndirectY() {
  modeIndirectY();
  lsrbase();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 8;
}

void CPU6502::cmd6502sreAbsolute() {
  modeAbsolute();
  lsrbase();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502sreAbsoluteX() {
  modeAbsoluteX();
  lsrbase();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 7;
}

void CPU6502::cmd6502sreAbsoluteY() {
  modeAbsoluteY();
  lsrbase();
  uint8_t r = getMem(z);
  a ^= r;
  atestandsetNZ();
  numofcycles += 7;
}

void CPU6502::cmd6502dcpZeropage() {
  modeZeropage();
  decbase();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 5;
}

void CPU6502::cmd6502dcpZeropageX() {
  modeZeropageX();
  decbase();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 6;
}

void CPU6502::cmd6502dcpIndirectX() {
  modeIndirectX();
  decbase();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 8;
}

void CPU6502::cmd6502dcpIndirectY() {
  modeIndirectY();
  decbase();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 8;
}

void CPU6502::cmd6502dcpAbsolute() {
  modeAbsolute();
  decbase();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 6;
}

void CPU6502::cmd6502dcpAbsoluteX() {
  modeAbsoluteX();
  decbase();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 7;
}

void CPU6502::cmd6502dcpAbsoluteY() {
  modeAbsoluteY();
  decbase();
  uint8_t r = getMem(z);
  cmpbase(a, r);
  numofcycles += 7;
}

void CPU6502::cmd6502xaaImmediate() {
  uint8_t r = getMem(pc++);
  a = (a | 0xfe) & x & r;
  atestandsetNZ();
  numofcycles += 2;
}

void CPU6502::cmd6502sbxImmediate() {
  uint8_t r = getMem(pc++);
  x = (a & x) - r;
  cmpbase(a & x, r);
  numofcycles += 2;
}

void CPU6502::cmd6502lasAbsolute() {
  x = sp;
  a = x;
  modeAbsoluteY();
  uint8_t r = getMem(z);
  a &= r;
  x = a;
  sp = x;
  atestandsetNZ();
  numofcycles += 4;
}

void CPU6502::cmd6502rlaZeropage() {
  modeZeropage();
  rolbase();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 5;
}

void CPU6502::cmd6502rlaZeropageX() {
  modeZeropageX();
  rolbase();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502rlaIndirectX() {
  modeIndirectX();
  rolbase();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 8;
}

void CPU6502::cmd6502rlaIndirectY() {
  modeIndirectY();
  rolbase();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 8;
}

void CPU6502::cmd6502rlaAbsolute() {
  modeAbsolute();
  rolbase();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 6;
}

void CPU6502::cmd6502rlaAbsoluteX() {
  modeAbsoluteX();
  rolbase();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 7;
}

void CPU6502::cmd6502rlaAbsoluteY() {
  modeAbsoluteY();
  rolbase();
  uint8_t r = getMem(z);
  a &= r;
  atestandsetNZ();
  numofcycles += 7;
}

void CPU6502::cmd6502tas() {
  uint8_t r = a & x;
  sp = r;
  modeAbsoluteY();
  r &= zh + 1;
  setMem(z, r);
  numofcycles += 5;
}

void CPU6502::cmd6502arr() {
  uint8_t r = getMem(pc++);
  bool oricflag = cflag;
  a &= r;
  cflag = a & 128;
  vflag = ((a & 128) >> 1) ^ (a & 64);
  uint16_t r1 = a;
  if (oricflag) {
    r1 |= 0x100;
  }
  r1 >>= 1;
  setNZ(r1);
  a = r1;
  numofcycles += 2;
}

void CPU6502::cmd6502shy() {
  modeAbsoluteX();
  uint8_t r = y & (zh + 1);
  setMem(z, r);
  numofcycles += 5;
}

void CPU6502::execute(uint8_t idx) { (this->*cmdarr6502[idx])(); }

void CPU6502::setPCToIntVec(uint16_t intvect, bool intfrombrk) {
  // push actual address to 6502 stack
  uint8_t pcl = pc & 0xFF;
  uint8_t pch = (pc >> 8);
  pushtostack(pch);
  pushtostack(pcl);
  // push state to 6502 stack
  bflag = intfrombrk;
  php();
  // clear d-flag
  dflag = false;
  // set interrupt disable flag
  iflag = true;
  // set pc
  pc = intvect;
  numofcycles += 7;
}
