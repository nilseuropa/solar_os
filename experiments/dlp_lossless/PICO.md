# Pico RAM lossless experiment, 2026-10-20

The connected Pico originally advertised the 240×64 monochrome M100 profile.
Its 2 MiB flash was backed up and verified before loading the RAM profile.
All measurements below use a 320×240 RGB565 image, a 125 MHz RP2040 B2,
Pico SDK 2.2.0, Release builds, and a 16-packet upload window. No panel or
continuous scanout backend runs. SPI remains initialized but has no test host;
the measured transport is USB CDC.

The source project had unrelated local changes. Builds used isolated copies
of its current sources under `/tmp`, and did not modify that checkout.
`results/pico/source-snapshot.json` records the source hashes and commit.

## Results

Medians of three measured updates per scene/mode. Each is preceded by one
unmeasured preparation frame. The scene generator and encoding planner are
shared with the ESP32-S3 experiment; dimensions differ. Times cover USB upload,
protocol handling, decode, RAM conversion/writes, and confirmed completion.
Desktop encoding and subsequent checksum queries are outside these times.

Both instrumented trials also perform a decoded-stream hash while writing;
this costs about 24 ms for a full frame and is included in the measurements.
It costs the same in each codec/control comparison. Ordinary RAM firmware has
no diagnostic hash work; its raw full-frame baseline was about 1.14 seconds.

| Scene | Existing RLE/bands, ms | Tiles + LZ4, ms | Tiles + LZ4 + faster CRC, ms |
|---|---:|---:|---:|
| Solid color | 1156.5 | 240.9 | 201.6 |
| Two small UI indicators | 79.1 | 7.7 | 5.7 |
| Synthetic text-line changes | 57.0 | 38.8 | 29.2 |
| Moving gradient | 1179.4 | 761.0 | 600.5 |
| Random noise | 1180.9 | 1180.6 | 941.2 |

Solid-frame USB traffic falls from 191,276 bytes to 10,326 bytes. Sparse UI
traffic falls from 12,300 bytes to 510 bytes. Noise is sent raw and retains
the same byte count. The noise improvement in the last column comes from the
CRC implementation, not compression.

The independent CRC experiment replaces eight bit steps per input byte with
two lookups into a 16-entry constant table. It produces the same reflected
IEEE CRC32 as the existing implementation and the desktop's zlib. With the
same RLE/band planner and raw noise payload, completion improves from
1,180.9 ms to 939.9 ms, about 20%. Its code/constant-data increase over the
LZ4 trial is 56 bytes, with no additional static RAM.

## CPU and storage

LZ4 decoding of a full 153,600-byte solid frame takes about **13.8 ms**.
The animated gradient takes about **22.8 ms**. The current byte-wise RGB565
conversion/write path takes about **85 ms** for a full frame. These timings
show that the decoder is feasible on Cortex-M0+; the current transport,
packet-processing, and framebuffer-conversion paths also matter substantially.

The ordinary RAM build has 52,100 bytes of text/constant data and 164,264
bytes of BSS. The LZ4 trial, including timing and verification support, uses
52,892 and 164,328 bytes respectively: **792 bytes more code/constant data
and 64 bytes more BSS**. The 1,024-byte decode buffer already exists in the
ordinary protocol device. The codec adds no framebuffer or heap allocation.
The fast-CRC trial uses 52,948 bytes of text/constant data and the same BSS.

## Fixed-cell USB framing

The faster-CRC trial also ran fixed 256-byte framing with both raw and LZ4
uploads. These are USB measurements using SPI-sized cells, not physical SPI
measurements.

| Solid full-frame mode | USB bytes | Completion, ms |
|---|---:|---:|
| Compact raw | 191,276 | 926.8 |
| Compact LZ4 | 10,326 | 201.6 |
| Fixed raw | 201,984 | 907.2 |
| Fixed LZ4 | 41,728 | 297.4 |

The LZ4 frame requires 150 data cells instead of 739 raw data cells, about
4.9 times fewer. Compact USB omits padding and therefore benefits more in
transferred bytes than fixed-cell SPI would. The fixed-framing gradient and
noise cases were also checked; their samples are saved with the other results.

## Verification

The study includes 60 ordinary-RAM baseline updates, 105 instrumented codec
updates, 45 fast-CRC updates, and 18 fixed-framing updates. **All 168 measured
instrumented updates matched both the decoded rectangle-stream checksum and
the checksum read from the actual stored RAM framebuffer.** The RAM checksum
serializes native pixels back into canonical big-endian RGB565, so it also
checks byte order and rectangle placement. FNV-1a is used for diagnostics,
alongside the protocol's normal packet CRCs.

Three portable protocol sanitizer executables passed for each of the baseline,
LZ4, and fast-CRC variants. Both native-storage framebuffer test variants
passed. The fast CRC also matched 1,028 zlib reference vectors. The decoder
is unchanged from the earlier reference-library and malformed-input tests.

## Reproduction and artifacts

Prepare independent source trees using:

```sh
python3 -B experiments/dlp_lossless/pico_prepare.py \
  --source ../pico_display_link --output /tmp/pico-study
python3 -B experiments/dlp_lossless/pico_crc_variant.py \
  --source /tmp/pico-study/trial --output /tmp/pico-study/trial-fast-crc
```

Build each tree with the project's standalone CMake flow, selecting
`-DSDLP_OUTPUT_PROFILE=RAM -DCMAKE_BUILD_TYPE=Release` and a Pico SDK 2.2.0
checkout. An ARM toolchain and picotool are required. USB loading needs a
picotool built with libusb support; the SDK's build-only helper can omit it.
This host used a locally built USB-enabled picotool 2.2.0 because the packaged
2.3.0 tool crashed when accessing BOOTSEL.

`pico_flash.py` uses the SDK's 1200-baud reset, checks the BOOTSEL flash identity,
loads and verifies a supplied UF2, and waits for CDC to reappear. Supply the
actual port, flash ID, and tool path; no device identity is embedded in it.
Back up the installed flash before the first change.

For ordinary RAM firmware:

```sh
python3 -B experiments/dlp_lossless/pico_benchmark.py \
  --port PICO_PORT --output /tmp/pico-baseline \
  --modes raw-full,rle-full,rle-bands,rle-tiles
```

For the LZ4 trial, use the same command with the default mode list and
`--verify`. For the CRC variant, use
`--modes raw-full,rle-bands,lz4-tiles --verify`. The fixed-framing check uses
`--modes raw-full,lz4-tiles --scenes solid,gradient,noise --fixed --verify`.

`pico-receiver.patch` records the private trial overlay; `pico-fast-crc.patch`
records the checksum-only difference from that trial. Source and firmware
hashes, memory sizes, all individual samples, and median summaries are in
`results/pico/`.

## Scope

After the trials, ordinary RAM firmware was installed and flash-verified.
Two subsequent RGB565 uploads completed, and the private trial query returned
unsupported. The Pico remains a 320×240 color RAM endpoint; the pre-existing
source checkout changes were preserved.

This validates lossless decoding and RAM updates on a real RP2040 over USB.
It does not measure an active VGA/DVI/panel backend, physical SPI transfers,
or LZ4 encoding on an ESP32 SolarOS sender. Larger blocks and optimized bulk
RGB565 writes were not tested. The experimental opcodes remain private;
the protocol allocation and capability negotiation are not a production API.
