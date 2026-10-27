# Display Link Protocol library

Display Link Protocol (DLP) connects a host framebuffer to a peripheral that
owns a physical display. Dimensions, pixel formats, presentation limits and
optional features are discovered. The protocol is independent of the MCU,
panel bus, operating system and transport API.

The portable C11 library includes serialization, CRC32, host/device state
machines, request replay, asynchronous presentation, negotiated upload windows,
byte-stream framing,
changed-region planning and bounded byte RLE. It has no allocation, SDK,
RTOS, framebuffer or GPIO dependency. It is MIT licensed.

Include `dlp.h` for the `dlp_*` API and `DLP_*` constants. Existing `sdlp.h`
names remain source compatible, and the implementation stays in `src/sdlp.c`.
Recompile consumers when updating the library: caller-owned structures have
grown. The original SDLP v1 SPI wire format is retained.

## Build and test

```sh
cmake -S protocol/display_link -B /tmp/display-link-build -DSDLP_BUILD_TESTS=ON
cmake --build /tmp/display-link-build
ctest --test-dir /tmp/display-link-build --output-on-failure
python3 -m unittest discover -s tests -p test_display_link.py
```

The Python runner adds AddressSanitizer and UBSan. Consumers compile
`src/sdlp.c` and add `include/`, or use the CMake `display_link` target.

## Transports

Commands have the same semantics over either transport profile:

- **Ordered byte stream:** UART, USB CDC, TCP or another ordered read/write
  stream. Compact `DLP1` packets carry only the header, used payload and CRC.
  Requests receive direct responses. Packet boundaries need not match reads
  or writes. Control requests use stop-and-wait sequencing; negotiated uploads
  use bounded windows with cumulative progress and completion replies.
- **SPI:** mode 1, fixed 256-byte full-duplex `SDLP` cells. Responses appear in
  later transactions; the host polls with IDLE cells. CS, clock, minimum
  spacing, optional READY/RESET and bus arbitration belong to the adapter.
  Byte streams can also carry these fixed cells for legacy compatibility.

A stream-only descriptor may set `max_clock_hz=0`. SPI discovery rejects
such endpoints. Baud rate, socket addresses, USB configuration and connection
establishment are outside DLP.

### Byte-stream host

Supply `dlp_stream_io_t` with a monotonic microsecond clock, delay callback,
and nonblocking `read`/`write` callbacks. Return the number of bytes transferred,
zero for temporary backpressure, or a negative number for an I/O error.
Initialize `dlp_stream_host_t`, obtain `dlp_stream_transport()`, and pass that
to `dlp_host_init()`. The adapter handles partial I/O, CRC checks, response
matching, deadlines and exact retries. Its default retry interval is 250 ms;
set `retry_us` to suit the link. Set `legacy=true` for a fixed-cell peer.
The C adapter does not automatically negotiate legacy framing.

A custom adapter can implement `dlp_transport_t.request()` directly. It takes
and returns canonical 256-byte library cells; the adapter owns their framing
on the actual link. Supply either `request` or `exchange`.
For windowed uploads on a custom stream adapter, also supply `push`: it writes
one canonical cell before the absolute deadline without waiting for a reply.
SPI reuses `exchange` for PUSH cells.

### SPI host

Supply `dlp_transport_t.exchange()` with a bounded full-duplex exchange.
It owns CS, minimum CS-high spacing, optional READY, and shared-bus locking.
Respect the absolute deadline and return with CS inactive and the bus released.

## Host lifecycle and retained images

Discover before an absolute boot deadline, configure a fresh nonzero session
ID and accepted format, then call `dlp_host_extensions()`. An older peer's
UNSUPPORTED reply disables extensions without failing configuration.
Optionally attach caller-owned `dlp_host_window_t` storage with
`dlp_host_negotiate_window(host, storage, requested_size)`, requesting 1..16
packets. UNSUPPORTED returns OK with `storage.active=false` for legacy peers.
The high-level uploader then uses the accepted window automatically; manual
frame commands remain synchronous. Calls and control requests stay serialized.

