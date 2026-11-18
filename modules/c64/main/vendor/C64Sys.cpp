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
// SolarOS port: caller-owned state and bounded raster execution.
#include "C64Sys.h"
#include <cstring>

uint8_t C64Sys::getDC01(uint8_t selected, bool reverse) {
  uint8_t result = 0xff;
  for (unsigned row = 0; row < 8; ++row) {
    if (!reverse) {
      if (!(selected & (1U << row))) result &= matrix[row];
    } else {
      for (unsigned col = 0; col < 8; ++col)
        if (!(selected & (1U << col)) && !(matrix[row] & (1U << col)))
          result &= ~(1U << row);
    }
  }
  return result;
}

uint8_t C64Sys::getMem(uint16_t addr) {
  if (addr < 0xa000) {
    if (addr == 1) return (register1 & ram[0]) | (uint8_t)~ram[0];
    return ram[addr];
  }
  if ((!bankARAM) && ((addr >= 0xa000) && (addr <= 0xbfff))) {
    //    basic rom
    return basic_rom[addr - 0xa000];
  } else if ((!bankERAM) && (addr >= 0xe000)) {
    // kernal rom
    return kernal_rom[addr - 0xe000];
  } else if (bankDIO && (addr >= 0xd000) && (addr <= 0xdfff)) {
    // ** VIC **
    if (addr <= 0xd3ff) {
      uint8_t vicidx = (addr - 0xd000) % 0x40;
      if ((vicidx == 0x1e) || (vicidx == 0x1f)) {
        uint8_t val = vic.vicreg[vicidx];
        vic.vicreg[vicidx] = 0;
        return val;
      } else if (vicidx == 0x11) {
        uint8_t raster8 = (vic.rasterline >= 256) ? 0x80 : 0;
        return (vic.vicreg[0x11] & 0x7f) | raster8;
      } else {
        return vic.vicreg[vicidx];
      }
    }
    // ** SID **
    else if (addr <= 0xd7ff) {
      uint8_t sididx = (addr - 0xd400) % 0x20;
      if (sididx == 0x1b) {
        random_state ^= random_state << 13;
        random_state ^= random_state >> 17;
        random_state ^= random_state << 5;
        return random_state;
      } else if (sididx == 0x1c) {
        return static_cast<uint8_t>(sid.sidVoice[2].envelope * 255.0f);
      } else {
        return sid.sidreg[sididx];
      }
    }
    // ** Colorram **
    else if (addr <= 0xdbff) {
      return vic.colormap[addr - 0xd800];
    }
    // ** CIA 1 **
    else if (addr <= 0xdcff) {
      uint8_t ciaidx = (addr - 0xdc00) % 0x10;
      if (ciaidx == 0x00) {
        uint8_t ddra = cia1.ciareg[0x02];
        uint8_t input = getDC01(cia1.ciareg[0x01] | ~cia1.ciareg[0x03], true);
        input &= joystick[1];
        return (cia1.ciareg[0x00] | ~ddra) & input;
      } else if (ciaidx == 0x01) {
        uint8_t ddrb = cia1.ciareg[0x03];
        uint8_t input = getDC01(cia1.ciareg[0x00] | ~cia1.ciareg[0x02], false);
        input &= joystick[0];
        return (cia1.ciareg[0x01] | ~ddrb) & input;
      }
      return cia1.getCommonCIAReg(ciaidx);
    }
    // ** CIA 2 **
    else if (addr <= 0xddff) {
      uint8_t ciaidx = (addr - 0xdd00) % 0x10;
      if (ciaidx == 0x00) {
        uint8_t ddra = cia2.ciareg[0x02];
        return cia2.ciareg[0x00] | ~ddra;
      } else if (ciaidx == 0x01) {
        uint8_t ddrb = cia2.ciareg[0x03];
        return cia2.ciareg[0x01] | ~ddrb;
      } else if (ciaidx == 0x0d) {
        nmiAck = true;
      }
      return cia2.getCommonCIAReg(ciaidx);
    }
  } else if ((!bankDRAM) && (addr >= 0xd000) && (addr <= 0xdfff)) {
    // dxxx character rom
    return charrom[addr - 0xd000];
  } else if (addr == 0x0001) {
    return (register1 & ram[0]) | (uint8_t)~ram[0];
  }
  // ram
  return ram[addr];
}

