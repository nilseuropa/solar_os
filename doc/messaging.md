# SolarOS Messaging Architecture

SolarOS messaging is a provider-neutral set of bounded services. Gateway Chat
and MeshCore are providers of the same Contacts, Conversations, and Messages
model; neither provider owns the user interface or generic history.

SolarOS Link remains a separate wire protocol. The optional Link messaging
adapter projects its packet-sized text messages into the shared model. Link
streams use a distinct Link message type to provide peer-bound virtual serial
ports; they bypass Messaging conversations and the diagnostic Link receive
queue.

## Stable identifiers

Services exchange fixed numeric identifiers rather than internal pointers or
unbounded strings:

- contact ID: one local address-book entity;
- endpoint ID: one provider address belonging to a contact;
- conversation ID: one direct, group, room, or broadcast conversation;
- message key: one locally stable message identity;
- credential ID: one opaque record in the Credentials service.

Zero is the invalid value for every identifier. Public snapshot and visitor
APIs copy records while holding the owning service lock, release the lock, and
only then invoke callers or perform I/O.

## Providers

The assigned provider identifiers are:

| ID | Name | Purpose |
| --- | --- | --- |
| 1 | `gateway` | Existing SolarOS gateway Chat protocol |
| 2 | `meshcore` | MeshCore companion/chat provider |
| 3 | `link` | SolarOS Link text, direct, and broadcast projection |

A provider owns transport configuration, connection state, wire identifiers,
and provider-specific metadata. Generic services own contacts, trust,
conversation summaries, retained messages, delivery state, and the volatile
outbox.

When `radio-link` or `espnow-link` starts with `chat=on`, each 32-bit Link source
ID becomes a discovered endpoint and the active Link gets one broadcast conversation.
Accepted text is projected without consuming the bounded Link receive queue.
When that diagnostic queue is full it evicts its oldest copy; its capacity does
not limit Messaging delivery. Direct Link messages remain `sending` until their
acknowledgement arrives; broadcast messages become `sent` after radio
transmission. Link v1 provides no
encryption, authentication, fragmentation, routing, or automatic retry.
The adapter combines each source/sequence pair with a fresh local adapter
session epoch before deduplication, so restarting a Link transport cannot make
new messages collide with retained history when the Link sequence restarts at one.

## Contacts and endpoints

A contact contains a stable ID, display name, flags, timestamps, and one or
more endpoint IDs. `pinned` prevents automatic eviction.

An endpoint contains:

- stable endpoint and owning-contact IDs;
- provider identifier;
- binary provider address of at most 32 bytes;
- `discovered`, `trusted`, or `blocked` trust;
- bounded capability flags;
- last-seen time;
- at most 64 bytes of opaque provider metadata.

A valid signed MeshCore advert proves control of the advertised public key, not
the human identity behind it, so it creates a `discovered` endpoint. Trust is
per endpoint even when gateway and MeshCore endpoints are linked into one
contact. Blocked direct endpoints cannot publish or receive messages.

When the bounded contact store is full, Contacts may evict the oldest contact
whose endpoints are all discovered and whose `pinned` flag is clear. Trusted
and blocked records are never evicted automatically.

## Credentials

Credentials stores opaque provider records. The supported kinds are asymmetric
identity, shared key, and token. Public enumeration returns only record ID,
provider, kind, and label. Secret reads name one exact record and copy into a
caller-provided buffer.

Secret bytes are never published to Inbox, logs, autocomplete, native agent
tools, Python, or Lua. Temporary secret buffers must be wiped. SolarOS
currently stores NVS without flash encryption; physical flash access can
therefore recover these records.

SSH keys, native-agent API keys, and existing tokens are not migrated into this
service in this release.

## Conversations and messages

Conversation kinds are `direct`, `group`, `room`, and `broadcast`. A snapshot
contains provider, kind, title, endpoint or provider-group reference, unread
count, last-message time, and a security summary.

A message contains:

- stable message key and optional provider message key;
- conversation, contact, and endpoint references;
- receive and provider timestamps;
- inbound or outbound direction;
- delivery state;
- security flags;
- body of at most 4096 bytes and a truncation marker;
- linked Inbox ID;
- bounded error text.

Delivery states are `received`, `queued`, `sending`, `sent`, `delivered`,
`failed`, and `cancelled`.

Security flags are `encrypted`, `peer-key-known`, `peer-trusted`,
`shared-key`, `sender-unverified`, and `transport-secured`. MeshCore group
messages are shared-key encrypted but sender-unverified. The name embedded in a
group message is display data and never establishes contact identity.

## Provider boundary

Providers register and report status, upsert or remove conversations, publish
normalized inbound messages, consume only their own outbound requests, update
delivery state, and publish bounded cursor-based events.