`dlp_host_upload_frame()` accepts a packed native-format image and optional,
distinct, equally sized retained storage plus a validity flag. Set validity
false initially and after every reconfiguration. Without a valid cache it
uploads a full replacement. Otherwise it compares eight-row bands and sends
tight byte-aligned changed rectangles, splitting them at `max_rect_bytes`.
If the estimated raw packet count is no better, it sends a full image.
This planner bounds memory and command overhead; it does not promise a
minimum-size encoding.

`PRESERVE_CONTENT` is required for partial updates. New `RLE` support lets
`dlp_host_write_some()` choose compressed or raw packets. Raw writes carry up
to 208 bytes; RLE packets expand to at most 1,024 bytes. Each packet is
independent. Incompressible input stays raw.

Optional negotiated LZ4 blocks are described in [Lossless block compression](COMPRESSION.md).
Set `host.compressed = true` before extension negotiation to opt in.

The retained cache is committed only after PRESENT completes. Errors invalidate
it. An unchanged image is skipped only after GET_SYNC confirms that this
session's retained content is still valid. If another source replaced it,
RESET/power loss invalidated it, or a partial BEGIN_FRAME is rejected, the host
uses a full replacement. Windowed upload rejection requires a new session;
the caller must reconnect and repaint rather than continue the rejected window.
Multi-source peripherals must mark affected sessions'
`need_full=true` whenever their retained content ceases to be valid.

Lower-level APIs expose BEGIN_FRAME, BEGIN_RECT, raw/sequential writes,
PRESENT, abort, status and optional controls. Writes may cross rows. Retry
identical request bytes and sequence numbers only; accepted requests are
replayed without applying their effects again. BUSY requires a new command
sequence for a new attempt.

Host APIs return a wire result or a negative local error. After a transport
timeout, the outcome is unknown and the configured state is invalidated.
Rediscover, configure a new session and repaint fully. A pending presentation
may temporarily reject configuration with BUSY. Calls are synchronous and
must be serialized by the owner.

## Peripheral adapter

Initialize `dlp_device_t` with an immutable descriptor, a nonzero boot ID that
changes after protocol state loss, and backend callbacks. The backend owns
its display representation. State, replay and the 1,024-byte decode scratch
buffer are caller-owned; the library does not allocate memory.

For streams, zero-initialize a `dlp_stream_t` parser and feed arbitrary input
bytes through `dlp_stream_feed()`. On a
complete packet, pass the canonical cell to `dlp_device_receive()` and frame
the response using `dlp_stream_encode()` with the incoming packet's compact
flag. Drain any buffered packet with `dlp_stream_next()` before waiting for
more input. Keep replies queued across partial output writes. The adapter
owns an idle-packet timeout: discard an incomplete parser buffer after a
suitable idle interval so truncated headers cannot strand short requests.
CRC/length checks otherwise scan past corrupt traffic to recover framing.

For SPI, arm TX with `dlp_device_idle()`. After CS rises, pass only a complete,
correctly sized cell to `dlp_device_receive()`. Queue a returned response for a
later transfer. A false return means retain the previous TX cell.

Call `dlp_device_tick()` regularly without host traffic. `present()` starts
bounded asynchronous work; `poll_present()` returns BUSY until completion.
Other callbacks finish within `command_ms` and the adapter's rearm interval.
The library creates no workers/interrupts and is not reentrant.

To support windows, attach caller-owned `dlp_device_window_t` with
`dlp_device_enable_window(device, storage, maximum_size)` before configuration.
Tick independently of incoming traffic, then drain `dlp_device_take_reply()`
for deferred completion replies. Preserve the pending request's stream framing
and partial output writes. SPI adapters queue these replies for later exchanges.
Do not overwrite an undrained reply. A receiver without attached window storage
returns UNSUPPORTED and keeps the original behavior.

