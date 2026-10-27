#pragma once
#include "solar_os_native_abi.h"
#include "solar_os_native_app_abi.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAC_RAM_SIZE (4U * 1024U * 1024U)
#define MAC_ROM_SIZE (128U * 1024U)
#define MAC_WIDTH 512U
#define MAC_HEIGHT 342U
#define MAC_SCREEN (MAC_RAM_SIZE - 0x5900U)
#define MAC_SCREEN_ALT (MAC_SCREEN - 0x8000U)

extern const solar_os_native_host_api_v1_t *solar_os_mac_host;
extern const solar_os_native_foreground_api_v1_t *solar_os_mac_app;
extern const solar_os_native_memory_api_v1_t *solar_os_mac_memory;
extern jmp_buf solar_os_mac_trap;
extern const char *solar_os_mac_failure;
extern uint8_t *solar_os_mac_ram, *solar_os_mac_rom;
void solar_os_mac_report(const char *text);
_Noreturn void solar_os_mac_fail(const char *text);
void solar_os_mac_machine_init(void);
void solar_os_mac_step(void);
const uint8_t *solar_os_mac_framebuffer(void);
bool solar_os_mac_rom_valid(const uint8_t *rom);
void solar_os_mac_diagnostic_rom(uint8_t *rom);
void solar_os_mac_render_reset(void);
bool solar_os_mac_render(uint8_t *pixels, uint16_t width, uint16_t height);
extern unsigned solar_os_mac_width, solar_os_mac_height, solar_os_mac_stride, solar_os_mac_screen;
bool solar_os_mac_video_configure(unsigned width, unsigned height, bool patch_rom);
int solar_os_mac_disk_open(const char *path);
int solar_os_mac_disk_close(void);
int solar_os_mac_floppy_open(const char *path);
int solar_os_mac_floppy_close(void);
bool solar_os_mac_floppy_hook(unsigned operation);
void *solar_os_mac_guest_span(uint32_t address, size_t length);
void solar_os_mac_input(const solar_os_native_foreground_key_v1_t *key);
void solar_os_mac_input_reset(void);
void solar_os_mac_pointer_tick(void);
void solar_os_mac_snapshot(void);
extern bool solar_os_mac_paused;
extern bool solar_os_mac_pram_dirty;
extern uint8_t solar_os_mac_pram[32];
int solar_os_mac_quiet(const char *format, ...);

#ifndef SOLAR_OS_MAC_PORT_IMPLEMENTATION
#define printf solar_os_mac_quiet
#define abort() solar_os_mac_fail("unsupported guest device state")
#define exit(code) solar_os_mac_fail("unsupported guest serial operation")
#undef assert
#define assert(test) ((test) ? (void)0 : solar_os_mac_fail("guest device assertion"))
#endif
