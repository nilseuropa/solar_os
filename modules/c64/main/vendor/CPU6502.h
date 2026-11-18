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
#ifndef CPU6502_H
#define CPU6502_H

#include <cstdint>

class CPU6502 {
private:
  uint8_t zl;
  uint8_t zh;
  uint16_t z;

  inline void modeZeropage() __attribute__((always_inline));
  inline void modeZeropageX() __attribute__((always_inline));
  inline void modeZeropageY() __attribute__((always_inline));
  inline void modeAbsolute() __attribute__((always_inline));
  inline void modeAbsoluteX() __attribute__((always_inline));
  inline void modeAbsoluteY() __attribute__((always_inline));
  inline void modeIndirectX() __attribute__((always_inline));
  inline void modeIndirectY() __attribute__((always_inline));
  inline void setNZ(uint8_t r) __attribute__((always_inline));
  inline void atestandsetNZ() __attribute__((always_inline));
  inline void xtestandsetNZ() __attribute__((always_inline));
  inline void ytestandsetNZ() __attribute__((always_inline));
  inline void branchbase(bool flag) __attribute__((always_inline));
  inline void adcbase(uint8_t r) __attribute__((always_inline));
  inline void sbcbase(uint8_t r) __attribute__((always_inline));
  inline void incbase() __attribute__((always_inline));
  inline void decbase() __attribute__((always_inline));
  inline void cmpbase(uint8_t r1, uint8_t r2) __attribute__((always_inline));
  inline uint8_t aslbase0(uint8_t r) __attribute__((always_inline));
  inline void aslbase() __attribute__((always_inline));
  inline uint8_t lsrbase0(uint8_t r) __attribute__((always_inline));
  inline void lsrbase() __attribute__((always_inline));
  inline uint8_t rolbase0(uint8_t r) __attribute__((always_inline));
  inline void rolbase() __attribute__((always_inline));
  inline uint8_t rorbase0(uint8_t r) __attribute__((always_inline));
  inline void rorbase() __attribute__((always_inline));
  inline void bitBase() __attribute__((always_inline));
  inline void srfromflags();
  inline void flagsfromsr();
  inline void pushtostack(uint8_t r);
  inline uint8_t pullfromstack();
  void cmd6502oraIndirectX();
  void cmd6502oraZeropage();
  void cmd6502aslZeropage();
  inline void php();
  void cmd6502php();
  void cmd6502oraImmediate();
  void cmd6502aslA();
  void cmd6502oraAbsolute();
  void cmd6502aslAbsolute();
  void cmd6502shell();
  void cmd6502bpl();
  void cmd6502oraIndirectY();
  void cmd6502oraZeropageX();
  void cmd6502aslZeropageX();
  void cmd6502clc();
  void cmd6502oraAbsoluteY();
  void cmd6502oraAbsoluteX();
  void cmd6502aslAbsoluteX();
  void cmd6502jsr();
  void cmd6502andIndirectX();
  void cmd6502bitZeropage();
  void cmd6502andZeropage();
  void cmd6502rolZeropage();
  inline void plp();
  void cmd6502plp();
  void cmd6502andImmediate();
  void cmd6502rolA();
  void cmd6502bitAbsolute();
  void cmd6502andAbsolute();
  void cmd6502rolAbsolute();
  void cmd6502bmi();
  void cmd6502andIndirectY();
  void cmd6502andZeropageX();
  void cmd6502rolZeropageX();
  void cmd6502sec();
  void cmd6502andAbsoluteY();
  void cmd6502andAbsoluteX();
  void cmd6502rolAbsoluteX();
  void cmd6502rti();
  void cmd6502eorIndirectX();
  void cmd6502eorZeropage();
  void cmd6502lsrZeropage();
  void cmd6502pha();
  void cmd6502eorImmediate();
  void cmd6502lsrA();
  void cmd6502jmpAbsolute();
  void cmd6502eorAbsolute();
  void cmd6502lsrAbsolute();
  void cmd6502bvc();
  void cmd6502eorIndirectY();
  void cmd6502eorZeropageX();
  void cmd6502lsrZeropageX();
  void cmd6502cli();
  void cmd6502eorAbsoluteY();
  void cmd6502eorAbsoluteX();
  void cmd6502lsrAbsoluteX();
  void cmd6502adcIndirectX();
  void cmd6502adcZeropage();
  void cmd6502rorZeropage();
  void cmd6502pla();
  void cmd6502adcImmediate();
  void cmd6502rorA();
  void cmd6502jmpIndirect();
  void cmd6502adcAbsolute();
  void cmd6502rorAbsolute();
  void cmd6502bvs();
  void cmd6502adcIndirectY();
  void cmd6502adcZeropageX();
  void cmd6502rorZeropageX();
  void cmd6502sei();
  void cmd6502adcAbsoluteY();
  void cmd6502adcAbsoluteX();
  void cmd6502rorAbsoluteX();
  void cmd6502staIndirectX();
  void cmd6502styZeropage();
  void cmd6502staZeropage();
  void cmd6502stxZeropage();
  void cmd6502dey();
  void cmd6502txa();
  void cmd6502styAbsolute();
  void cmd6502staAbsolute();
  void cmd6502stxAbsolute();
  void cmd6502bcc();
  void cmd6502staIndirectY();
  void cmd6502styZeropageX();
  void cmd6502staZeropageX();
  void cmd6502stxZeropageY();
  void cmd6502tya();
  void cmd6502staAbsoluteY();
  void cmd6502txs();
  void cmd6502staAbsoluteX();
  void cmd6502ldyImmediate();
  void cmd6502ldaIndirectX();
  void cmd6502ldxImmediate();
  void cmd6502ldyZeropage();
  void cmd6502ldaZeropage();
  void cmd6502ldxZeropage();
  void cmd6502tay();
  void cmd6502ldaImmediate();
  void cmd6502tax();
  void cmd6502ldyAbsolute();
  void cmd6502ldaAbsolute();
  void cmd6502ldxAbsolute();
  void cmd6502bcs();
  void cmd6502ldaIndirectY();
  void cmd6502ldyZeropageX();
  void cmd6502ldaZeropageX();
  void cmd6502ldxZeropageY();
  void cmd6502clv();
  void cmd6502ldaAbsoluteY();
  void cmd6502tsx();
  void cmd6502ldyAbsoluteX();
  void cmd6502ldaAbsoluteX();
  void cmd6502ldxAbsoluteY();
  void cmd6502cpyImmediate();
  void cmd6502cmpIndirectX();
  void cmd6502cpyZeropage();
  void cmd6502cmpZeropage();
  void cmd6502decZeropage();
  void cmd6502iny();
  void cmd6502cmpImmediate();
  void cmd6502dex();
  void cmd6502cpyAbsolute();
  void cmd6502cmpAbsolute();
  void cmd6502decAbsolute();
  void cmd6502bne();
  void cmd6502cmpIndirectY();
  void cmd6502cmpZeropageX();
  void cmd6502decZeropageX();
  void cmd6502cld();
  void cmd6502cmpAbsoluteY();
  void cmd6502cmpAbsoluteX();
  void cmd6502decAbsoluteX();
  void cmd6502cpxImmediate();
  void cmd6502sbcIndirectX();
  void cmd6502cpxZeropage();
  void cmd6502sbcZeropage();
  void cmd6502incZeropage();
  void cmd6502inx();
  void cmd6502sbcImmediate();
  void cmd6502nop();
  void cmd6502cpxAbsolute();
  void cmd6502sbcAbsolute();
  void cmd6502incAbsolute();
  void cmd6502beq();
  void cmd6502sbcIndirectY();
  void cmd6502sbcZeropageX();
  void cmd6502incZeropageX();
  void cmd6502sed();
  void cmd6502sbcAbsoluteY();
  void cmd6502sbcAbsoluteX();
  void cmd6502incAbsoluteX();
  void cmd6502nopImmediate();
  void cmd6502nopZeropage();
  void cmd6502nopZeropageX();
  void cmd6502laxIndirectX();
  void cmd6502laxZeropage();
  void cmd6502laxAbsolute();
  void cmd6502laxIndirectY();
  void cmd6502laxZeropageY();
  void cmd6502laxAbsoluteY();
  void cmd6502lxaImmediate();
  void cmd6502alrImmediate();
  void cmd6502ancImmediate();
  void cmd6502saxZeropage();
  void cmd6502saxZeropageY();
  void cmd6502saxAbsolute();
  void cmd6502saxIndirectX();
  inline uint8_t isbincbase();
  void cmd6502isbZeropage();
  void cmd6502isbZeropageX();
  void cmd6502isbIndirectX();
  void cmd6502isbIndirectY();
  void cmd6502isbAbsolute();
  void cmd6502isbAbsoluteX();
  void cmd6502isbAbsoluteY();
  void cmd6502skwAbsolute();
  void cmd6502skwAbsoluteX();
  void cmd6502shaZeropageY();
  void cmd6502shaAbsoluteY();
  void cmd6502shxAbsoluteY();
  void cmd6502rraZeropage();
  void cmd6502rraZeropageX();
  void cmd6502rraIndirectX();
  void cmd6502rraIndirectY();
  void cmd6502rraAbsolute();
  void cmd6502rraAbsoluteX();
  void cmd6502rraAbsoluteY();
  void cmd6502asoZeropage();
  void cmd6502asoZeropageX();
  void cmd6502asoIndirectX();
  void cmd6502asoIndirectY();
  void cmd6502asoAbsolute();
  void cmd6502asoAbsoluteX();
  void cmd6502asoAbsoluteY();
  void cmd6502sreZeropage();
  void cmd6502sreZeropageX();
  void cmd6502sreIndirectX();
  void cmd6502sreIndirectY();
  void cmd6502sreAbsolute();
  void cmd6502sreAbsoluteX();
  void cmd6502sreAbsoluteY();
  void cmd6502dcpZeropage();
  void cmd6502dcpZeropageX();
  void cmd6502dcpIndirectX();
  void cmd6502dcpIndirectY();
  void cmd6502dcpAbsolute();
  void cmd6502dcpAbsoluteX();
  void cmd6502dcpAbsoluteY();
  void cmd6502xaaImmediate();
  void cmd6502sbxImmediate();
  void cmd6502lasAbsolute();
  void cmd6502rlaZeropage();
  void cmd6502rlaZeropageX();
  void cmd6502rlaIndirectX();
  void cmd6502rlaIndirectY();
  void cmd6502rlaAbsolute();
  void cmd6502rlaAbsoluteX();
  void cmd6502rlaAbsoluteY();
  void cmd6502tas();
  void cmd6502arr();
  void cmd6502shy();

