# Link stream protocol v3

Link streams carry ordered duplex bytes inside unicast Link messages of type
`SOLAR_OS_LINK_MESSAGE_STREAM` (4). The outer Link packet format is unchanged.
The stream header is 20 bytes; multi-byte fields use network byte order.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 2 | Magic `0x53 0x56` (`SV`) |
| 2 | 1 | Stream version, 3 |
| 3 | 1 | Opcode: OPEN=1, DATA=2, ACK=3, CLOSE=4 |
| 4 | 4 | Sender's nonzero transmit session epoch |
| 8 | 2 | DATA sequence; zero for control frames |
| 10 | 4 | Session being acknowledged, or zero if unknown |
| 14 | 2 | Last contiguous DATA sequence received in that session |
| 16 | 4 | Absolute receive-window right edge in that session |
| 20 | variable | DATA bytes; empty for control frames |

Each direction starts at sequence 1 and skips zero on 16-bit wraparound.
Acknowledgement sequence zero means no DATA has been received yet. It may carry
initial credit when the acknowledgement session is nonzero. An unknown
acknowledgement session requires both its sequence and right edge to be zero.
ACK frames require a known acknowledgement session. The stream version must
match; there is no v2 fallback.

## Delivery and credit

Each receive epoch starts with a zero byte count and a 2048-byte queue. Its
advertised right edge is the count of accepted bytes plus current queue space:
equivalently, bytes consumed by the application plus 2048. The counter wraps
modulo 2^32. Credit is an absolute edge, not an increment that could be applied
twice after duplication.

The sender reserves byte credit when moving data from its TX queue into its
bounded retransmission window. New bytes must fit below the advertised edge;
retransmission consumes no additional credit. Credit applies only when its
acknowledgement session matches the current transmit epoch. A newer edge
replaces the previous edge using signed modular subtraction. The outstanding
range stays far below 2^31 bytes, including at counter rollover.

DATA is delivered to the receive queue only at the expected sequence. Recent
duplicates are acknowledged without delivering them again. Later sequences
are discarded; the sender retries from its oldest unacknowledged frame after
the retry timeout (go-back-N). Cumulative acknowledgements release the matching
prefix of the transmit window. Ordering and duplicate suppression apply within
an epoch; closing a port discards its queued data.

Acknowledgements and credit updates piggyback on every outbound frame once the
peer epoch is known. Standalone updates use a timer that starts with the first
pending update and is not extended by incoming bursts. A batch of at least two
new frames, or half the configured frame window when larger, makes the ACK due
immediately. Due ACKs take priority over continued DATA transmission. Application
reads schedule credit updates even when no DATA is waiting in the other
direction. A sender with queued data, no outstanding frames and no credit sends
an OPEN probe at the retry interval; repeated OPENs request a rate-limited
response containing the current credit. This recovers lost window updates.

## Sessions and transport policy

Opening a port creates a new random nonzero transmit epoch. A changed peer OPEN
resets the receive sequence, credit counter and RX queue. An established local
transmit direction rotates its epoch and resequences outstanding bytes once;
until its new epoch is confirmed by a matching DATA acknowledgement it does not
rotate again in response to a peer reset. Periodic OPENs with the same epoch do
not reset delivery. A temporary transport stop retains endpoint epochs, receive
state and buffered data, and retries outstanding frames when the transport
returns. A send completion from an earlier epoch cannot update the new epoch.

The default policy uses eight frames, a 10 ms ACK timer, a retry interval of
800 ms plus 0–400 ms deterministic jitter, a 15 s OPEN interval and a 45 s peer
timeout. Sparse writes are sent immediately; queued bursts fill the available
window without a consumer-specific bulk-mode option. MeshCore uses two frames,
a 250 ms ACK timer, 12–16 s retries, 30 s OPENs and a 180 s peer timeout.
Frame data capacity is the outer Link payload MTU minus 20 bytes: 216 bytes on
ESP-NOW and 78 bytes through the current MeshCore stream envelope.
The raw `radio-link` worker caps its effective window at one frame: its
half-duplex send loop only returns to receive when the transmit queue is empty.
`link stream status` reports the effective window. ESP-NOW and MeshCore use their
configured eight- and two-frame windows respectively.

The stream engine queues a frame only when the transport's Link transmit queue
is empty. Remaining bytes stay in the stream window or TX queue. This limits
queueing latency, leaves space for packet clients, and handles transport
backpressure without repeatedly attempting to enqueue into a full queue.
Streams sharing a Link instance take turns, so a sustained bulk writer cannot
starve another stream on that transport.

The port, stream, shell, bridge and DLP consumer APIs are unchanged. Existing
stream endpoints require firmware that supports v3 on both ends. Packet Chat
and standalone Link packet consumers retain their existing wire format.
