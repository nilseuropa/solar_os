# Lossless DLP experiment

This archive describes the pre-production trials at the source revisions
recorded in the result metadata. Reproduce the overlays against those revisions.
The private trial query `0x08` differs from the production GET_CODECS query;
use the [production compression contract](../../protocol/display_link/COMPRESSION.md)
for current clients and firmware. Device restoration notes below describe the
state at the end of each trial.

The [Pico RAM follow-up](PICO.md) measures the RP2040 receiver without a panel,
including actual RAM-image checks, decoder timing, and a compatible CRC trial.

This is a reproducible USB receiver experiment on the Freenove ESP32-S3
Display 4.0 (FNK0104S), at 480×320 RGB565. It evaluates existing byte RLE,
changed-region selection, and an opt-in independent LZ4-block decoder.
The trial is kept outside the production protocol contract.

## Method

The installed 4.16.19 firmware was measured first. A temporary build from
the checkout recorded in `results/metadata.json` then ran all controls and
LZ4 cases on the same firmware. The running application partition was backed
up before the trial. Only that application partition was flashed.

Each case has one unmeasured preparation frame followed by three measured
updates. Times include USB upload, protocol processing, decode, and confirmed
PRESENT completion. Desktop encoding time is recorded separately. Checksum
queries and preparation frames are outside the measured upload time.

The deterministic input scenes are:

- `solid`: alternating red and blue full screens.
- `sparse-ui`: two 13×13 color indicators at opposite display edges.
- `scroll`: repeated text lines with changing line numbers; this is a synthetic
  terminal-like update, not a captured desktop scrolling workload.
- `gradient`: an animated RGB gradient, quantized once to input RGB565.
- `noise`: independently changing pseudo-random RGB565 images.

Losslessness is measured against these input RGB565 bytes. The LZ4 encoder
uses the system reference library and checks every block by reference decode.
The trial receiver hashes the decoded bytes passed successfully to its
framebuffer backend; the sender compares this hash and decoded-byte count on
every measured update. This checksum covers the rectangle byte stream, not a
physical screen readback.

## Encodings and region selection

`raw-full` and `rle-full` always replace the full image. `*-bands` uses the
existing eight-row bounding rectangles. `*-tiles` uses 16×16 changed tiles,
merged horizontally. Both delta strategies fall back to a full replacement
when their estimated raw packet cost is higher.

The LZ4 sender also tries the existing RLE and raw representations. It begins
with a 1,024-byte independent LZ4 block and halves the candidate when its
compressed bytes do not fit the 206-byte compressed-data budget of one cell.
The chosen block maximizes decoded bytes per cell, then prefers smaller
payloads for equal decoded lengths. This policy suits fixed SPI cells; a
compact USB stream could use a different cost model.

The decoder reuses the existing 1,024-byte decode scratch buffer. It does not
allocate memory, retain a cross-block dictionary, or allocate another image.
Private trial opcode `0x16` writes one LZ4 block. Private query `0x08` returns
trial version, scratch limit, decoded-stream FNV-1a checksum, and byte count.
These opcode values are experimental and are not an allocated DLP extension.

## Reproduction

Dependencies are Python 3, Pillow, pyserial, a system `liblz4.so.1`, and the
sibling `linux-dlp-driver` checkout. The receiver needs a free duplex USB
stream and a separate network console. Test scenes temporarily own the
selected display; cleanup stops the receiver and restores the previous
display session.

From the SolarOS project root, baseline measurement uses:

```sh
python3 -B experiments/dlp_lossless/benchmark.py \
  --host DEVICE_ADDRESS --port /dev/ttyACM0 --output /tmp/dlp-baseline
```

The reversible source overlay and trial checks use:

```sh
python3 -B experiments/dlp_lossless/trial_overlay.py apply
python3 -B experiments/dlp_lossless/test_trial.py
SOLAR_OS_DLP_LOSSLESS_TRIAL=1 /home/nils/.platformio/penv/bin/pio run \
  -e freenove_esp32_s3_display_4_0
```

