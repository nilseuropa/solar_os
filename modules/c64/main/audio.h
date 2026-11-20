// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>

// Single-producer PCM queue with the same overlap/add clock following used
// by the Amiga module. Guest speed and waveform pitch advance independently.
struct solar_os_c64_audio {
    static constexpr unsigned capacity = 8192, hop = 256, source_rate = 16000;
    int16_t *ring = nullptr;
    std::atomic<uint32_t> read{0}, write{0}, underruns{0}, output_frames{0};
    void reset(int16_t *buffer);
    void push(const int16_t *samples, unsigned count);
    void render(int16_t *output, size_t frames, uint32_t rate);
private:
    int16_t overlap[hop] = {}, grain[hop] = {};
    unsigned grain_offset = 0, grain_size = 0;
    uint32_t source = 0, speed = 65536, speed_fraction = 0;
    int32_t clock = 0, level = 0;
    uint32_t previous_write = 0, first_write = 0, resample_fraction = 0;
    uint64_t observed_frames = 0;
    bool ready = false, overlap_valid = false, resample_valid = false;
    int previous_sample = 0, next_sample = 0, last_sample = 0;
    unsigned fade_in = 0, fade_out = 0;
    uint32_t difference(uint32_t start, uint32_t limit) const;
    bool prepare(uint32_t &read_index, uint32_t write_index);
    bool sample(int &value, uint32_t &read_index, uint32_t write_index);
};
