+++
id = "dsp"
title = "Digital signal processing"
section = "api"
summary = "Portable fixed-point DSP operations, streaming contexts, and script APIs"
aliases = ["service.dsp", "dsp.api"]
keywords = "dsp fixed point q15 fir decimator fft simd pie python lua"
packages_any = ["service_dsp"]
+++
# Digital signal processing

Use `solaros.dsp` to scale, mix, filter, or analyze signed 16-bit samples from
Python or Lua. Create a FIR, decimator, or FFT processor for repeated work, and
close it when finished. Native C APIs are listed below.

SolarOS selects acceleration automatically where available on ESP32-S3.
The same API works with the portable backend on other boards.
Python and Lua bindings use the public system DSP ABI. Streaming processors
release their system handles when closed or collected by the interpreter.

## Numeric contract

- Samples, gains, windows, and FIR coefficients are signed 16-bit values.
- Gains, windows, and coefficients use Q15. `32767` is the largest positive
  value and `-32768` represents `-1.0`.
- Products and filter sums use wide accumulators. Scaling divides by 32768,
  rounds toward negative infinity, and saturates to the signed 16-bit range.
- Portable and accelerated paths have the same output contract.
- Exact in-place stateless operations are supported where the input and output
  element counts match. Partial buffer overlap is rejected.
- `level` returns unsigned peak magnitude and integer RMS magnitude.
- FFT input is real signed 16-bit data. Output is interleaved signed 16-bit
  `(real, imaginary)` data. Every radix-2 stage divides by two, so the returned
  exponent is `log2(size)` and the mathematical spectrum equals the returned
  values multiplied by `2 ** exponent`, subject to fixed-point quantization.

FIR coefficient zero multiplies the newest sample. A decimator filters every
input sample and emits the filtered value at phase zero. Its history and phase
continue across calls until `reset`.

## Native API

Include `solar_os_dsp.h`. Stateless operations are:

```c
solar_os_dsp_dot_s16(a, b, count, &dot);
solar_os_dsp_gain_q15(output, input, count, gain_q15);
solar_os_dsp_mix_q15(output, a, b, count, gain_a_q15, gain_b_q15);
solar_os_dsp_clip_s16(output, input, count, minimum, maximum);
solar_os_dsp_level_s16(input, count, &level);
solar_os_dsp_window_q15(output, input, window_q15, count);
```

Streaming processors use caller-owned opaque contexts:

```c
solar_os_dsp_fir_create(coefficients, taps, &fir);
solar_os_dsp_fir_process(fir, output, input, count);
solar_os_dsp_fir_reset(fir);
solar_os_dsp_fir_destroy(fir);

solar_os_dsp_decimator_create(coefficients, taps, factor, &decimator);
solar_os_dsp_decimator_process(decimator, output, capacity,
                               input, count, &produced);
solar_os_dsp_decimator_destroy(decimator);

solar_os_dsp_fft_create(size, &fft);
solar_os_dsp_fft_execute(fft, spectrum, input, &scale_exponent);
solar_os_dsp_fft_destroy(fft);
```

FIR filters support 1 through 1024 taps. FFT sizes must be powers of two from 2
through 4096. One context must not be used concurrently by multiple callers.

Use `solar_os_dsp_backend()`, `solar_os_dsp_capabilities()`, and
`solar_os_dsp_accelerated_capabilities()` for diagnostics. Capability bits are:

| Bit | Operation |
| --- | --- |
| 0 | signed 16-bit dot product |
| 1 | Q15 gain |
| 2 | Q15 mix |
| 3 | signed 16-bit clip |
| 4 | signed 16-bit level |
| 5 | Q15 window |
| 6 | Q15 FIR |
| 7 | Q15 decimator |
| 8 | signed 16-bit FFT, PIE accelerated on eligible ESP32-S3 blocks |

## Python

Python accepts contiguous buffer objects containing native little-endian
signed 16-bit samples. `array('h')` and `bytearray` objects are suitable.
Stateless output functions return a new `bytearray`:

```python
from array import array
import solaros

dsp = solaros.dsp
samples = array('h', [1000, -1000, 500, -500])

scaled = dsp.gain(samples, 0.5)
mixed = dsp.mix(samples, samples, 0.7, 0.3)
clipped = dsp.clip(samples, -800, 800)
peak, rms = dsp.level(samples)
energy = dsp.dot(samples, samples)
```

`window(samples, coefficients)` returns a new sample buffer. Both inputs must
contain the same number of samples. Gains use floating-point values from `-1.0`
through `1.0` and are converted to Q15.

Stateful processors own their native context and release it during `close()` or
garbage collection:

```python
coefficients = array('h', [16384, 16384])
filt = dsp.fir(coefficients)
filtered = filt.process(samples)

downsample = dsp.fir(coefficients, decimation=4)
low_rate = downsample.process(samples)
downsample.reset()

fft = dsp.fft(1024)
spectrum, exponent = fft.execute(array('h', [0] * 1024))
fft.close()
```

The script bridge accepts at most 32768 samples per call.

## Lua

Lua uses binary strings containing native little-endian signed 16-bit values.
Stateless functions return a new binary string. FIR and FFT constructors return
userdata that own the native context:

```lua
local dsp = solaros.dsp
local scaled = dsp.gain(samples, 0.5)
local peak, rms = dsp.level(samples)

local filt = dsp.fir(coefficients, 4)
local low_rate = filt:process(samples)
filt:reset()
filt:close()

local fft = dsp.fft(1024)
local spectrum, exponent = fft:execute(samples_1024)
```

Lua strings are immutable, so each output operation allocates a new string.
The script bridge accepts at most 32768 samples per call.

## Quick reference

Use `service.dsp` for synchronous fixed-point block processing. The service
selects portable or ESP32-S3 PIE code without application-side board checks.
Callers own streaming contexts and must destroy or close them. Python uses
signed 16-bit buffer objects; Lua uses binary strings. The native Synth service
uses `level` for its captured PCM scope blocks and publishes the resulting peak
and RMS values through Synth status.

`service.signal-widgets` builds reusable oscilloscope and spectrum views on top
of `solar_os_gfx` and this DSP API. Its submit calls copy recent mono or
interleaved signed-16-bit PCM into thread-safe snapshots. Rendering the spectrum
applies a Hann window and a 512-point FFT; the DSP service selects PIE SIMD on
ESP32-S3 without widget-side board checks.
