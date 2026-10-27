+++
id = "lua.sc2"
title = "Lua SC2 end-to-end chat security API"
section = "api"
summary = "SC2 end-to-end encryption: sc2"
keywords = "lua solaros api sc2 e2e encryption chat ratchet x3dh safety number room"
packages_any = ["service_sc2"]
agent_reference_sections = true
+++
# Lua SC2 end-to-end chat security API

[API overview](lua.md) · [Python SC2](python.sc2.md)

`solaros.sc2` in Lua has the same functions, arguments and result fields as
the [Python module](python.sc2.md): `identity`, `status`, `keys_publish`,
`add_bundle`, `peer`, `safety_number`, `forget`, `dm_encrypt`, `dm_decrypt`,
`room_create`, `room_share`, `room_rotate`, `room_encrypt`, `room_decrypt`,
`room_info`, `room_forget`, `post_sign`, `post_verify` and `selftest`.
Results are tables instead of dicts, `nil` replaces `None`, and errors are
raised as `sc2: <reason>`.

Keys never enter the Lua state: scripts see frames, plaintext and public
metadata only.

```lua
local sc2 = solaros.sc2
local me = sc2.identity().device_id
local frame = sc2.dm_encrypt(peer, '{"t":"text","body":"hi"}')
local msg = sc2.dm_decrypt(line)
if msg.kind == "message" then print(msg.from, msg.body) end
```

## Quick reference

Use `solaros.sc2` for SC2 end-to-end encrypted DMs, private rooms and signed
public posts; the calls match `man python.sc2`. Keys stay in C. Errors raise
`sc2: <reason>`. See `man lua` for runtime conventions.
