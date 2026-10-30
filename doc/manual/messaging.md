+++
id = "messaging"
title = "Messaging, contacts, and credential security"
section = "service"
summary = "Manage contacts, conversations, trust, and saved credentials"
aliases = ["contacts.service", "credentials"]
keywords = "messaging contacts credentials trust endpoint gateway meshcore security"
packages_any = ["service_contacts", "service_credentials"]
+++
# Messaging identities and security

Run `messages` without arguments to open the tabbed instance, connection, and
message manager. It supports provider settings forms, connection diagnostics,
conversation and unread browsing, pending-send cancellation, and deletion of
individual local messages. `c` opens the existing Chat as a child and returns
to the browser when Chat closes. See [Application controls](apps.md#messages).

To start a direct conversation by name, open `contacts`, select the person,
and press `c`. Chat opens their conversation and returns to Contacts when
closed. The Chat application must be included in the firmware.

Provider classes describe protocols; named instances describe individual
connections. Multiple IRC instances can share a class while keeping server
settings, rooms, nicknames, and message routing separate. Chat groups by
instance and accepts `chat INSTANCE`, `chat CLASS`, or a conversation ID.
Use `/join INSTANCE TARGET`, `/query INSTANCE NICK`, and `/leave` in Chat,
or `messages join|query INSTANCE TARGET` and `messages leave CONVERSATION_ID`
from the shell. See [Native IRC](messaging.irc.md).

`messages remove INSTANCE` (name or ID), or `/remove INSTANCE` in Chat, removes
a configurable provider instance. It closes the connection, stops retries,
cancels queued sends, and removes the saved connection settings and credentials.
Local messages, conversations, and associated Inbox notifications are deleted.
Gateway, MeshCore, and Link support removing their default instances too.
Removal requires writable mounted storage.

`messages classes` lists provider classes; `messages instances` lists configured
connections. `messages add gateway NAME URL [TOKEN]` creates a Gateway connection.
`messages add link NAME TRANSPORT` creates a Link projection. Enable it with
`messages connect NAME`, and run the corresponding transport job. Chat accepts
the same arguments after `/add`, and changes settings with `/configure NAME ...`.
MeshCore settings are `messages add meshcore NAME radio RADIO PROFILE` or
`messages add meshcore NAME ble ADDRESS TYPE [PIN]`; see [MeshCore](meshcore.md).

| Class | Settings after `messages add CLASS NAME` | Worker |
| --- | --- | --- |
| `gateway` | `URL [TOKEN]` | `gateway-sync` |
| `irc` | `HOST PORT NICK` | `irc-sync` |
| `meshcore` | `radio RADIO PROFILE` or `ble ADDRESS TYPE [PIN]` | `meshcore` or `meshcore-ble` |
| `link` | `TRANSPORT` | Its `radio-link` or `espnow-link` transport with `chat=on` |
| `sc2` | `DOMAIN [ADMIT] [key=B64]`, or `chats://HOST:PORT[#pin=sha256/B64] [ADMIT] [key=B64]` or `chat://HOST:PORT [ADMIT] [key=B64]` | `sc2-sync` |

New instances start disabled. Use `messages connect NAME` and start the worker.
For SC2, use `messages connect NAME` and `job start sc2-sync`.
Enabling or configuring an instance, restoring it after a reboot, and sending
a message do not start the job. Messages queue while the worker is stopped.
Use `job stop sc2-sync` to stop the worker, or `messages disconnect NAME` to
disable one instance. Configured job autostart can start the worker at boot.
`messages configure NAME SETTINGS...` changes connection settings; IRC also
accepts `sasl ACCOUNT PASSWORD`, `sasl off`, `pass PASSWORD|off`, and `ca FILE|default`.
Chat exposes `/classes`, `/instances`, `/add CLASS NAME SETTINGS...`,
`/configure NAME SETTINGS...`, and `/remove NAME`. Python and Lua expose the same
class registry and instance operations through `solaros.messages`.

A small record containing only the retired instance ID, name, and class stays
on mounted storage. Retired IDs are never reused, so contacts and retained
messages cannot be routed through a different account. Creating the same name
again creates an independent instance. `messages clear INSTANCE_ID` deletes
history without removing an active instance. Interrupted removals finish deleting
history on the next boot.

SolarOS separates messaging identity from transport. A contact is one local
address-book entry, while each gateway, MeshCore, or SolarOS Link address is an
endpoint with its own trust and capabilities. Linking endpoints does not copy
trust between providers.

Trust has three states:

- `discovered`: a provider supplied a valid address or advert;
- `trusted`: the user explicitly accepts that endpoint identity;
- `blocked`: direct inbound and outbound messaging is denied.

A signed MeshCore advert proves ownership of its public key, not the human
identity using the device. New adverts therefore remain `discovered` until the
user trusts them.

Contacts can retain up to 512 contacts and 576 endpoints. They are saved in
`/.contacts/contacts.bin` (under 192 KiB). If storage is unavailable, contacts
remain usable for the current boot; check `contacts status` for save errors.
Imports keep existing contacts and report skipped entries when full.

The credential store holds provider keys and tokens. Secret values are not
shown in Inbox, logs, autocomplete, agent tools, Python, or Lua. They are saved in
`.settings/credentials.bin` on mounted storage. Existing NVS records migrate only
after a checked file is committed. The file is not encrypted. SSH keys, agent API keys, and other provider tokens keep their separate
stores.

See `contacts` for the TUI and mutation commands. `chat` is the unified
conversation UI and works without Wi-Fi; provider jobs connect transports when
available. The `messages` shell API exposes the same disk history.
Stable 64-bit message IDs are printed as hexadecimal strings so they
survive shell and scripting runtimes without precision loss.

Received and sent messages are saved in `.messages/history.bin` on the selected
storage volume. The 64-message RAM cache does not limit disk history. History
grows with available disk space; gateway sync and cache eviction do not delete
older messages. Chat loads each conversation from disk. Use Page Up and Page
Down to move through older and newer history pages. `messages list ID` reads
the complete saved conversation.

SolarOS selects mounted storage when creating the archive and saves that
location in internal flash. If SD mounts after startup, an internal archive
moves to SD through a checked, synced copy before the saved location changes.
Existing files on SD are not overwritten. An unavailable saved volume is retried
when it returns; a new empty archive does not replace its history.
If storage is unavailable or a write fails, `messages status` reports the error.
New sends are queued only after their history is saved.

Existing full message records and incoming message records from the compact
Inbox store migrate to the archive. Their original files are retained.

Direct messages to a blocked endpoint are always rejected. A discovered
endpoint requires an explicit second confirmation in Chat or
`--allow-untrusted` in the shell. That opt-in applies only to the one send and
does not change the endpoint's trust state.

MeshCore group labels distinguish `[radio]` from `[companion]`. A retained
group outside the current transport or channel configuration has a `(history)`
label: its messages remain readable, but it cannot accept new sends. Outbound
queues are held in memory; after a restart, retained `queued` or `sending`
messages become `failed` and must be resent explicitly.

## Security flags

Conversations and messages carry a `security` bit set, which
`messages conversations` and `messages list` print in hex, and which Python and Lua
expose as `security_flags`. A conversation also inherits the bits of its messages.

| Bit | Name | Meaning |
| --- | --- | --- |
| `0x01` | encrypted | Content is end-to-end encrypted |
| `0x02` | peer key known | The peer's public key is known |
| `0x04` | peer trusted | The user trusted this endpoint |
| `0x08` | shared key | Encrypted with a key shared by a group |
| `0x10` | sender unverified | The sender could not be verified |
| `0x20` | transport secured | The link to the server uses TLS |
| `0x40` | public signed | Not encrypted; every message is signed by its sender and was verified |
| `0x80` | legacy insecure | Legacy protocol without end-to-end protection or sender authentication |

An `sc2` instance is one SC2 relay. All relays share the device identity in
`/.sc2`. Its rooms are `pub:NAME` public rooms, marked `public signed`, and
32-hex private rooms, marked `encrypted` and `shared key`. Direct
conversations use the peer's 32-hex device ID and are marked `encrypted`; they
also get `peer key known` once the peer's key bundle is stored. `messages
configure NAME admit CODE|off` and `pin sha256/B64|off` change the admission
code and the TLS pin. With a pin, the pinned key alone authenticates the relay
(a self-signed certificate works; host name and expiry are not checked);
without one the certificate must chain to the ESP-IDF CA bundle. `sc2-sync`
connects the first enabled `sc2` instance; `sc2 relay` shows its state.
`messages query NAME PEER` and `sc2 safety PEER` take the 32-hex device ID, a
unique prefix of at least 6 hex digits of a peer whose keys or session are
stored, or a nickname saved with `sc2 alias NAME DEVICE`.

Private rooms: `sc2 room create "Team room"` makes a named end-to-end encrypted room on the
primary relay with this device as its admin and prints its invite, a
`sc2i:...` text that holds the room's invite secret. Whoever has the invite
can join, so pass it on privately (`sc2 room invite ROOM` prints it again).
Names may contain spaces and are limited to 48 UTF-8 bytes. The encrypted
membership checkpoint supplies the name to joining devices. Names are display
labels; the room keeps its stable 32-hex ID. `sc2 room invite "Team room"`
accepts a unique exact name, and room names and IDs support Tab completion.
Omitting the name retains the `room XXXXXXXX` label.
On the other device, `sc2 room join sc2i:...` (or `sc2 room join @FILE` when
the invite is longer than the input line) asks to join. The joining device
also sends the admin an encrypted proof of invite possession. The admin checks
that proof before admitting the member and sending its room key and membership
checkpoint. A relay notification alone cannot admit a device. Both devices
need firmware with this admission exchange; older clients remain waiting.
Both rooms then appear as `room XXXXXXXX`
conversations: use `messages send` as for any room, and `messages leave` to
leave one (this also deletes the device's copy of the room record; an admin
that leaves cannot invite again). `sc2 rooms` lists the instance's rooms with
their state (`waiting for an admin`, `waiting for the room key`, `joined`).
Room keys are accepted only from the invited admin after membership validation.
Later membership changes require an authorized admin signature and a matching
commit chain. Sending waits for the new epoch key after a membership change.
Private text messages carry the sender's SolarOS `user@hostname` label inside
the authenticated encryption. Public posts use that identity as their signed
display name. Older peers without a sender label appear by device ID. Display
names do not replace device keys or safety-number verification.
Invites cannot be revoked from the device yet, and members cannot be removed;
create a new room instead.

Safety numbers: in Chat, `/safety` on an sc2 DM (or `/safety PEER`) opens a
screen with the short code, the eight words and the full number; compare them
with the other device, then press `V` to mark the peer verified (`U` takes it
back). `/verify` opens the same screen, `/unverify` clears the mark. From the
shell: `sc2 safety PEER`, `sc2 verify PEER`, `sc2 unverify PEER`. The mark is
stored in the SC2 store and Chat labels sc2 DMs `(verified)` or `(unverified)`.
The device ID is a hash of the peer's identity key, so a peer cannot change its
key under a verified ID. Public sc2 rooms are labelled `(public)`; the first
time one is opened in Chat (or joined with `messages join`) a notice explains
that posts are signed and non-deniable. It is shown once per room.

Adding a relay by domain: `messages add sc2 NAME DOMAIN [ADMIT] [key=B64]`
fetches `https://DOMAIN/.well-known/solaros-relay`. The connection must
verify against the CA bundle and the host name; there is no plain-HTTP,
unverified or pin-on-first-use fallback, and a failure stops with
`messages: refused: ...`. The relay's signed descriptor gives the
`chats://` endpoint (port, optional pin) and up to three fallback relays,
each with its key. The descriptor must verify against the relay key it
names. With `key=B64` (the `relay_key` from the operator) that key must
match as well. The relay key and the fallback keys are pinned in the
instance. At every login the relay must sign a fresh nonce with its pinned
key. A different key is a hard failure: `sc2 relay` shows "relay key
differs from the pinned key", and the pin is never changed by itself. The
one exception is a rotation signed by the pinned key, which the relay
announces at login (one step). To accept a new relay key otherwise, remove
the instance and add it again. `messages configure NAME key B64|off` sets
or clears the primary's pin, `URL key=B64` sets it together with a new URL
(a new URL without `key=` drops the pin), and fallback keys go after each
fallback URL: `fallback URL key=B64 URL key=B64`. The fetch uses one
1.6 KiB buffer while it runs.

Fallback relays: `messages configure NAME fallback URL [URL URL]` gives an
`sc2` instance up to three fallback relays (same URL forms as the primary,
each with its own `#pin=`); it replaces the whole list, `fallback off` clears
it. `sc2-sync` keeps one connection. After two failed attempts in a row (the
normal reconnect backoff paces them) it moves to the next relay. On a fallback
it probes the primary every 15 minutes and stays there once it works. On the
primary it visits each fallback in turn for about 30 seconds every 15 minutes
(every minute until each relay has seen this device once) to publish the key
bundle and collect DMs that peers sent there; visits wait while messages are
queued. `sc2 relay` shows the relay in use. Trade-offs:

- The identity, admission code and room list are the same on every relay;
  the admission code must work on all of them.
- One-time prekeys are published on the primary only, so the same prekey can
  never be claimed on two relays. A peer that starts a new session through a
  fallback uses the signed prekey alone; the first message of that session
  then lacks the extra forward secrecy a one-time prekey gives. Existing
  sessions are not affected.
- Peers only meet on a relay they both use. If you and a peer are on
  different relays, a DM waits in the other relay's mailbox until the next
  visit (up to about 15 minutes).
- A public room on two relays is one conversation: posts from each relay are
  kept apart by relay index, so each relay resumes from its own position.
  Reordering the fallback list may re-fetch or skip some recent history.
  Private rooms live on the primary relay: while a fallback is in use they
  wait, and a message to one fails with "private room is on the primary
  relay".
- Queued messages are sent through whichever relay is in use, never during a
  visit.

Reconnects: a relay may close a session on purpose and says why before it
does. A session that only sent keepalive pings for the relay's idle timeout
(`408 idle`), one that reached the relay's session lifetime
(`503 reconnect`) and an idle session closed to make room on a full relay
(`503 evicted`) are normal: `sc2-sync` reconnects after one to three
seconds, does not count a failed attempt and does not move to a fallback.
DMs wait in the relay's mailbox and rooms catch up at the next sync. When
the same device key logs in on another connection, the relay closes the
older session with `409 replaced`; `sc2-sync` then waits 4 seconds, longer
each time it happens again in a row (up to a minute), and stays on the same
relay. Two devices that share one identity keep replacing each other this
way; give every device its own. Every other drop, including
`503 server_full`, goes through the backoff.

A device can also be the relay: `job start chatd` serves a small SC2
mini-relay (login, DM mailbox, prekeys and the public room `pub:general`,
with fixed caps) next to the v1 gateway on the same port. Add it with
`messages add sc2 NAME chat://IP:7777 [TOKEN] key=B64`, using the key chatd
logs at start. See the `chatd` job reference for its limits. `chat://` is
plain TCP, so the relay sees your IP; message contents stay end-to-end
encrypted.

Gateway conversations use the v1 protocol. They are always marked `legacy
insecure`, and also `transport secured` with `chats://` or `tls://`. The server
can read and change these messages, and sender names are not signed by the
sender.

## Quick reference

```text
contacts
contacts status
contacts list [all|discovered|trusted|blocked]
contacts show CONTACT_ID
contacts rename CONTACT_ID NAME
contacts trust CONTACT_ID [ENDPOINT_ID]
contacts block CONTACT_ID [ENDPOINT_ID]
contacts remove CONTACT_ID
contacts link TARGET_CONTACT_ID SOURCE_CONTACT_ID
messages
messages status
messages conversations
messages list CONVERSATION_ID
messages send CONVERSATION_ID TEXT [--allow-untrusted]
messages read CONVERSATION_ID
messages delete MESSAGE_ID
messages remove INSTANCE
messages clear INSTANCE|INSTANCE_ID|all
messages outbox
messages cancel MESSAGE_ID
outbox [list]
outbox cancel MESSAGE_ID
```

Conversations hold message history. Inbox shows incoming notifications from
messaging and other apps. Outbox holds pending sends.

`messages clear` also removes the selected provider's Inbox projections,
including stale projections whose retained ring slot has already wrapped. It
does not remove unrelated Inbox sources; use `inbox clear` for the entire
universal Inbox.

For packet-radio operation, see `meshcore` and `link`. MeshCore direct messages
use the same conversations and trust states as gateway messages; shared-key
groups remain visibly sender-unverified. A `radio-link` or `espnow-link` job
started with `chat=on` adds unencrypted, packet-sized Link broadcast and direct
conversations and discovers source device IDs as Contacts.