void C64Sys::decodeRegister1(uint8_t val) {
  switch (val) {
  case 0:
  case 4:
    bankARAM = true;
    bankDRAM = true;
    bankERAM = true;
    bankDIO = false;
    break;
  case 1:
    bankARAM = true;
    bankDRAM = false;
    bankERAM = true;
    bankDIO = false;
    break;
  case 2:
    bankARAM = true;
    bankDRAM = false;
    bankERAM = false;
    bankDIO = false;
    break;
  case 3:
    bankARAM = false;
    bankDRAM = false;
    bankERAM = false;
    bankDIO = false;
    break;
  case 5:
    bankARAM = true;
    bankDRAM = true;
    bankERAM = true;
    bankDIO = true;
    break;
  case 6:
    bankARAM = true;
    bankDRAM = true;
    bankERAM = false;
    bankDIO = true;
    break;
  case 7:
    bankARAM = false;
    bankDRAM = true;
    bankERAM = false;
    bankDIO = true;
    break;
  }
}

void C64Sys::adaptVICBaseAddrs(bool fromcia) {
  uint8_t val = vic.vicreg[0x18];
  uint16_t val1 = (val & 0xf0) << 6;
  uint16_t vicmem = vic.vicmem;
  // screenmem is used for text mode and bitmap mode
  vic.screenmemstart = vicmem + val1;
  vic.bitmapstart = vicmem + ((val & 0x0e) << 10);
  bool bmm = vic.vicreg[0x11] & 32;
  if ((!bmm) || fromcia) {
    // charactermem
    val1 = (val & 0x0e) << 10;
    uint16_t charmemstart = vicmem + val1;
    if ((charmemstart == 0x1800) || (charmemstart == 0x9800)) {
      vic.charset = vic.chrom + 0x0800;
    } else if ((charmemstart == 0x1000) || (charmemstart == 0x9000)) {
      vic.charset = vic.chrom;
    } else {
      vic.charset = ram + charmemstart;
    }
  }
}

