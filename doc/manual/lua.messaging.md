+++
id = "lua.messaging"
title = "Lua contacts and messages API"
section = "api"
summary = "Contacts and messages: contacts, messages"
keywords = "lua solaros api messaging contacts messages"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua contacts and messages API

[API overview](lua.md) · [Python contacts and messages](python.messaging.md)

## `solaros.contacts`

- `solaros.contacts`: `list`, `get` when provider-neutral messaging is compiled

## `solaros.messages`

- `solaros.messages`: `conversations`, `list`, `send`, `mark_read`, `cancel` when provider-neutral messaging is compiled

## Sending messages

`solaros.messages.list(conversation_id[, offset[, limit]])` reads a disk history
page in chronological order. The default page contains the newest 64 messages.
Offset skips messages from the newest end; limit is 1 through 64. Increase
offset by the returned page length to read older pages.

`solaros.messages.send(conversation_id, body[, allow_untrusted])` queues a
message and returns its stable hexadecimal ID. `list()` also represents message
IDs as hexadecimal strings, and `cancel(id)` accepts that representation.
Blocked direct endpoints are always rejected; discovered endpoints require the
optional boolean for that one send. `solaros.contacts` returns only contact
summaries and endpoint IDs, never credentials or endpoint secret material.

`solaros.messages.classes()` lists class names, capabilities, and `creatable`.
`solaros.messages.add(class_name, name, settings)` returns a new instance ID;
`messages.configure(instance_id, settings)` changes its settings. Pass settings
as a table of at most eight strings in shell argument order. For example:

```lua
local community = solaros.messages.add("irc", "community", {"irc.example.org", "6697", "nils"})
solaros.messages.configure(community, {"sasl", "account", "password"})
solaros.messages.connect(community, true)
```

The corresponding provider job must run to connect. Authentication settings are
saved in the instance profile and are never returned by enumeration.

`solaros.messages.instances()` lists instance IDs, names, provider classes,
capabilities, and connection status without authentication secrets. Use
`messages.join(instance_id, channel)`, `messages.query(instance_id, nickname)`,
`messages.leave(conversation_id)`, and `messages.connect(instance_id, enabled)`
on that same submodule. Join and query return a conversation ID. Connection
settings are saved; the provider job must also be running.

`solaros.messages.remove_instance(instance_id)` closes and removes a configurable
instance, credentials, pending sends, local history, and Inbox notifications. It protects the
default instances without backend removal support and requires writable mounted storage. Removed instances
are omitted from `instances()`. Creating the same name again allocates a different ID.

Conversation summaries include `instance_id`, `provider_class`, `join_state`
(0 none, 1 left, 2 joining, 3 joined, 4 failed), and `join_error`.
Conversation summaries include the raw `title`, a display `label` that identifies
MeshCore group transports, and `history_only`. History-only conversations are
readable but reject sends.

## Quick reference

Use `solaros.contacts`, `solaros.messages` for contacts and messages.
See `man lua` for runtime conventions and service availability.
