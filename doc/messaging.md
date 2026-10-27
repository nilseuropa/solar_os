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

## Provider classes and instances

A provider class supplies protocol capabilities and operations. Independent
instances carry stable numeric IDs, names, configuration, status, and runtime
contexts. Classes and instances register at runtime; enumeration follows the
registry rather than protocol enums or fixed provider arrays. Several IRC and
Gateway instances can run simultaneously, with independent transport state,
room membership, queues, and reconnect handling. Link projections bind separate
instances to named Link transports, each with its own send and acknowledgement
tracker. MeshCore instances carry separate local-radio or BLE-companion runtime
contexts. Each MeshCore job maintains all enabled bindings of its transport type;
both jobs can run concurrently. Duplicate physical-radio or companion bindings
are rejected. Hardware transport workers retain their resource ownership rules.

The original IDs retain their meaning as default instances:

| Instance ID | Name | Purpose |
| --- | --- | --- |
| 1 | `gateway` | Existing SolarOS gateway Chat protocol |
| 2 | `meshcore` | MeshCore companion/chat provider |
| 3 | `link` | SolarOS Link text, direct, and broadcast projection |

Additional instance IDs are allocated from 4 through 255, preserving the
byte-sized identity field in existing Contacts and Credentials files. This
identity-space bound is independent of the number of protocol classes;
runtime memory admission still limits active connections. Conversation keys,
contact endpoints, events, and outbound requests reference an instance, so
matching channel names or nicknames on different connections remain separate.
The existing history record layout remains compatible.

Classes expose optional create, configure, join, leave, direct-open, connect, and target
normalization operations. Messaging dispatches them outside its global lock.
Group backends can resolve a subscription and return its canonical conversation
through `join_open`, so membership does not allocate a placeholder room.
Chat and scripts consume common membership states: none, left, pending, joined,
and failed with a bounded reason. Class descriptors have static lifetime;
instance contexts are leased until each operation finishes. Removal is refused
while an operation is in progress. A class's `remove_prepare` excludes workers
until `remove_finish` commits destruction or resumes them after a storage error.
Registry labels remain stable and instance IDs are never reused.

`solar_os_messaging_instance_remove` requires lifecycle hooks before removing a
bound instance, including a default instance. It replaces the profile with a checked zero-payload record,
then destroys its runtime context, cancels queued work, and deletes its retained
messages, conversations, and Inbox projections. A reset after committing the
record completes the history purge during Messaging initialization. The record retains only ID, name, and class; no
connection settings or credentials remain. A committed record takes precedence
over a stale backup during recovery. Failure before committing the record leaves
the original profile and transport intact. File operations never hold the
Messaging state lock. IRC also excludes configuration edits and its worker
during the replacement, then frees the retired instance's PSRAM context.

Saved connection profiles use checked, versioned files in `.chat/profiles` on
mounted storage. The selected volume remains fixed during a boot. Temporary
files are verified before replacement and an interrupted replacement can
recover its backup. Gateway configuration migrates from NVS only after a
verified file round trip; legacy keys remain intact when saving fails. IRC
does not add NVS records. Its authentication settings are stored in its profile
and excluded from status and scripting enumeration.

Shared credentials, user radio profiles, and configured ESP-NOW peers use checked
files in `.settings` on mounted storage. Each store pins its selected volume for
the boot and recovers an interrupted replacement from a valid backup. Legacy NVS
blobs remain until a valid file has been committed. New settings writes use files;
there is no NVS fallback for mutations. Removing an instance also deletes its
shared credential records, and boot recovery repeats that purge for retired IDs.

`messages classes` lists registered classes; `messages instances` lists active
instances. `messages add gateway NAME URL [TOKEN]` creates a disabled Gateway
instance and `messages configure NAME URL [TOKEN]` changes its connection.
Use `messages connect NAME`, `messages join NAME ROOM`, and `messages remove NAME`
for its lifecycle. Chat exposes the same settings through `/add gateway NAME URL
[TOKEN]` and `/configure NAME URL [TOKEN]`. Configuration commands are omitted
from shell and Chat input history. `gateway` commands address the default Gateway.
`job start gateway-sync` maintains all enabled Gateway instances.

All installed backends expose setup through the same class registry. IRC accepts
`HOST PORT NICK`, MeshCore accepts `radio RADIO PROFILE` or `ble ADDRESS TYPE [PIN]`,
and Link accepts `TRANSPORT`. The `solaros.messages` Python and Lua submodules
expose `classes()`, `add(class, name, settings)`, and `configure(instance, settings)`;
settings are bounded lists/tables in shell argument order. Enumeration excludes
authentication material.

`messages add link NAME TRANSPORT` binds a Chat instance to a named Link
transport; two Chat instances cannot bind the same transport. Its transport job
must run with `chat=on`. `messages connect NAME` enables the projection and
`messages join NAME broadcast` selects its broadcast conversation. Direct
targets use hexadecimal device IDs with `messages query NAME DEVICE_ID`.
`messages configure NAME TRANSPORT` changes the binding. Removal detaches only
that Chat projection and deletes its history; a shared transport and its byte
streams continue running. Legacy `chat=on` starts reuse the saved projection for
that transport, or allocate one when no binding exists. The first legacy
projection uses default ID 3 when available. Bindings and enabled state are
saved in mounted storage.