void C64Sys::setMem(uint16_t addr, uint8_t val) {
  if (bankDIO && (addr >= 0xd000) && (addr <= 0xdfff)) {
    // ** VIC **
    if (addr <= 0xd3ff) {
      uint8_t vicidx = (addr - 0xd000) % 0x40;
      if (vicidx == 0x11) {
        // only bit 7 of latch register d011 is used
        vic.latchd011 = val;
        vic.vicreg[vicidx] = val & 0x7f;
        adaptVICBaseAddrs(false);
      } else if (vicidx == 0x12) {
        vic.latchd012 = val;
      } else if (vicidx == 0x16) {
        vic.vicreg[vicidx] = val;
        adaptVICBaseAddrs(false);
      } else if (vicidx == 0x18) {
        vic.vicreg[vicidx] = val;
        adaptVICBaseAddrs(false);
      } else if (vicidx == 0x19) {
        // clear given bits
        /*
        // does not work for all games, e.g. bubble bobble
        uint8_t act = vic.vicreg[vicidx];
        act &= ~val;
        if ((act & 7) == 0) {
          act &= ~0x80;
        }
        vic.vicreg[vicidx] = val;
        */
        vic.vicreg[vicidx] = 0;
      } else if ((vicidx == 0x1e) || (vicidx == 0x1f)) {
        vic.vicreg[vicidx] = 0;
      } else {
        vic.vicreg[vicidx] = vicidx >= 0x20 && vicidx <= 0x2e ? val & 15 : val;
      }
    }
    // ** SID **
    else if (addr <= 0xd7ff) {
      uint8_t sididx = (addr - 0xd400) % 0x20;
      sid.sidreg[sididx] = val;
      if (sididx <= 0x14) {
        uint8_t voice = sididx / 7;
        int regInVoice = sididx % 7;
        switch (regInVoice) {
        case 0:
        case 1:
          sid.sidVoice[voice].updVarFrequency(sid.sidreg[voice * 7] |
                                              (sid.sidreg[1 + voice * 7] << 8));
          break;
        case 2:
        case 3:
          sid.sidVoice[voice].updVarPulseWidth(
              sid.sidreg[2 + voice * 7] | (sid.sidreg[3 + voice * 7] << 8));
          break;
        case 4:
          sid.sidVoice[voice].updVarControl(sid.sidreg[sididx]);
          break;
        case 5:
          sid.sidVoice[voice].updVarEnvelopeAD(sid.sidreg[sididx]);
          break;
        case 6:
          sid.sidVoice[voice].updVarEnvelopeSR(sid.sidreg[sididx]);
          break;
        }
      } else if (sididx == 0x18) {
        sid.c64Volume = (float)(val & 0x0f) / 15.0;
      }
    }
    // ** Colorram **
    else if (addr <= 0xdbff) {
      vic.colormap[addr - 0xd800] = val & 15;
    }
    // ** CIA 1 **
    else if (addr <= 0xdcff) {
      uint8_t ciaidx = (addr - 0xdc00) % 0x10;
      if (ciaidx == 0x00) {
        uint8_t ddra = cia1.ciareg[0x02];
        cia1.ciareg[ciaidx] = (cia1.ciareg[ciaidx] & ~ddra) | (val & ddra);
      } else if (ciaidx == 0x01) {
        uint8_t ddrb = cia1.ciareg[0x03];
        cia1.ciareg[ciaidx] = (cia1.ciareg[ciaidx] & ~ddrb) | (val & ddrb);
      } else {
        cia1.setCommonCIAReg(ciaidx, val);
      }
    }
    // ** CIA 2 **
    else if (addr <= 0xddff) {
      uint8_t ciaidx = (addr - 0xdd00) % 0x10;
      if (ciaidx == 0x00) {
        uint8_t bank = val & 3;
        switch (bank) {
        case 0:
          vic.vicmem = 0xc000;
          break;
        case 1:
          vic.vicmem = 0x8000;
          break;
        case 2:
          vic.vicmem = 0x4000;
          break;
        case 3:
          vic.vicmem = 0x0000;
          break;
        }
        cia2.ciareg[ciaidx] = 0x94 | bank;
        // adapt VIC base addresses
        adaptVICBaseAddrs(true);
      } else {
        cia2.setCommonCIAReg(ciaidx, val);
      }
    }
  }
  // ** register 1 **
  else if (addr == 0x0001) {
    register1 = ram[1] = val;
    decodeRegister1(((register1 & ram[0]) | (uint8_t)~ram[0]) & 7);
  }
  else if (addr == 0) {
    ram[0] = val;
    decodeRegister1(((register1 & ram[0]) | (uint8_t)~ram[0]) & 7);
  }
  // ** ram **
  else {
    ram[addr] = val;
  }
}

