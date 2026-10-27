# Lossless block compression

Compression belongs to the sending connection. A display server advertises
decoder capabilities and accepts raw and supported compressed writes together.
Backends always receive decoded pixel bytes in the configured wire format.

The default sender behavior remains raw plus negotiated byte RLE. Opting in
with `dlp_host_t.compressed = true` before `dlp_host_extensions()` additionally
queries LZ4 support. Retain this setting across reconnects and renegotiate on
each fresh session. SolarOS exposes it as `compressed=1` on `spi-dlp` and
`stream-dlp` expansion attachments. The Linux driver uses `--compressed`.
This option does not change resolution, color depth, or pixel values.

## Capability discovery

After CONFIGURE, GET_CODECS (`0x08`) takes an empty request and returns exactly
four bytes: a little-endian codec capability mask. Bit 7 (`LZ4`, value 128)
advertises independent LZ4 block decoding with a maximum decoded size of
1,024 bytes. No other bits are currently defined for this query. A zero mask
or an older server's UNSUPPORTED response keeps the existing raw/RLE path.
Other errors and malformed responses fail negotiation.

LZ4 is deliberately absent from GET_INFO and GET_EXTENSIONS. Those responses
retain their existing masks because older clients reject unknown bits.
Opt-out clients do not issue GET_CODECS and do not send LZ4 writes.

## Compressed rectangle writes

RECT_LZ4 (`0x16`) uses the same header as RECT_RLE (`0x15`):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Frame ID, little endian |
| 4 | 4 | Rectangle ID, little endian |
| 8 | 4 | Offset in decoded rectangle bytes, little endian |
| 12 | 2 | Exact decoded byte count, little endian, 1..1024 |
| 14 | 1..206 | Independent LZ4 block |

The block follows the [LZ4 block format](https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md),
without an LZ4 frame, size prefix, dictionary, or references to previous
packets. Each block must decode to the exact declared count. Invalid lengths,
offsets, and truncated data are rejected before the backend write. Existing
rectangle bounds, pixel padding, session, sequencing, replay, CRC, window
credit, and PRESENT completion rules also apply. Both stop-and-wait requests
and windowed PUSH packets support RECT_LZ4. SPI and byte streams share the
same semantics.

The sender considers raw (up to 208 bytes), RLE, and LZ4 candidates, preferring
more decoded bytes per cell and then a smaller payload at equal decoded size.
It tries smaller LZ4 input blocks when a 1,024-byte block cannot fit in one
cell. Incompressible blocks stay raw. This is a bounded heuristic, not a
minimum-size encoder. Compression does not change the existing changed-region
planner or framebuffer ownership and scanout rules.

The C receiver reuses its existing 1,024-byte RLE scratch buffer. The portable
C encoder uses a local 256-entry, 16-bit hash table and no heap allocation.
Python senders use the `lz4` package only when LZ4 is requested and supported;
install it with `python3 -m pip install lz4` for the standalone scripts.
