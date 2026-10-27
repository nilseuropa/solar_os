+++
id = "python.sc2"
title = "Python SC2 end-to-end chat security API"
section = "api"
summary = "SC2 end-to-end encryption: sc2"
keywords = "python solaros api sc2 e2e encryption chat ratchet x3dh safety number room"
packages_any = ["service_sc2"]
agent_reference_sections = true
+++
# Python SC2 end-to-end chat security API

[API overview](python.md) · [Lua SC2](lua.sc2.md) · [Commands](commands.md)

`solaros.sc2` exposes the device's SC2 (SolarOS Chat v2, suite SC2-1) session
layer: X3DH key agreement, the double ratchet for direct messages, epoch keys
with per-sender chains for private rooms, and signed posts for public rooms.
The module exists in builds with the `sc2` group (enabled in the `full`
flavor on boards with PSRAM).

**Keys never enter the Python heap.** Scripts pass plaintext and relay frames
in and get frames, plaintext and public metadata out. Identity keys, prekeys,
ratchet states and room epoch keys stay in the C service. Messages that
carry keys (`t:"epoch_key"`, `t:"history"`) are consumed inside C and only
reported as events; scripts cannot send them (`dm_encrypt` and
`room_encrypt` refuse `epoch_key`, `history` and `removed` inner types).

Plaintext is the *inner JSON* of the protocol, an object with a `t` field,
for example `{"t":"text","id":"…","body":"hello","ts":1760889600}`. Build it
with `json.dumps` and read it back with `json.loads`. Inner JSON is limited to
8191 bytes; it is padded to 256…8192-byte buckets before sealing.

Frames are complete relay lines (`dm.send`, `room.send`, `room.post`,
`keys.publish`) ready to write to a relay connection, and the decrypt and
verify calls take the relay's `dm.msg`, `room.msg` and `room.pub` lines.
This module does not open network connections.

## Identity and peers

- `solaros.sc2.identity()`: `{"device_id", "fingerprint", "ik", "dk",
  "spk_id", "opk_count"}`. The first call on a device generates the identity.
  Public values only.
- `solaros.sc2.status()`: `{"ready", "core", "hash", "device_id", "store",
  "psram", "peers", "sessions", "rooms", "opk_count"}`, or `"error"` when the
  service cannot start (for example, internal flash not mounted).
- `solaros.sc2.keys_publish(opk_target=16, rid=None)`: returns a
  `keys.publish` frame. Tops up one-time prekeys to `opk_target` (at most 32)
  and rotates the signed prekey after 30 days; the new private keys are
  stored before the frame is returned.
- `solaros.sc2.add_bundle(frame)`: verify a relay `keys.bundle` line (device
  ID must match the identity key; both signatures must verify) and store it.
  Returns the peer's device ID. Needed before the first DM in either
  direction.
- `solaros.sc2.peer(device_id)`: `{"session", "opk", "spk_id",
  "fingerprint"}` or `None`.
- `solaros.sc2.safety_number(device_id)`: `{"short", "words", "full"}`.
  Compare it with the contact over another channel before trusting them.
- `solaros.sc2.forget(device_id)`: delete the stored bundle and DM session.

## Direct messages

- `solaros.sc2.dm_encrypt(device_id, inner_json, rid=None)`: returns a
  `dm.send` frame. The first message to a peer runs X3DH from the stored
  bundle; the session is saved before the frame is returned, so send what you
  get or the receiver will see a gap.
- `solaros.sc2.dm_decrypt(frame)`: open a `dm.msg` line. Returns
  `{"from", "seq", "kind", "new_session", ...}`. With `kind == "message"`
  the dict has `"body"` (the inner JSON). With `"epoch_key"` or `"history"` a
  room key was installed and the dict has `"room"` and `"epoch"`; `"ignored"`
  means a key from someone who is not the room's admin was dropped. Replays,
  tampering and unknown senders raise `OSError("sc2: …")`.

