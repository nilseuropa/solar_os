# DLP windowed transfer profile, revision 2

This profile extends DLP's transport-independent session and frame contract.
It uses the existing checksummed compact packets or fixed SPI cells, with an
explicit negotiation before using the new packet kind. Legacy peers continue
to use stop-and-wait. GET_INFO and GET_EXTENSIONS retain their existing layouts.

The profile reduces protocol round trips and reverse traffic. It cannot remove
physical bandwidth, radio duty-cycle, connection-interval, bus rearm or panel
refresh limits. A transport adapter that itself uses stop-and-wait can remain
the bottleneck. Transport setup, fragmentation below a complete DLP packet,
SPI READY/RESET and channel scheduling remain adapter responsibilities.

## Negotiation and memory

After CONFIGURE, send NEGOTIATE_WINDOW (`0x06`), with two little-endian u16
values: profile version `2` and requested window `1..16`. A successful response
contains version `2` and the accepted window, no larger than requested or the
receiver's configured maximum. UNSUPPORTED selects legacy operation. Other
errors do not authorize windowed traffic. A successful CONFIGURE disables the
profile and clears all window state; renegotiate for each new session.

The portable receiver requires caller-owned window state, but no packet queue:
it applies valid packets in order and retains the last window's sequence/CRC
fingerprints. The host retains at most one negotiated window of exact packets
until acknowledged. The existing 220-byte payload and 1,024-byte RLE decode
limits continue to bound packet memory. Window size is a memory and latency
choice, not a promise of a particular throughput.

## Upload lane and credits

Packet kind PUSH (`3`) uses the existing request header and payload encodings.
Result and reserved fields must be zero. PUSH is permitted only after successful
negotiation, with the configured session and boot identity. Its sequence space
is independent of ordinary request sequences, starts at `1` and cannot wrap.
Reconfigure before sequence exhaustion.

Allowed PUSH operations are BEGIN_FRAME, BEGIN_RECT, RECT_DATA, RECT_RLE and
PRESENT. They retain their existing geometry, format, staged-image and full-
replacement validation. A frame cannot depend on an uncompleted predecessor.
PRESENT is the last upload operation for its frame. No submitted frame is
silently replaced or reported complete before backend presentation finishes.

The initial grant permits PUSH sequences `1..window`. The host must never send
above the latest grant. A progress response extends the grant to its applied
prefix plus one window, saturating at `UINT32_MAX`. A retransmission
uses the original sequence and exact original packet, and consumes no new grant.
Only the expected next PUSH can take effect; future packets within the grant
are discarded until the gap is repaired. This is bounded go-back-N recovery,
not an unbounded reorder buffer or selective-repeat promise.

A duplicate within the retained window must match its saved packet CRC. It
does not repeat a backend operation. Older duplicates are ignored. Conflicting
duplicates, credit violations or rejected upload operations latch the first
error and sequence; later uploads cannot execute or reach PRESENT. The host
must configure a new session and repaint fully after such an error. Ordinary
read-only diagnostics remain available. CRC is integrity checking, not sender
authentication or protection against deliberate forgery.

## Cumulative progress and presentation

WINDOW_SYNC (`0x07`) is an ordinary stop-and-wait control request. Its payload is
`last_sent_push u32, wait_frame u32`. Zero wait_frame requests upload progress;
a nonzero value requests the presentation outcome of that frame. Control
requests are serialized even while PUSH uploads are windowed.

On success the 32-byte response payload is:

| Offset | Type | Meaning |
|---:|---|---|
| 0 | u16 | Profile version, 2 |
| 2 | u16 | Negotiated window |
| 4 | u32 | Highest contiguous successfully applied PUSH |
| 8 | u32 | Highest granted PUSH sequence |
| 12 | u32 | First rejected PUSH sequence, or zero |
| 16 | u16 | Latched upload error, or OK |
| 18 | u16 | Backend frame error, or OK |
| 20 | u32 | Last completed frame |
| 24 | u32 | Last failed frame |
| 28 | u32 | need_full, zero or one |

A missing prefix produces an immediate progress response, even when a frame
outcome was requested. The host retransmits the unacknowledged suffix of its
retained window and sends a **new** WINDOW_SYNC request sequence. Exact retries
of the same control request replay that request's snapshot. Once the whole
window has applied, packets may be released from the host's retransmit storage.

If the prefix is complete but wait_frame is still presenting, the response is
deferred until completion or failure. The receiver continues ticking without
host traffic. Stream adapters drain `device_take_reply()` and preserve partial
writes; SPI adapters queue that same reply for a later transaction. SPI hosts
continue bounded IDLE exchanges while waiting. Retrying a pending control
request neither repeats PRESENT nor starts a second presentation.

The host can send a complete small frame, including PRESENT, in one window and
wait for one WINDOW_SYNC completion response. Larger frames use intermediate
progress checkpoints. An applied-prefix acknowledgement is not a presentation
acknowledgement. Retained host images become valid only after the matching
completed-frame outcome. Full presentation bounds apply to deferred completion
requests; upload checkpoints use the command bound.

## Failures and transports

Lost packets, duplicates, reordering, corrupt packets and lost progress replies
are handled within the bounded window. Framing remains independently checked
and resynchronizable. Retry deadlines are caller/adapter policy and must allow
the chosen link's propagation and serialization delays. Permanent link failure,
boot-identity change, malformed progress, upload rejection or presentation
failure invalidates the host cache. Reconnect starts a fresh session and full
repaint unless a future explicitly negotiated persistence contract says otherwise.
No exactly-once presentation guarantee survives receiver state loss.

UART, USB, TCP, BLE and radio byte-stream adapters carry identical packets and
state transitions. Reliable transports keep their own reliability; DLP's prefix
acknowledgements verify application progress and allow bounded recovery from
framing corruption or unknown completion. Datagram adapters must deliver whole
packets and service the control request/reply path; packet loss can use the same
go-back-N rules. SPI keeps fixed cells and one-transaction-delayed replies, while
PUSH cells no longer require an acknowledgement exchange after every operation.
