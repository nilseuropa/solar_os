#pragma once

#include <stdint.h>

/* Normalized logical key bytes shared by SolarOS input and native apps.
 * Ordinary characters use their character byte; special keys use these codes. */
#define SOLAR_OS_KEY_ENTER ((uint8_t)'\n')
#define SOLAR_OS_KEY_UP 0x80u
#define SOLAR_OS_KEY_DOWN 0x81u
#define SOLAR_OS_KEY_LEFT 0x82u
#define SOLAR_OS_KEY_RIGHT 0x83u
#define SOLAR_OS_KEY_PAGE_UP 0x84u
#define SOLAR_OS_KEY_PAGE_DOWN 0x85u
#define SOLAR_OS_KEY_F1 0x86u
#define SOLAR_OS_KEY_F2 0x87u
#define SOLAR_OS_KEY_F3 0x88u
#define SOLAR_OS_KEY_F4 0x89u
#define SOLAR_OS_KEY_F5 0x8au
#define SOLAR_OS_KEY_F6 0x8bu
#define SOLAR_OS_KEY_F7 0x8cu
#define SOLAR_OS_KEY_F8 0x8du
#define SOLAR_OS_KEY_F9 0x8eu
#define SOLAR_OS_KEY_F10 0x8fu
#define SOLAR_OS_KEY_F11 0x90u
#define SOLAR_OS_KEY_F12 0x91u
#define SOLAR_OS_KEY_APP_EXIT 0x92u
#define SOLAR_OS_KEY_ALT_PREFIX 0x93u
#define SOLAR_OS_KEY_HOME 0x94u
#define SOLAR_OS_KEY_END 0x95u
#define SOLAR_OS_KEY_DELETE 0x96u
#define SOLAR_OS_KEY_SHIFT_UP 0x97u
#define SOLAR_OS_KEY_SHIFT_DOWN 0x98u
#define SOLAR_OS_KEY_SHIFT_LEFT 0x99u
#define SOLAR_OS_KEY_SHIFT_RIGHT 0x9au
#define SOLAR_OS_KEY_SHIFT_PAGE_UP 0x9bu
#define SOLAR_OS_KEY_SHIFT_PAGE_DOWN 0x9cu
#define SOLAR_OS_KEY_SHIFT_HOME 0x9du
#define SOLAR_OS_KEY_SHIFT_END 0x9eu
#define SOLAR_OS_KEY_CTRL_UP 0x9fu
#define SOLAR_OS_KEY_CTRL_DOWN 0xa0u
#define SOLAR_OS_KEY_CTRL_LEFT 0xa1u
#define SOLAR_OS_KEY_CTRL_RIGHT 0xa2u
#define SOLAR_OS_KEY_CTRL_SHIFT_UP 0xa3u
#define SOLAR_OS_KEY_CTRL_SHIFT_DOWN 0xa4u
#define SOLAR_OS_KEY_CTRL_SHIFT_LEFT 0xa5u
#define SOLAR_OS_KEY_CTRL_SHIFT_RIGHT 0xa6u
#define SOLAR_OS_KEY_CTRL_HOME 0xa7u
#define SOLAR_OS_KEY_CTRL_END 0xa8u
#define SOLAR_OS_KEY_CTRL_SHIFT_HOME 0xa9u
#define SOLAR_OS_KEY_CTRL_SHIFT_END 0xaau
#define SOLAR_OS_KEY_CTRL_PLUS 0xabu
#define SOLAR_OS_KEY_CTRL_MINUS 0x1fu
#define SOLAR_OS_KEY_CTRL 0xacu
#define SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE 0xadu
/* A language layout character that no key byte can carry (outside ASCII and
 * Latin-1 letters, or a dead key). The character follows from the event's
 * usage and modifiers, see solar_os_input_resolve_layout_key(); input
 * consumers receive the resolved byte instead of these codes. */
#define SOLAR_OS_KEY_LAYOUT_TEXT 0xaeu
#define SOLAR_OS_KEY_LAYOUT_TEXT_CAPS 0xafu
#define SOLAR_OS_KEY_ESCAPE 0x1bu