## Private rooms

Private rooms use one random epoch key per epoch, distributed by the admin
over pairwise DMs, and one sending chain per member. Every room message is
also Ed25519-signed by its sender.

- `solaros.sc2.room_create(room=None)`: `{"room", "epoch"}`; this device is
  the admin. `room` is 32 hex characters (random when omitted).
- `solaros.sc2.room_share(room, device_id, rid=None)`: admin only; returns a
  `dm.send` frame that carries the current epoch key to one member.
- `solaros.sc2.room_rotate(room)`: admin only; starts a new epoch and returns
  its number. Share it with every remaining member. The previous epoch stays
  readable for late messages.
- `solaros.sc2.room_encrypt(room, inner_json, rid=None)`: returns a
  `room.send` frame.
- `solaros.sc2.room_decrypt(frame)`: `{"room", "from", "seq", "epoch", "n",
  "body"}` for a `room.msg` line. The sender's bundle must be stored.
- `solaros.sc2.room_info(room)`: `{"admin", "is_admin", "epoch", "sent",
  "senders"}` or `None`.
- `solaros.sc2.room_forget(room)`: delete the room keys.

This API does not check membership commits (`room.commit`); the `sc2-sync`
worker verifies them for the rooms it manages. A device therefore accepts epoch
keys for a room it does not know from whoever sends them first, and afterwards
only newer epochs from that admin.

## Public rooms

Public rooms (`pub:` names) are signed, not encrypted.

- `solaros.sc2.post_sign(relay_id, room, name, body, ts=0, ref=None,
  delete=False, rid=None)`: returns a `room.post` frame. `ts=0` uses the
  current time. `delete=True` retracts the post named by `ref`.
- `solaros.sc2.post_verify(relay_id, frame, max_body=1024)`:
  `{"valid", "room", "from", "id", "ref", "name", "ts", "seq", "deleted",
  "body"}` for a `room.pub` line. A post that fails verification is returned
  with `valid=False` so it can be shown as unverified or dropped.

## Selftest

`solaros.sc2.selftest()` returns `{"passed", "failed"}` after the same
checks as the `sc2 selftest` shell command. It uses throwaway in-memory
devices and never touches stored keys.

## Example

```python
import json, solaros
sc2 = solaros.sc2
me = sc2.identity()["device_id"]
publish = sc2.keys_publish()          # send to the relay
peer = sc2.add_bundle(bundle_line)    # bundle_line from keys.claim
frame = sc2.dm_encrypt(peer, json.dumps({"t": "text", "body": "hi"}))
msg = sc2.dm_decrypt(incoming_line)
if msg["kind"] == "message":
    print(msg["from"], json.loads(msg["body"])["body"])
print(sc2.safety_number(peer)["words"])
```

## Keys at rest

Keys live on internal flash in `/.sc2/` (`/flash/.sc2/` on boards with an SD
slot), never on removable cards. Each record is versioned, CRC-checked and
kept in two alternating copies, so an interrupted write falls back to the
previous copy. Flash encryption is **off by default**: anyone who holds the
device can read its flash, copy the identity and session keys, impersonate
it and read messages that arrive afterwards until contacts block it.
Deleted records are overwritten before removal, but the flash
wear-levelling layer can keep older sector copies until they are erased.

The hardware RNG is only a true RNG while Wi-Fi or Bluetooth is running.
SC2 mixes it with a seed carried across boots, and the identity is created
on first use rather than at boot; create it with Wi-Fi connected.

## Quick reference

Use `solaros.sc2` for SC2 end-to-end encrypted DMs, private rooms and signed
public posts. Keys stay in C: pass inner JSON and relay lines, get frames and
plaintext back. Call `keys_publish()` once and `add_bundle()` for each
contact, then `dm_encrypt`/`dm_decrypt`, `room_*` and `post_sign`/
`post_verify`. Errors raise `OSError("sc2: …")`. See `man python` for runtime
conventions.
