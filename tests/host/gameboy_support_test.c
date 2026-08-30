#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_gameboy_rom.h"
#include "solar_os_gameboy_video.h"

#define AUDIO_SAMPLE_RATE 16000
#define MINIGB_APU_AUDIO_FORMAT_S16SYS 1
#include "vendor/peanut_gb/minigb_apu.h"

#undef VERTICAL_SYNC
#define ENABLE_SOUND 0
#define PEANUT_GB_12_COLOUR 0
#include "vendor/peanut_gb/peanut_gb.h"

static uint8_t core_rom[32U * 1024U];
static size_t core_scanlines;

static uint8_t core_rom_read(struct gb_s *gb, uint_fast32_t address) {
  (void)gb;
  return address < sizeof(core_rom) ? core_rom[address] : 0xFFU;
}

static uint8_t core_ram_read(struct gb_s *gb, uint_fast32_t address) {
  (void)gb;
  (void)address;
  return 0xFFU;
}

static void core_ram_write(struct gb_s *gb, uint_fast32_t address,
                           uint8_t value) {
  (void)gb;
  (void)address;
  (void)value;
}

static void core_error(struct gb_s *gb, enum gb_error_e error,
                       uint16_t address) {
  (void)gb;
  fprintf(stderr, "Peanut-GB error %d at 0x%04x\n", (int)error,
          (unsigned)address);
  abort();
}

static void core_draw_line(struct gb_s *gb, const uint8_t *pixels,
                           uint_fast8_t line) {
  (void)gb;
  assert(pixels != NULL);
  assert(line < SOLAR_OS_GAMEBOY_LCD_HEIGHT);
  core_scanlines++;
}

static void finish_header(uint8_t *rom) {
  uint8_t checksum = 0;
  for (size_t i = 0x134U; i <= 0x14CU; i++) {
    checksum = (uint8_t)(checksum - rom[i] - 1U);
  }
  rom[0x14DU] = checksum;
}

static void test_rom_validation(void) {
  uint8_t *rom = calloc(1, 32U * 1024U);
  assert(rom != NULL);
  memcpy(&rom[0x134U], "SOLAROS TEST", 12U);
  rom[0x147U] = 0;
  rom[0x148U] = 0;
  rom[0x149U] = 0;
  finish_header(rom);

  assert(solar_os_gameboy_rom_validate(rom, 32U * 1024U) ==
         SOLAR_OS_GAMEBOY_ROM_OK);
  assert(solar_os_gameboy_rom_validate(rom, 0x14FU) ==
         SOLAR_OS_GAMEBOY_ROM_TOO_SMALL);
  rom[0x14DU] ^= 1U;
  assert(solar_os_gameboy_rom_validate(rom, 32U * 1024U) ==
         SOLAR_OS_GAMEBOY_ROM_BAD_CHECKSUM);
  rom[0x14DU] ^= 1U;
  rom[0x148U] = 1U;
  finish_header(rom);
  assert(solar_os_gameboy_rom_validate(rom, 32U * 1024U) ==
         SOLAR_OS_GAMEBOY_ROM_TRUNCATED);
  rom[0x148U] = 0U;
  rom[0x143U] = 0xC0U;
  finish_header(rom);
  assert(solar_os_gameboy_rom_validate(rom, 32U * 1024U) ==
         SOLAR_OS_GAMEBOY_ROM_CGB_ONLY);
  free(rom);
}

static void assert_uniform_shade(uint8_t shade) {
  uint8_t bitmap[SOLAR_OS_GAMEBOY_BITMAP_BYTES];
  uint8_t line[SOLAR_OS_GAMEBOY_LCD_WIDTH];
  memset(line, shade, sizeof(line));
  solar_os_gameboy_video_clear(bitmap, sizeof(bitmap));
  for (size_t y = 0; y < SOLAR_OS_GAMEBOY_LCD_HEIGHT; y++) {
    assert(solar_os_gameboy_video_scanline(bitmap, sizeof(bitmap), line, y));
  }
  const uint8_t packed = (uint8_t)(shade * 0x55U);
  for (size_t i = 0; i < sizeof(bitmap); i++) {
    assert(bitmap[i] == packed);
  }
}