  // undocumented implied nop commands are defined virtual so them may be used
  // for logging, profiling etc.
  virtual void cmd6502nop1a();
  virtual void cmd6502nop3a();
  virtual void cmd6502nop5a();
  virtual void cmd6502nop7a();
  virtual void cmd6502nopda();
  virtual void cmd6502nopfa();

  // brk instruction may be used for kernal patching
  virtual void cmd6502brk();

  virtual void cmd6502halt();

  void (CPU6502::*cmdarr6502[256])() = {
      // 0x00
      &CPU6502::cmd6502brk, &CPU6502::cmd6502oraIndirectX,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502asoIndirectX,
      &CPU6502::cmd6502nopZeropage, &CPU6502::cmd6502oraZeropage,
      &CPU6502::cmd6502aslZeropage, &CPU6502::cmd6502asoZeropage,
      &CPU6502::cmd6502php, &CPU6502::cmd6502oraImmediate,
      &CPU6502::cmd6502aslA, &CPU6502::cmd6502ancImmediate,
      &CPU6502::cmd6502skwAbsolute, &CPU6502::cmd6502oraAbsolute,
      &CPU6502::cmd6502aslAbsolute, &CPU6502::cmd6502asoAbsolute,
      // 0x10
      &CPU6502::cmd6502bpl, &CPU6502::cmd6502oraIndirectY,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502asoIndirectY,
      &CPU6502::cmd6502nopZeropageX, &CPU6502::cmd6502oraZeropageX,
      &CPU6502::cmd6502aslZeropageX, &CPU6502::cmd6502asoZeropageX,
      &CPU6502::cmd6502clc, &CPU6502::cmd6502oraAbsoluteY,
      &CPU6502::cmd6502nop1a, &CPU6502::cmd6502asoAbsoluteY,
      &CPU6502::cmd6502skwAbsoluteX, &CPU6502::cmd6502oraAbsoluteX,
      &CPU6502::cmd6502aslAbsoluteX, &CPU6502::cmd6502asoAbsoluteX,
      // 0x20
      &CPU6502::cmd6502jsr, &CPU6502::cmd6502andIndirectX,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502rlaIndirectX,
      &CPU6502::cmd6502bitZeropage, &CPU6502::cmd6502andZeropage,
      &CPU6502::cmd6502rolZeropage, &CPU6502::cmd6502rlaZeropage,
      &CPU6502::cmd6502plp, &CPU6502::cmd6502andImmediate,
      &CPU6502::cmd6502rolA, &CPU6502::cmd6502ancImmediate,
      &CPU6502::cmd6502bitAbsolute, &CPU6502::cmd6502andAbsolute,
      &CPU6502::cmd6502rolAbsolute, &CPU6502::cmd6502rlaAbsolute,
      // 0x30
      &CPU6502::cmd6502bmi, &CPU6502::cmd6502andIndirectY,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502rlaIndirectY,
      &CPU6502::cmd6502nopZeropageX, &CPU6502::cmd6502andZeropageX,
      &CPU6502::cmd6502rolZeropageX, &CPU6502::cmd6502rlaZeropageX,
      &CPU6502::cmd6502sec, &CPU6502::cmd6502andAbsoluteY,
      &CPU6502::cmd6502nop3a, &CPU6502::cmd6502rlaAbsoluteY,
      &CPU6502::cmd6502skwAbsoluteX, &CPU6502::cmd6502andAbsoluteX,
      &CPU6502::cmd6502rolAbsoluteX, &CPU6502::cmd6502rlaAbsoluteX,
      // 0x40
      &CPU6502::cmd6502rti, &CPU6502::cmd6502eorIndirectX,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502sreIndirectX,
      &CPU6502::cmd6502nopZeropage, &CPU6502::cmd6502eorZeropage,
      &CPU6502::cmd6502lsrZeropage, &CPU6502::cmd6502sreZeropage,
      &CPU6502::cmd6502pha, &CPU6502::cmd6502eorImmediate,
      &CPU6502::cmd6502lsrA, &CPU6502::cmd6502alrImmediate,
      &CPU6502::cmd6502jmpAbsolute, &CPU6502::cmd6502eorAbsolute,
      &CPU6502::cmd6502lsrAbsolute, &CPU6502::cmd6502sreAbsolute,
      // 0x50
      &CPU6502::cmd6502bvc, &CPU6502::cmd6502eorIndirectY,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502sreIndirectY,
      &CPU6502::cmd6502nopZeropageX, &CPU6502::cmd6502eorZeropageX,
      &CPU6502::cmd6502lsrZeropageX, &CPU6502::cmd6502sreZeropageX,
      &CPU6502::cmd6502cli, &CPU6502::cmd6502eorAbsoluteY,
      &CPU6502::cmd6502nop5a, &CPU6502::cmd6502sreAbsoluteY,
      &CPU6502::cmd6502skwAbsoluteX, &CPU6502::cmd6502eorAbsoluteX,
      &CPU6502::cmd6502lsrAbsoluteX, &CPU6502::cmd6502sreAbsoluteX,
      // 0x60
      &CPU6502::cmd6502rts, &CPU6502::cmd6502adcIndirectX,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502rraIndirectX,
      &CPU6502::cmd6502nopZeropage, &CPU6502::cmd6502adcZeropage,
      &CPU6502::cmd6502rorZeropage, &CPU6502::cmd6502rraZeropage,
      &CPU6502::cmd6502pla, &CPU6502::cmd6502adcImmediate,
      &CPU6502::cmd6502rorA, &CPU6502::cmd6502arr, &CPU6502::cmd6502jmpIndirect,
      &CPU6502::cmd6502adcAbsolute, &CPU6502::cmd6502rorAbsolute,
      &CPU6502::cmd6502rraAbsolute,
      // 0x70
      &CPU6502::cmd6502bvs, &CPU6502::cmd6502adcIndirectY,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502rraIndirectY,
      &CPU6502::cmd6502nopZeropageX, &CPU6502::cmd6502adcZeropageX,
      &CPU6502::cmd6502rorZeropageX, &CPU6502::cmd6502rraZeropageX,
      &CPU6502::cmd6502sei, &CPU6502::cmd6502adcAbsoluteY,
      &CPU6502::cmd6502nop7a, &CPU6502::cmd6502rraAbsoluteY,
      &CPU6502::cmd6502skwAbsoluteX, &CPU6502::cmd6502adcAbsoluteX,
      &CPU6502::cmd6502rorAbsoluteX, &CPU6502::cmd6502rraAbsoluteX,
      // 0x80
      &CPU6502::cmd6502nopImmediate, &CPU6502::cmd6502staIndirectX,
      &CPU6502::cmd6502nopImmediate, &CPU6502::cmd6502saxIndirectX,
      &CPU6502::cmd6502styZeropage, &CPU6502::cmd6502staZeropage,
      &CPU6502::cmd6502stxZeropage, &CPU6502::cmd6502saxZeropage,
      &CPU6502::cmd6502dey, &CPU6502::cmd6502nopImmediate, &CPU6502::cmd6502txa,
      &CPU6502::cmd6502xaaImmediate, &CPU6502::cmd6502styAbsolute,
      &CPU6502::cmd6502staAbsolute, &CPU6502::cmd6502stxAbsolute,
      &CPU6502::cmd6502saxAbsolute,
      // 0x90
      &CPU6502::cmd6502bcc, &CPU6502::cmd6502staIndirectY,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502shaZeropageY,
      &CPU6502::cmd6502styZeropageX, &CPU6502::cmd6502staZeropageX,
      &CPU6502::cmd6502stxZeropageY, &CPU6502::cmd6502saxZeropageY,
      &CPU6502::cmd6502tya, &CPU6502::cmd6502staAbsoluteY, &CPU6502::cmd6502txs,
      &CPU6502::cmd6502tas, &CPU6502::cmd6502shy, &CPU6502::cmd6502staAbsoluteX,
      &CPU6502::cmd6502shxAbsoluteY, &CPU6502::cmd6502shaAbsoluteY,
      // 0xa0
      &CPU6502::cmd6502ldyImmediate, &CPU6502::cmd6502ldaIndirectX,
      &CPU6502::cmd6502ldxImmediate, &CPU6502::cmd6502laxIndirectX,
      &CPU6502::cmd6502ldyZeropage, &CPU6502::cmd6502ldaZeropage,
      &CPU6502::cmd6502ldxZeropage, &CPU6502::cmd6502laxZeropage,
      &CPU6502::cmd6502tay, &CPU6502::cmd6502ldaImmediate, &CPU6502::cmd6502tax,
      &CPU6502::cmd6502lxaImmediate, &CPU6502::cmd6502ldyAbsolute,
      &CPU6502::cmd6502ldaAbsolute, &CPU6502::cmd6502ldxAbsolute,
      &CPU6502::cmd6502laxAbsolute,
      // 0xb0
      &CPU6502::cmd6502bcs, &CPU6502::cmd6502ldaIndirectY,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502laxIndirectY,
      &CPU6502::cmd6502ldyZeropageX, &CPU6502::cmd6502ldaZeropageX,
      &CPU6502::cmd6502ldxZeropageY, &CPU6502::cmd6502laxZeropageY,
      &CPU6502::cmd6502clv, &CPU6502::cmd6502ldaAbsoluteY, &CPU6502::cmd6502tsx,
      &CPU6502::cmd6502lasAbsolute, &CPU6502::cmd6502ldyAbsoluteX,
      &CPU6502::cmd6502ldaAbsoluteX, &CPU6502::cmd6502ldxAbsoluteY,
      &CPU6502::cmd6502laxAbsoluteY,
      // 0xc0
      &CPU6502::cmd6502cpyImmediate, &CPU6502::cmd6502cmpIndirectX,
      &CPU6502::cmd6502nopImmediate, &CPU6502::cmd6502dcpIndirectX,
      &CPU6502::cmd6502cpyZeropage, &CPU6502::cmd6502cmpZeropage,
      &CPU6502::cmd6502decZeropage, &CPU6502::cmd6502dcpZeropage,
      &CPU6502::cmd6502iny, &CPU6502::cmd6502cmpImmediate, &CPU6502::cmd6502dex,
      &CPU6502::cmd6502sbxImmediate, &CPU6502::cmd6502cpyAbsolute,
      &CPU6502::cmd6502cmpAbsolute, &CPU6502::cmd6502decAbsolute,
      &CPU6502::cmd6502dcpAbsolute,
      // 0xd0
      &CPU6502::cmd6502bne, &CPU6502::cmd6502cmpIndirectY,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502dcpIndirectY,
      &CPU6502::cmd6502nopZeropageX, &CPU6502::cmd6502cmpZeropageX,
      &CPU6502::cmd6502decZeropageX, &CPU6502::cmd6502dcpZeropageX,
      &CPU6502::cmd6502cld, &CPU6502::cmd6502cmpAbsoluteY,
      &CPU6502::cmd6502nopda, &CPU6502::cmd6502dcpAbsoluteY,
      &CPU6502::cmd6502skwAbsoluteX, &CPU6502::cmd6502cmpAbsoluteX,
      &CPU6502::cmd6502decAbsoluteX, &CPU6502::cmd6502dcpAbsoluteX,
      // 0xe0
      &CPU6502::cmd6502cpxImmediate, &CPU6502::cmd6502sbcIndirectX,
      &CPU6502::cmd6502nopImmediate, &CPU6502::cmd6502isbIndirectX,
      &CPU6502::cmd6502cpxZeropage, &CPU6502::cmd6502sbcZeropage,
      &CPU6502::cmd6502incZeropage, &CPU6502::cmd6502isbZeropage,
      &CPU6502::cmd6502inx, &CPU6502::cmd6502sbcImmediate, &CPU6502::cmd6502nop,
      &CPU6502::cmd6502sbcImmediate, &CPU6502::cmd6502cpxAbsolute,
      &CPU6502::cmd6502sbcAbsolute, &CPU6502::cmd6502incAbsolute,
      &CPU6502::cmd6502isbAbsolute,
      // 0xf0
      &CPU6502::cmd6502beq, &CPU6502::cmd6502sbcIndirectY,
      &CPU6502::cmd6502halt, &CPU6502::cmd6502isbIndirectY,
      &CPU6502::cmd6502nopZeropageX, &CPU6502::cmd6502sbcZeropageX,
      &CPU6502::cmd6502incZeropageX, &CPU6502::cmd6502isbZeropageX,
      &CPU6502::cmd6502sed, &CPU6502::cmd6502sbcAbsoluteY,
      &CPU6502::cmd6502nopfa, &CPU6502::cmd6502isbAbsoluteY,
      &CPU6502::cmd6502skwAbsoluteX, &CPU6502::cmd6502sbcAbsoluteX,
      &CPU6502::cmd6502incAbsoluteX, &CPU6502::cmd6502isbAbsoluteX};

protected:
  const char *cmdName[256] = {
      // 0x00
      "brk", "oraIndirectX", "hlt", "asoIndirectX", "nopZeropage",
      "oraZeropage", "aslZeropage", "asoZeropage", "php", "oraImmediate",
      "aslA", "ancImmediate", "skwAbsolute", "oraAbsolute", "aslAbsolute",
      "asoAbsolute",
      // 0x10
      "bpl", "oraIndirectY", "hlt", "asoIndirectY", "nopZeropageX",
      "oraZeropageX", "aslZeropageX", "asoZeropageX", "clc", "oraAbsoluteY",
      "nop1a", "asoAbsoluteY", "skwAbsoluteX", "oraAbsoluteX", "aslAbsoluteX",
      "asoAbsoluteX",
      // 0x20
      "jsr", "andIndirectX", "hlt", "rlaIndirectX", "bitZeropage",
      "andZeropage", "rolZeropage", "rlaZeropage", "plp", "andImmediate",
      "rolA", "ancImmediate", "bitAbsolute", "andAbsolute", "rolAbsolute",
      "rlaAbsolute",
      // 0x30
      "bmi", "andIndirectY", "hlt", "rlaIndirectY", "nopZeropageX",
      "andZeropageX", "rolZeropageX", "rlaZeropageX", "sec", "andAbsoluteY",
      "nop3a", "rlaAbsoluteY", "skwAbsoluteX", "andAbsoluteX", "rolAbsoluteX",
      "rlaAbsoluteX",
      // 0x40
      "rti", "eorIndirectX", "hlt", "sreIndirectX", "nopZeropage",
      "eorZeropage", "lsrZeropage", "sreZeropage", "pha", "eorImmediate",
      "lsrA", "alrImmediate", "jmpAbsolute", "eorAbsolute", "lsrAbsolute",
      "sreAbsolute",
      // 0x50
      "bvc", "eorIndirectY", "hlt", "sreIndirectY", "nopZeropageX",
      "eorZeropageX", "lsrZeropageX", "sreZeropageX", "cli", "eorAbsoluteY",
      "nop5a", "sreAbsoluteY", "skwAbsoluteX", "eorAbsoluteX", "lsrAbsoluteX",
      "sreAbsoluteX",
      // 0x60
      "rts", "adcIndirectX", "hlt", "rraIndirectX", "nopZeropage",
      "adcZeropage", "rorZeropage", "rraZeropage", "pla", "adcImmediate",
      "rorA", "arr", "jmpIndirect", "adcAbsolute", "rorAbsolute", "rraAbsolute",
      // 0x70
      "bvs", "adcIndirectY", "hlt", "rraIndirectY", "nopZeropageX",
      "adcZeropageX", "rorZeropageX", "rraZeropageX", "sei", "adcAbsoluteY",
      "nop7a", "rraAbsoluteY", "skwAbsoluteX", "adcAbsoluteX", "rorAbsoluteX",
      "rraAbsoluteX",
      // 0x80
      "nopImmediate", "staIndirectX", "nopImmediate", "saxIndirectX",
      "styZeropage", "staZeropage", "stxZeropage", "saxZeropage", "dey",
      "nopImmediate", "txa", "xaaImmediate", "styAbsolute", "staAbsolute",
      "stxAbsolute", "saxAbsolute",
      // 0x90
      "bcc", "staIndirectY", "hlt", "shaZeropageY", "styZeropageX",
      "staZeropageX", "stxZeropageY", "saxZeropageY", "tya", "staAbsoluteY",
      "txs", "tas", "shy", "staAbsoluteX", "shxAbsoluteY", "shaAbsoluteY",
      // 0xa0
      "ldyImmediate", "ldaIndirectX", "ldxImmediate", "laxIndirectX",
      "ldyZeropage", "ldaZeropage", "ldxZeropage", "laxZeropage", "tay",
      "ldaImmediate", "tax", "lxaImmediate", "ldyAbsolute", "ldaAbsolute",
      "ldxAbsolute", "laxAbsolute",
      // 0xb0
      "bcs", "ldaIndirectY", "hlt", "laxIndirectY", "ldyZeropageX",
      "ldaZeropageX", "ldxZeropageY", "laxZeropageY", "clv", "ldaAbsoluteY",
      "tsx", "lasAbsolute", "ldyAbsoluteX", "ldaAbsoluteX", "ldxAbsoluteY",
      "laxAbsoluteY",
      // 0xc0
      "cpyImmediate", "cmpIndirectX", "nopImmediate", "dcpIndirectX",
      "cpyZeropage", "cmpZeropage", "decZeropage", "dcpZeropage", "iny",
      "cmpImmediate", "dex", "sbxImmediate", "cpyAbsolute", "cmpAbsolute",
      "decAbsolute", "dcpAbsolute",
      // 0xd0
      "bne", "cmpIndirectY", "hlt", "dcpIndirectY", "nopZeropageX",
      "cmpZeropageX", "decZeropageX", "dcpZeropageX", "cld", "cmpAbsoluteY",
      "nopda", "dcpAbsoluteY", "skwAbsoluteX", "cmpAbsoluteX", "decAbsoluteX",
      "dcpAbsoluteX",
      // 0xe0
      "cpxImmediate", "sbcIndirectX", "nopImmediate", "isbIndirectX",
      "cpxZeropage", "sbcZeropage", "incZeropage", "isbZeropage", "inx",
      "sbcImmediate", "nop", "sbcImmediate", "cpxAbsolute", "sbcAbsolute",
      "incAbsolute", "isbAbsolute",
      // 0xf0
      "beq", "sbcIndirectY", "hlt", "isbIndirectY", "nopZeropageX",
      "sbcZeropageX", "incZeropageX", "isbZeropageX", "sed", "sbcAbsoluteY",
      "nopfa", "isbAbsoluteY", "skwAbsoluteX", "sbcAbsoluteX", "incAbsoluteX",
      "isbAbsoluteX"};

  bool cflag;
  bool zflag;
  bool dflag;
  bool bflag;
  bool vflag;
  bool nflag;
  // interrupt flag
  bool iflag;

  uint8_t a;
  uint8_t x;
  uint8_t y;
  uint8_t sp;
  uint8_t sr;
  uint16_t pc;

  void execute(uint8_t idx);
  void setPCToIntVec(uint16_t intvect, bool intfrombrk);
  void cmd6502rts();

public:
  // number of cycles since last adjustment
  uint8_t numofcycles;

  // stop cpu
  bool cpuhalted;

  // pure virtual methods
  virtual void run() = 0;
  uint8_t getMem(uint16_t addr);
  void setMem(uint16_t addr, uint8_t val);
};

#endif // CPU6502_H