void C64Sys::checkciatimers(uint8_t cycles) {
  // CIA 1 TOD alarm
  cia1.checkAlarm();
  // check for CIA 1 TOD alarm interrupt
  if ((cia1.latchdc0d & 0x04) && (cia1.ciareg[0x0d] & 0x04) && (!iflag)) {
    setPCToIntVec(getMem(0xfffe) + (getMem(0xffff) << 8), false);
  }
  // CIA 1 Timer A
  cia1.checkTimerA(cycles);
  // check for CIA 1 Timer A interrupt
  if ((cia1.latchdc0d & 0x01) && (cia1.ciareg[0x0d] & 0x01) && (!iflag)) {
    setPCToIntVec(getMem(0xfffe) + (getMem(0xffff) << 8), false);
  }
  // CIA 1 Timer B
  cia1.checkTimerB(cycles);
  // check for CIA 1 Timer B interrupt
  if ((cia1.latchdc0d & 0x02) && (cia1.ciareg[0x0d] & 0x02) && (!iflag)) {
    setPCToIntVec(getMem(0xfffe) + (getMem(0xffff) << 8), false);
  }
  // check for CIA 1 SDR interrupt
  if ((cia1.latchdc0d & 0x08) && (cia1.ciareg[0x0d] & 0x08) && (!iflag)) {
    setPCToIntVec(getMem(0xfffe) + (getMem(0xffff) << 8), false);
  }
  // CIA 2 TOD alarm
  cia2.checkAlarm();
  // check for CIA 2 TOD alarm interrupt
  if ((cia2.latchdc0d & 0x04) && (cia2.ciareg[0x0d] & 0x04) && nmiAck) {
    nmiAck = false;
    setPCToIntVec(getMem(0xfffa) + (getMem(0xfffb) << 8), false);
  }
  // CIA 2 Timer A
  cia2.checkTimerA(cycles);
  // check for CIA 2 Timer A interrupt
  if ((cia2.latchdc0d & 0x01) && (cia2.ciareg[0x0d] & 0x01) && nmiAck) {
    nmiAck = false;
    setPCToIntVec(getMem(0xfffa) + (getMem(0xfffb) << 8), false);
  }
  // CIA 2 Timer B
  cia2.checkTimerB(cycles);
  // check for CIA 2 Timer B interrupt
  if ((cia2.latchdc0d & 0x02) && (cia2.ciareg[0x0d] & 0x02) && nmiAck) {
    nmiAck = false;
    setPCToIntVec(getMem(0xfffa) + (getMem(0xfffb) << 8), false);
  }
  // check for CIA 2 SDR interrupt
  if ((cia2.latchdc0d & 0x08) && (cia2.ciareg[0x0d] & 0x08) && nmiAck) {
    nmiAck = false;
    setPCToIntVec(getMem(0xfffa) + (getMem(0xfffb) << 8), false);
  }
}

void C64Sys::stepRaster() {
  if (cpuhalted) return;
  // Preserve upstream's zero bad-line penalty for game compatibility.
  vic.nextRasterline();
  numofcycles = 0;
  int8_t numofcyclestoexe = 63 - adjustcycles;
  if (numofcyclestoexe < 0) numofcyclestoexe = 0;

  // Execute the two half-lines around VIC rendering, as in upstream.
  while (numofcycles < numofcyclestoexe / 2 && !cpuhalted) {
    execute(getMem(pc++));
    if ((vic.vicreg[0x19] & 0x81) && (vic.vicreg[0x1a] & 1) && !iflag) {
      setPCToIntVec(getMem(0xfffe) + (getMem(0xffff) << 8), false);
    }
  }
  checkciatimers(31);
  vic.drawRasterline();

  while (numofcycles < numofcyclestoexe && !cpuhalted) {
    execute(getMem(pc++));
    if ((vic.vicreg[0x19] & 0x81) && (vic.vicreg[0x1a] & 1) && !iflag) {
      setPCToIntVec(getMem(0xfffe) + (getMem(0xffff) << 8), false);
    }
  }
  checkciatimers(32);
  adjustcycles = numofcycles - numofcyclestoexe;
  if (adjustcycles > 0) adjustcycles = 0;

  if ((vic.vicreg[0x19] & 0x86) && (vic.vicreg[0x1a] & 6) && !iflag) {
    setPCToIntVec(getMem(0xfffe) + (getMem(0xffff) << 8), false);
  }
  if (restorenmi && nmiAck) {
    nmiAck = false;
    restorenmi = false;
    setPCToIntVec(getMem(0xfffa) + (getMem(0xfffb) << 8), false);
  }
  sid.fillBuffer(vic.rasterline);
}

void C64Sys::reset() {
  memset(ram, 0, 65536);
  memset(vic.colormap, 0, 1024);
  memset(vic.getBitmap(), 0, 320 * 200);
  memset(matrix, 0xff, sizeof(matrix));
  joystick[0] = joystick[1] = 0xff;
  cia1.init(true); cia2.init(false);
  vic.initVarsAndRegs();
  vic.rasterline = 311; // first slice is raster zero
  sid.initSID();
  setMem(0, 0x2f); setMem(1, 0x37);
  a = x = y = sr = 0; sp = 0xff;
  cflag = zflag = dflag = bflag = vflag = nflag = false;
  iflag = nmiAck = true; restorenmi = cpuhalted = false;
  numofcycles = adjustcycles = 0;
  pc = kernal_rom[0x1ffc] | (kernal_rom[0x1ffd] << 8);
}
