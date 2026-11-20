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
#ifndef CIA_H
#define CIA_H

#include <atomic>
#include <cstdint>

// register dc0d:
// - Interrupt Control Register when written to
// - is an Interrupt Latch Register when read from
// - read the clear-on-read register $dc0d will ACK all pending CIA 1 interrupts

class CIA {
private:
  bool updateTODInt();

public:
  uint8_t ciareg[0x10];

  bool underflowTimerA;
  uint8_t serbitnr;
  uint8_t serbitnrnext;
  uint8_t latchdc04;
  uint8_t latchdc05;
  uint8_t latchdc06;
  uint8_t latchdc07;
  uint8_t latchdc0d; // read latch register
  uint16_t timerA;
  uint16_t timerB;

  std::atomic<bool> isTODRunning;
  bool isTODFreezed;
  std::atomic<bool> isAlarm;
  std::atomic<uint8_t> latchrundc08; // TOD running
  std::atomic<uint8_t> latchrundc09;
  std::atomic<uint8_t> latchrundc0a;
  std::atomic<uint8_t> latchrundc0b;
  std::atomic<uint8_t> latchalarmdc08; // set alarm
  std::atomic<uint8_t> latchalarmdc09;
  std::atomic<uint8_t> latchalarmdc0a;
  std::atomic<uint8_t> latchalarmdc0b;
  std::atomic<uint16_t> todSeq{0};

  CIA(bool isCIA1);
  void init(bool isCIA1);
  void checkAlarm();
  void checkTimerA(uint8_t deltaT);
  void checkTimerB(uint8_t deltaT);
  uint8_t getCommonCIAReg(uint8_t ciaidx);
  void setCommonCIAReg(uint8_t ciaidx, uint8_t val);
  void updateTOD();
};
#endif // CIA_H
