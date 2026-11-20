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
#pragma once
#include "CPU6502.h"
#include "CIA.h"
#include "VIC.h"
#include "SID.h"

class C64Sys final : public CPU6502 {
  bool bankARAM, bankDRAM, bankERAM, bankDIO, nmiAck;
  uint8_t register1, adjustcycles;
  uint32_t random_state = 0x12345678;
  uint8_t getDC01(uint8_t selected, bool reverse);
  void adaptVICBaseAddrs(bool fromcia);
  void decodeRegister1(uint8_t val);
  void checkciatimers(uint8_t cycles);
public:
  uint8_t *ram;
  const uint8_t *basic_rom, *kernal_rom, *charrom;
  uint8_t matrix[8], joystick[2];
  VIC vic;
  CIA cia1{true}, cia2{false};
  SID sid;
  bool restorenmi;
  uint8_t getMem(uint16_t addr);
  void setMem(uint16_t addr, uint8_t value);
  void run() override { stepRaster(); }
  void stepRaster();
  void reset();
  uint16_t getPC() const { return pc; }
  void setPC(uint16_t value) { pc = value; }
};