Retained history remains visible when a provider is stopped. Chat reconciles
its display from the Messages generation instead of treating delivery events
as an independent history source, so an event-ring wrap cannot make a message
exist in `messages list` but disappear from Chat. With no explicit conversation
argument, Chat initially selects an unread conversation, preferring the most
recent one.

The 16-entry outbox contains only pending outbound requests. It is volatile: it
survives provider-job restarts but not a reboot. Sent, delivered, failed, and
cancelled messages remain in conversation history but leave the outbox. Use
`outbox` or `messages outbox` to list pending work and `outbox cancel
<hex-message-id>` to cancel one entry. Generic messaging invokes no provider
callback while holding its global lock.

Inbox is the cross-application notification projection for inbound items, not
the messaging API or owner of conversation history. Messaging first
publishes a normalized message, releases its lock, publishes the notification,
then links the returned Inbox ID using the message key and the current
generation. Marking a message read updates its linked Inbox entry after
releasing the messaging lock. `inbox clear` removes notification projections,
not retained conversation history. It also invalidates every Messaging-to-Inbox
link and persists that unlink, so a reset Inbox ID cannot later target a new,
unrelated notification. Message delete and mark-read verify the linked Inbox
entry's source message ID before changing it.

Inbox notification sound is a persisted policy and defaults to on when the
board has audio output. It can be disabled with `inbox notify off`.
Only a newly committed, non-duplicate entry can request it. Bursts are
coalesced, and the request is dropped while another audio user is active.
`service.audio-board` owns the bounded tone queue, built-in playback
serialization, and cancellation; Inbox never drives board audio directly.

Deleting a retained message also deletes its linked Inbox projection. Provider
history can be cleared independently with `messages clear gateway`,
`messages clear meshcore`, or `messages clear link`; `messages clear all`
clears all three. A clear is rejected while the selected provider has queued
outbound work, avoiding an outbox entry whose message has been removed. Clear
also sweeps the provider-owned Inbox source tags, so stale projections survive
neither ring wrap nor an in-flight publish; unrelated Inbox sources remain.
To remove both views, run `inbox clear` and the applicable `messages clear`
command; either order is safe.

## Persistence

Contacts uses a versioned, CRC-checked store with two alternating headers at
`/.contacts/contacts.bin`, capped below 24 KiB. It remains usable in volatile
mode and reports the storage error when persistence is unavailable.

Messages uses the same bounded fixed-slot and dual-header approach at
`/.messages/messages.bin` only when the active storage has the large-history
capacity already required by Chat. Small internal flash uses the compact
records physically shared with Inbox as its internal history backend. This is
reported as `compact internal` rather than exposed as an Inbox fallback.
Deletions rewrite the compact store atomically. Full-history deletions use CRC-checked
tombstones in their original fixed slots, so surviving records do not need to
be copied during provider-scoped clearing.

No `/.chat/messages.bin` records are migrated. After the new Messages store
initializes successfully, the obsolete Chat store is removed. Existing Inbox
notifications remain.

Persistence snapshots carry a generation. Filesystem writes and `fsync` occur
after releasing the service lock; the generation is rechecked before the
result is committed as current.

## User interfaces and scripting

The unified `chat` TUI renders conversations from every provider and does not
require Wi-Fi. `chat gateway`, `chat meshcore`, and `chat link` filter the
conversation list; `chat <decimal-conversation-id>` opens one conversation.
The selector is optional. Gateway configuration and room lifecycle belong to
the `gateway` shell command, not the provider-neutral TUI. Sending to a
discovered MeshCore endpoint requires interactive confirmation; shell and
scripting callers must pass `allow_untrusted=true`.

Contacts and Chat are resumable text TUIs that work through the common shell
I/O layer on display and VT100 port shells. They exit with `Esc` or `Ctrl+]`
according to the active terminal.

Inbox messages can be removed individually with `d` in either Inbox view or
with `inbox delete <decimal-id>`. Shared retained messages can be removed with
`messages delete <hex-message-id>`.

Provider receive queues, event rings, and the compact persistence ring are
bounded implementation details. They are not additional user mailboxes:
Messaging owns conversations and history, Inbox shows inbound notifications,
and Outbox shows pending sends.

Python and Lua receive bounded contact, conversation, and message snapshots
plus send, mark-read, and cancel operations. They cannot mutate trust or access
provider metadata or Credentials.

## Concurrency and memory rules

- Registry metadata is protected by short lock sections.
- Long-lived resources use generation-checked, reference-counted handles.
- No global lock is held across filesystem or NVS I/O, Inbox publication,
  provider callbacks, or radio operations.
- Large registries, message bodies, events, outbox entries, packet pools, and
  complete MeshCore provider state require external PSRAM.
- MeshCore must leave at least 64 KiB of internal SRAM free on the DevKit and
  at least 1 KiB of worker stack watermark while running.
