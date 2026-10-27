// SPDX-License-Identifier: MIT
#include "a500.h"
#include "port.h"
#include <string.h>

static constexpr uint32_t colorclock = 3546895;
struct audio_channel {
    uint32_t location, current, words_left, remaining;
    uint16_t length, period, volume, data, word;
    int8_t sample;
    bool low_byte, manual, pending;
};
static audio_channel channels[4];
static unsigned dma_mask;
static uint32_t output_rate = 16000, output_phase, integrated_clocks;
static int64_t integrated_left, integrated_right;

void solar_os_amiga_audio_rate(uint32_t rate) { output_rate = rate; }
void paula_reset(void)
{
    memset(channels, 0, sizeof(channels)); dma_mask = 0;
    output_phase = integrated_clocks = 0;
    integrated_left = integrated_right = 0;
}
static uint32_t period(const audio_channel &c) { return c.period ? c.period : 65536; }
static uint32_t length(const audio_channel &c) { return c.length ? c.length : 65536; }

void paula_write(uint32_t off, uint16_t value)
{
    unsigned index = (off - 0xa0) / 16;
    if (index >= 4) return;
    audio_channel &c = channels[index];
    switch (off & 15) {
    case 0: c.location = (c.location & 0xffff) | ((uint32_t)value << 16); break;
    case 2: c.location = (c.location & 0xffff0000) | (value & 0xfffe); break;
    case 4: c.length = value; break;
    case 6: c.period = value; break;
    case 8: c.volume = (value & 64) ? 64 : value & 63; break;
    case 10:
        c.data = value;
        if (!(dma_mask & (1U << index))) {
            if (!c.manual) {
                c.manual = true; c.word = value; c.sample = (int8_t)(value >> 8);
                c.low_byte = true; c.remaining = period(c);
            } else c.pending = true;
        }
        break;
    }
}

void paula_set_dma(uint16_t value)
{
    unsigned next = (value & 0x200) ? value & 15 : 0;
    for (unsigned i = 0; i < 4; ++i) {
        unsigned bit = 1U << i;
        audio_channel &c = channels[i];
        if ((next & bit) && !(dma_mask & bit)) {
            c.current = c.location & 0x7fffe; c.words_left = length(c);
            c.remaining = period(c); c.low_byte = c.manual = c.pending = false;
            intreq_set(7 + i);
        } else if (!(next & bit) && (dma_mask & bit)) {
            c.sample = 0; c.manual = c.pending = false;
        }
    }
    dma_mask = next;
}

static void advance(unsigned i)
{
    audio_channel &c = channels[i];
    if (dma_mask & (1U << i)) {
        if (!c.low_byte) {
            uint32_t address = c.current & 0x7fffe;
            c.word = (uint16_t)chip_ram[address] << 8 | chip_ram[address + 1];
            c.current = (address + 2) & 0x7fffe;
            if (!--c.words_left) {
                c.current = c.location & 0x7fffe; c.words_left = length(c);
                intreq_set(7 + i);
            }
            c.sample = (int8_t)(c.word >> 8);
        } else c.sample = (int8_t)c.word;
        c.low_byte = !c.low_byte;
    } else if (c.low_byte) {
        c.sample = (int8_t)c.word; c.low_byte = false;
    } else {
        intreq_set(7 + i);
        if (c.pending) {
            c.word = c.data; c.pending = false; c.low_byte = true;
            c.sample = (int8_t)(c.word >> 8);
        } else c.manual = false;
    }
    c.remaining = period(c);
}

void paula_step(int clocks)
{
    // Advance to DAC changes and output boundaries rather than visiting every
    // color clock. Integrating each output interval also suppresses aliasing
    // when a Paula channel runs above the host's output sample rate.
    while (clocks > 0) {
        unsigned span = (colorclock - output_phase + output_rate - 1) / output_rate;
        if (span > (unsigned)clocks) span = clocks;
        for (unsigned i = 0; i < 4; ++i)
            if (((dma_mask & (1U << i)) || channels[i].manual) && channels[i].remaining < span)
                span = channels[i].remaining;
        int left = (channels[0].sample * channels[0].volume + channels[3].sample * channels[3].volume) * 2;
        int right = (channels[1].sample * channels[1].volume + channels[2].sample * channels[2].volume) * 2;
        integrated_left += (int64_t)left * span;
        integrated_right += (int64_t)right * span;
        integrated_clocks += span; output_phase += span * output_rate; clocks -= span;
        for (unsigned i = 0; i < 4; ++i) if ((dma_mask & (1U << i)) || channels[i].manual) {
            channels[i].remaining -= span;
            if (!channels[i].remaining) advance(i);
        }
        if (output_phase >= colorclock) {
            output_phase -= colorclock;
            solar_os_amiga_audio_sample(integrated_left / integrated_clocks, integrated_right / integrated_clocks);
            integrated_left = integrated_right = 0; integrated_clocks = 0;
        }
    }
}
