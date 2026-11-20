// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio.h"

void solar_os_c64_audio::reset(int16_t *buffer)
{
    ring = buffer;
    read.store(0); write.store(0); underruns.store(0); output_frames.store(0);
    grain_offset = grain_size = 0;
    source = speed_fraction = resample_fraction = 0; speed = 65536;
    clock = level = 0; observed_frames = 0; previous_write = first_write = 0;
    ready = overlap_valid = resample_valid = false;
    previous_sample = next_sample = last_sample = 0; fade_in = fade_out = 0;
}

void solar_os_c64_audio::push(const int16_t *samples, unsigned count)
{
    uint32_t w = write.load(std::memory_order_relaxed);
    uint32_t r = read.load(std::memory_order_acquire);
    if (!ring || count > capacity - (w - r)) return;
    for (unsigned i = 0; i < count; ++i) ring[(w + i) & (capacity - 1)] = samples[i];
    write.store(w + count, std::memory_order_release);
}

uint32_t solar_os_c64_audio::difference(uint32_t start, uint32_t limit) const
{
    uint32_t error = 0;
    for (unsigned i = 0; i < hop; i += 4) {
        int delta = (int)overlap[i] - ring[(start + i) & (capacity - 1)];
        error += delta < 0 ? -delta : delta;
        if (error > limit) break;
    }
    return error;
}

bool solar_os_c64_audio::prepare(uint32_t &r, uint32_t w)
{
    constexpr unsigned search = hop * 3 / 4;
    unsigned available = w - r, center = source - r;
    unsigned first = center > search ? center - search : 0;
    if (available < hop * 2 || first > available - hop * 2) return false;
    unsigned last = center + search;
    if (last > available - hop * 2) last = available - hop * 2;
    unsigned best = center < last ? center : last;
    if (overlap_valid) {
        uint32_t error = difference(r + best, UINT32_MAX);
        if (error) {
            for (unsigned candidate = first; candidate <= last; candidate += 4) {
                uint32_t next = difference(r + candidate, error);
                if (next < error) { best = candidate; error = next; }
                if (!error) break;
            }
            unsigned low = best > 4 ? best - 4 : 0, high = best + 4;
            if (low < first) low = first;
            if (high > last) high = last;
            for (unsigned candidate = low; candidate <= high; ++candidate) {
                uint32_t next = difference(r + candidate, error);
                if (next < error) { best = candidate; error = next; }
            }
        }
    }
    for (unsigned i = 0; i < hop; ++i) {
        int value = ring[(r + best + i) & (capacity - 1)];
        if (overlap_valid) value = overlap[i] + (value - overlap[i]) * (int)(i + 1) / (int)hop;
        grain[i] = value;
        overlap[i] = ring[(r + best + i + hop) & (capacity - 1)];
    }
    overlap_valid = true; grain_offset = 0; grain_size = hop;
    speed_fraction += speed * hop;
    source += speed_fraction >> 16; speed_fraction &= 65535;
    unsigned behind = source - r;
    if (behind > search) r += behind - search;
    return true;
}

bool solar_os_c64_audio::sample(int &value, uint32_t &r, uint32_t w)
{
    if (grain_offset == grain_size && !prepare(r, w)) return false;
    value = grain[grain_offset++]; return true;
}

void solar_os_c64_audio::render(int16_t *output, size_t frames, uint32_t rate)
{
    if (!frames || !rate) return;
    uint32_t r = read.load(std::memory_order_relaxed), w = write.load(std::memory_order_acquire);
    constexpr unsigned target = source_rate * 128 / 1000;
    if (!ready && w - r < 2) observed_frames = 0;
    if (!observed_frames) first_write = w;
    if (!ready && w - r >= target) {
        ready = true; source = r; speed_fraction = resample_fraction = 0;
        uint64_t estimate = clock ? (unsigned)clock : observed_frames ?
            ((uint64_t)(w - first_write) * rate << 16) / (observed_frames * source_rate) : 65536;
        speed = estimate < 4096 ? 4096 : estimate > 131072 ? 131072 : estimate;
        clock = speed; level = target * 256;
        overlap_valid = resample_valid = false; grain_offset = grain_size = 0; fade_in = 32;
    }
    if (ready) {
        int64_t measured = ((uint64_t)(w - previous_write) * rate << 16) / (frames * source_rate);
        int64_t next_clock = clock + (measured - clock) / 8;
        clock = next_clock < 4096 ? 4096 : next_clock > 131072 ? 131072 : next_clock;
        level += ((int32_t)(w - r) * 256 - level) / 16;
        int desired = clock + (level / 256 - (int)target) * 8;
        speed = desired < 4096 ? 4096 : desired > 131072 ? 131072 : desired;
    }
    uint32_t step = (uint64_t)source_rate * 65536 / rate;
    for (size_t i = 0; i < frames; ++i) {
        int value = 0;
        bool valid = ready;
        if (valid && !resample_valid) {
            valid = sample(previous_sample, r, w) && sample(next_sample, r, w);
            resample_valid = valid;
        }
        if (valid) {
            value = previous_sample + (int64_t)(next_sample - previous_sample) * resample_fraction / 65536;
            resample_fraction += step;
            while (resample_fraction >= 65536 && valid) {
                resample_fraction -= 65536; previous_sample = next_sample;
                valid = sample(next_sample, r, w);
            }
        }
        if (ready && !valid) {
            ready = false; fade_out = 32; observed_frames = 0; first_write = w;
            underruns.store(underruns.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
        if (ready) {
            if (fade_in) { value = value * (int)(32 - fade_in) / 32; --fade_in; }
            last_sample = value;
        } else if (fade_out) { value = last_sample * (int)fade_out / 32; --fade_out; }
        else value = 0;
        output[i * 2] = output[i * 2 + 1] = value;
    }
    read.store(r, std::memory_order_release);
    observed_frames += frames; previous_write = w;
    output_frames.store(output_frames.load(std::memory_order_relaxed) + frames, std::memory_order_relaxed);
}