Native IRC provides live TLS sessions with optional SASL, channels, private
messages, reconnect/rejoin, bounded buffers, and paced sends. It requests no
history or echo-message capabilities. A completed socket write becomes `sent`,
without a recipient-delivery claim. Interrupted sends fail without automatic
replay. See `manual/messaging.irc.md` for setup and controls.

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
tools, Python, or Lua. Temporary secret buffers must be wiped. The mounted
credential file is unencrypted; access to its volume can recover these records.

SSH keys, native-agent API keys, and existing tokens are not migrated into this
service in this release.

## Settings forms and messaging manager

The `messages` foreground app uses the common TUI layer on display and VT100
shells. It has Instances, Connections, and Messages tabs. Chat remains a separate
app, launched with `SOLAR_OS_LAUNCH_CHILD_RETURN` so the manager retains its
selected instance, view, and cursor.

Settings use `solar_os_tui_draw_form`, the shared scrollable form widget. The
widget renders the selected field's inline editor last and owns cursor position,
visibility, horizontal input scrolling, and masking. Apps supply row labels and
values, field actions, and save/cancel behavior. Draw surrounding screen elements
before the form and refresh after it.

Provider classes own `settings_read` and `settings_write` callbacks. A NULL
context describes or creates an instance; non-NULL contexts are leased for the
operation. Form indices enumerate backend variants. The generic settings API
validates field lengths against the provider's schema before saving. Secret
readback is empty with a separate presence flag; unchanged fields preserve saved
values and explicitly edited empty secrets clear them. Settings forms and
configuration copies belong in external memory, outside the foreground stack.

`solar_os_messaging_message_page_filtered` provides bounded archive pages with
optional unread filtering. Its offset counts matching messages. The browser
deletes a selected message by its stable key through
`solar_os_messaging_message_delete`; pending sends must be cancelled first.

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
`service.audio` owns the bounded tone queue, primary-backend playback
serialization, and cancellation; Inbox never drives an audio driver directly.

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
`/.contacts/contacts.bin`, capped below 192 KiB, with 512 contacts and 576
endpoints. Version 1 files migrate through a staged file and backup on the next
successful write, preserving IDs, names, trust, and provider metadata. Snapshot
I/O uses requests of at most 4 KiB, and a restart recovers a retained backup
when replacement was interrupted. Companion imports defer
persistence until the batch ends and reject overflow without evicting retained
contacts. It remains usable in volatile
mode and reports the storage error when persistence is unavailable.

The active MeshCore group namespace is selected when a transport synchronizes
its configured channels. Retained groups outside that configuration are
history-only; their messages remain readable and sends are rejected. Restored
groups stay history-only until synchronized. Conversation labels expose the
radio or companion transport without changing provider keys or message IDs.
The outbox is volatile. Restored outbound `queued` and `sending` messages become
`failed`, with an interruption reason, rather than implying pending work.

Messages retains received and sent records in `.messages/history.bin` on its
selected disk volume. The 64-entry PSRAM cache is independent of disk retention.
History grows with available disk space and does not overwrite older records.
Disk readers support complete conversation visits and bounded history pages.
Unread counts, replay cursors, deduplication, deletion, and delivery updates
include records outside the cache.

Each conversation tracks its message count, replay cursor, and recent record
offsets in bounded PSRAM indexes. Opening an empty conversation and marking an
already-read conversation read require no archive reads. Recent pages reuse
cached message bodies and read only missing records; older pages remain
available from disk.

Each record has two CRC-checked copies. An update syncs the inactive copy before
committing its state in memory. A fixed 256-bucket index in PSRAM follows
on-disk links for message lookup. Disk transfers and syncing release the
metadata mutex while a separate history mutex serializes history operations.
Visitor callbacks run outside the history and metadata locks.

New sends enter the volatile outbox only after their history record is durable.
Storage errors are reported rather than evicting accepted messages. Explicit
deletion writes a tombstone. Provider and conversation clearing compact the
archive through a staged replacement; interrupted replacements recover the
previous archive from its backup. Outbound IDs remain monotonic after clearing.

The selected archive path is recorded in internal flash. If SD mounts after
startup, an internal archive is copied, checked, and synced before the location
record is replaced. The internal copy is removed only after that commit.
Existing SD archives are not overwritten. A missing selected volume leaves
history unavailable until it returns; status queries retry attachment without
replacing the saved history with an empty store.

The previous `.messages/messages.bin` ring and compact Inbox records migrate
through a staged archive, including the internal Inbox when SD is mounted.
Original files remain intact. The obsolete `.chat/messages.bin` format is not
imported or removed.

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

Provider receive queues, event rings, Inbox notifications, and the RAM cache are
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