Backend callbacks return DLP wire results. BAD_ARGUMENT, BAD_STATE, BUSY,
UNSUPPORTED and NO_MEMORY leave the operation unapplied. IO_ERROR or TIMEOUT
may indicate partial effects and require reconfiguration/full repaint.
RLE length, rectangle bounds and row-padding bits are checked before any
backend write. Never return OK for dropped work.

## Wire profiles and extensions

All integers are little endian except RGB565 pixel bytes. Both profiles use
this 32-byte header:

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 4 | `DLP1` compact stream or `SDLP` fixed cell |
| 4 | 1 | Version 1 |
| 5 | 1 | Reserved zero |
| 6 | 1 | Kind: IDLE=0, REQUEST=1, RESPONSE=2, negotiated PUSH=3 |
| 7 | 1 | Opcode |
| 8 | 4 | Session |
| 12 | 4 | Sequence |
| 16 | 2 | Result (zero in requests) |
| 18 | 2 | Payload length, 0..220 |
| 20 | 8 | Boot identity |
| 28 | 4 | Reserved zero |

Compact packets append the used payload then CRC32 of header+payload: total
`36 + payload_length` bytes, at most 256. Fixed cells zero-pad the payload to
220 bytes and append CRC32 at offset 252. CRC uses reflected polynomial
`0xedb88320`, initial/final inversion (the usual CRC32).

New optional commands use normal configured session/sequence/replay rules:

| Opcode | Request | Successful response |
|---|---|---|
| GET_EXTENSIONS `0x04` | Empty | u32 feature bits: RLE=32, SYNC=64 |
| GET_SYNC `0x05` | Empty; requires SYNC | u32 `need_full`, zero or one |
| NEGOTIATE_WINDOW `0x06` | version u16=2, requested size u16=1..16 | version u16=2, accepted size u16 |
| WINDOW_SYNC `0x07` | last sent PUSH u32, wait frame u32 (0 for progress) | 32-byte cumulative progress/completion |
| RECT_RLE `0x15` | frame u32, rectangle u32, **decoded** offset u32, decoded length u16, RLE bytes | Empty |

GET_INFO keeps its original capability bits (0..4) so old clients continue to
accept descriptors. New hosts query extensions after CONFIGURE. Unknown
commands return UNSUPPORTED; new data encodings are used only when advertised.

RLE is a sequence of byte tokens. A tag 0..127 copies the following `tag+1`
literal bytes. A tag 128..255 repeats the following byte `(tag & 127)+3` times
(3..130). Tokens must consume the packet exactly and expand to exactly the
advertised decoded length (1..1,024), within the open rectangle. Offsets and
row-padding checks apply to decoded bytes. No dictionary or state survives
between packets.

See [the windowed profile contract](WINDOWED.md) for credits, separate PUSH
sequences, bounded go-back-N retransmission, duplicate handling and deferred
completion. Framing version 1 is retained; profile revision 2 is explicitly
negotiated. Small frames need one completion round trip. Larger frames use
intermediate progress checkpoints. Physical link and display limits still apply.

MONO1/GRAY2/GRAY4 pack low bits first with zero unused row bits; MONO1 black
is one, grayscale white is maximal. GRAY8 has one byte per pixel; RGB565 is
big endian. Rectangle rows are padded independently, using their own width.

## Reference peripheral and host integration

The sibling `pico_display_link` repository vendors this library; its explicit
`scripts/sync_protocol.py` refreshes the copy. Its Python `DisplayLink` client
uses the same stream framing, delta and RLE rules. SolarOS's `spi-dlp` and
`stream-dlp` drivers share a host core for discovery, uploads and presentation.
`stream-dlp` claims an existing exclusive duplex byte stream; its provider owns
UART, TCP or radio setup. Private Python/Lua `solaros.dlp.connect` objects use
the same core without registering a named display.

SolarOS can also act as the display peripheral: `dlp serve <display>
stream=<byte-stream>` runs a receiver on a named display. The
`scripts/dlp_image.py` desktop script sends test images to that receiver.
