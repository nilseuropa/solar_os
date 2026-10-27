+++
id = "python.messaging"
title = "Python contacts and messages API"
section = "api"
summary = "Contacts and messages: contacts, messages"
keywords = "python solaros api messaging contacts messages"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python contacts and messages API

[API overview](python.md) · [Lua contacts and messages](lua.messaging.md)

## `solaros.contacts` and `solaros.messages`

Provider-neutral messaging builds expose:

- `solaros.contacts.list()`: bounded contact summaries.
- `solaros.contacts.get(contact_id)`: one contact with endpoint IDs, or `None`.
- `solaros.messages.conversations()`: bounded conversation summaries.
- `solaros.messages.list(conversation_id[, offset[, limit]])`: saved history
  in chronological order. The default page contains the newest 64 messages.
  Offset skips messages from the newest end; limit is 1 through 64. Increase
  offset by the returned page length to read older pages.
- `solaros.messages.send(conversation_id, body, allow_untrusted=False)`: queue
  a message and return its stable hexadecimal ID.
- `solaros.messages.mark_read(conversation_id)`: mark linked message state read.
- `solaros.messages.cancel(message_id)`: cancel a queued message using the
  hexadecimal string returned by `send()` or `list()`.

Scripts cannot read credentials or endpoint secret material. Blocked direct
endpoints are rejected, and discovered endpoints require
`allow_untrusted=True` for that one send.

`solaros.messages.instances()` lists instance IDs, names, provider classes,
capabilities, and connection status without authentication secrets. Use
`messages.join(instance_id, channel)`, `messages.query(instance_id, nickname)`,
`messages.leave(conversation_id)`, and `messages.connect(instance_id, enabled)`
on that same submodule. Join and query return a conversation ID. Connection
settings are saved; the provider job must also be running.

`solaros.messages.classes()` lists class names, capabilities, and `creatable`.
`messages.add(class_name, name, settings)` returns the new instance ID;
`messages.configure(instance_id, settings)` changes its settings. Pass settings
as a list or tuple of at most eight strings, using the same order as
`messages add` and `messages configure` in the shell. For example:

```python
community = solaros.messages.add("irc", "community", ["irc.example.org", "6697", "nils"])
solaros.messages.configure(community, ["sasl", "account", "password"])
solaros.messages.connect(community, True)
```

The corresponding provider job must run to connect. Authentication settings are
written to the instance profile and are never returned by enumeration.

`solaros.messages.remove_instance(instance_id)` closes and removes a configurable
instance, credentials, pending sends, local history, and Inbox notifications. Default
instances require backend removal support. Removal requires writable mounted storage. Removed instances
are omitted from `instances()`. Creating the same name again allocates a different ID.

Conversation summaries include `instance_id`, `provider_class`, `join_state`
(0 none, 1 left, 2 joining, 3 joined, 4 failed), and `join_error`.
Conversation summaries include the raw `title`, a display `label` that identifies
MeshCore group transports, and `history_only`. History-only conversations are
readable but reject sends.

## Quick reference

Use `solaros.contacts`, `solaros.messages` for contacts and messages.
See `man python` for runtime conventions and service availability.