After installing that trial application on the identified target:

```sh
python3 -B experiments/dlp_lossless/benchmark.py \
  --host DEVICE_ADDRESS --port /dev/ttyACM0 --output /tmp/dlp-trial \
  --modes raw-full,rle-full,rle-bands,rle-tiles,lz4-full,lz4-bands,lz4-tiles \
  --verify
```

Restore the source overlay with:

```sh
python3 -B experiments/dlp_lossless/trial_overlay.py restore
```

The script checks the modified files before restoration, so it will not
overwrite concurrent edits. `receiver.patch` records the exact overlay.

## Results (2026-10-20)

Medians of three updates on the same trial firmware. The comparison uses the
existing RLE/band policy and the trial tile/LZ4 policy; full-frame cases
automatically fall back to one full rectangle. Times exclude desktop encoding.

| Scene | RLE bands: USB bytes / ms | Tiles + LZ4: USB bytes / ms |
|---|---:|---:|
| solid | 382,324 / 1390.6 | 20,472 / 420.5 |
| sparse-ui | 18,616 / 256.2 | 510 / 209.9 |
| scroll | 7,996 / 251.7 | 5,493 / 234.6 |
| gradient | 381,796 / 1381.6 | 199,731 / 925.6 |
| noise | 382,324 / 1379.3 | 382,324 / 1336.5 |

All 105 measured updates matched the receiver decoded-stream checksum and
byte count. The receiver completed 140 presentations including preparation
frames; its mean display phase was 189.4 ms (maximum 194.4 ms).

Findings:

- Solid RGB565 colors expose byte RLE's weakness. Independent LZ4 blocks cut
  USB traffic by 18.7 times and upload-plus-presentation time by 3.3 times.
- The solid case uses 300 LZ4 data cells instead of 1,477 raw data cells. With
  unchanged fixed 256-byte SPI cells, this corresponds to about 4.9 times
  fewer data cells, not the 18.7-times compact-USB byte reduction. This is a
  packet-count calculation, not a measured SPI result.
- Sparse changes benefit strongly in transferred bytes, but RGB565 currently
  presents the full display for every update. About 190 ms of display work
  remains even when the upload is only 510 bytes.
- Tile selection alone is not always better: the synthetic text-line case
  sends 15,724 bytes with RLE tiles versus 7,996 with the existing RLE bands.
  A production sender should choose among band, tile, and full-frame plans.
- The gradient benefits moderately. Random noise uses raw fallback and sends
  exactly the same number of bytes. Its small timing difference is consistent
  with run-to-run variation; attempting compression adds desktop CPU work.

These results support an optional lossless block codec and adaptive region
selection. Carrying dirty RGB565 regions through presentation is the next
useful optimization on this particular panel. Larger negotiated blocks could
reduce packet overhead further; they were not tested here.

After measurement, the backed-up application was restored with flash hash
verification. Runtime checks confirmed 4.16.19, completed two USB DLP RGB565
frames, and confirmed the experimental query was unsupported. The receiver
was stopped and the display shell restored. The production source overlay was
removed, and the ordinary Freenove build succeeded without the trial flag.

## Evidence and limits

`results/baseline-frames.jsonl` and `results/trial-frames.jsonl` contain the
individual samples. Summary JSON files contain medians. Measurements are
specific to USB Serial/JTAG on ESP32-S3 and this display backend. They do not
measure Pico USB CDC, RP2040 decoding under scanout, SPI transfer, or encoding
on the SolarOS sender.

Targeted checks cover reference-library block comparisons, mutations and
truncations, 20,000 random malformed packets, output canaries, exact RGB565
reconstruction for all scene/mode combinations, and the production receiver
with both packet framings and injected loss, corruption, duplication,
reordering, and lost acknowledgements. The existing DLP host tests also pass.