static void test_frame_index2(void) {
  assert_uniform_shade(0);
  assert_uniform_shade(1);
  assert_uniform_shade(2);
  assert_uniform_shade(3);

  uint8_t shades[SOLAR_OS_GAMEBOY_LCD_WIDTH];
  for (size_t x = 0; x < sizeof(shades); x++) {
    shades[x] = (uint8_t)(x & 3U);
  }
  uint8_t packed[SOLAR_OS_GAMEBOY_BITMAP_BYTES] = {0};
  assert(solar_os_gameboy_video_scanline(packed, sizeof(packed), shades, 0));
  for (size_t x = 0; x < SOLAR_OS_GAMEBOY_BITMAP_STRIDE; x++) {
    assert(packed[x] == 0xe4U);
  }

  uint8_t bitmap[SOLAR_OS_GAMEBOY_BITMAP_BYTES] = {0};
  uint8_t line[SOLAR_OS_GAMEBOY_LCD_WIDTH] = {0};
  assert(!solar_os_gameboy_video_scanline(bitmap, sizeof(bitmap), line,
                                          SOLAR_OS_GAMEBOY_LCD_HEIGHT));
  assert(
      !solar_os_gameboy_video_scanline(bitmap, sizeof(bitmap) - 1U, line, 0));
}

static void test_vendored_core_frame(void) {
  memset(core_rom, 0, sizeof(core_rom));
  core_rom[0x100U] = 0xC3U; /* JP 0x0150, past the cartridge header. */
  core_rom[0x101U] = 0x50U;
  core_rom[0x102U] = 0x01U;
  core_rom[0x147U] = 0x00U; /* ROM-only cartridge. */
  core_rom[0x148U] = 0x00U; /* 32 KiB ROM. */
  core_rom[0x149U] = 0x00U; /* No cartridge RAM. */
  core_rom[0x150U] = 0xC3U; /* Infinite JP 0x0150 loop. */
  core_rom[0x151U] = 0x50U;
  core_rom[0x152U] = 0x01U;
  finish_header(core_rom);

  struct gb_s *core = calloc(1, sizeof(*core));
  assert(core != NULL);
  assert(gb_init(core, core_rom_read, core_ram_read, core_ram_write, core_error,
                 NULL) == GB_INIT_NO_ERROR);
  gb_init_lcd(core, core_draw_line);
  gb_run_frame(core); /* Complete the core's initial partial video frame. */
  core_scanlines = 0;
  gb_run_frame(core);
  assert(core_scanlines == SOLAR_OS_GAMEBOY_LCD_HEIGHT);

  core->direct.frame_skip = true;
  core_scanlines = 0;
  gb_run_frame(core);
  const size_t first_skipped_count = core_scanlines;
  core_scanlines = 0;
  gb_run_frame(core);
  const size_t second_skipped_count = core_scanlines;
  assert((first_skipped_count == 0U &&
          second_skipped_count == SOLAR_OS_GAMEBOY_LCD_HEIGHT) ||
         (second_skipped_count == 0U &&
          first_skipped_count == SOLAR_OS_GAMEBOY_LCD_HEIGHT));
  free(core);
}

static void test_vendored_apu_output(void) {
  struct minigb_apu_ctx apu;
  int16_t samples[AUDIO_SAMPLES_TOTAL];
  minigb_apu_audio_init(&apu);
  assert((minigb_apu_audio_read(&apu, 0xFF26U) & 0x80U) != 0);

  minigb_apu_audio_write(&apu, 0xFF24U, 0x77U);
  minigb_apu_audio_write(&apu, 0xFF25U, 0x11U);
  minigb_apu_audio_write(&apu, 0xFF11U, 0x80U);
  minigb_apu_audio_write(&apu, 0xFF12U, 0xF3U);
  minigb_apu_audio_write(&apu, 0xFF13U, 0x00U);
  minigb_apu_audio_write(&apu, 0xFF14U, 0x87U);
  minigb_apu_audio_callback(&apu, samples);

  bool nonzero = false;
  for (size_t i = 0; i < AUDIO_SAMPLES_TOTAL; i++) {
    if (samples[i] != 0) {
      nonzero = true;
      break;
    }
  }
  assert(nonzero);
}

int main(void) {
  test_rom_validation();
  test_frame_index2();
  test_vendored_core_frame();
  test_vendored_apu_output();
  puts("gameboy support tests passed");
  return 0;
}
