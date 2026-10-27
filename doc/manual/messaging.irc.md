+++
id = "messaging.irc"
title = "Native IRC"
section = "service"
summary = "Connect IRC servers to unified Chat"
keywords = "irc tls sasl chat server channel private message irc-sync"
packages_any = ["service_irc", "job_irc_sync"]
+++
# Native IRC

IRC instances connect servers directly to the unified Chat application. Each
instance has its own server, nickname, optional authentication, channel list,
connection state, and outbound queue routing. Several instances can run at once.
Identical channels and nicknames on different instances remain separate.

## Quick reference

```text
irc add community irc.example.org 6697 pocket
irc connect community
job start irc-sync
irc join community #general
irc query community anothernick
irc status
messages conversations
chat
chat community
chat irc
```

Replace the example server and nicknames with your settings. `irc add` saves a
disabled instance. `irc connect` enables that instance; `irc-sync` owns the live
connections. Enable several instances before or after starting the job.

```text
irc set community irc.example.org 6697 pocket
irc sasl community account password
irc sasl community off
irc pass community server-password
irc pass community off
irc ca community /sdcard/certs/network-ca.pem
irc ca community default
irc disconnect community
irc remove community
irc leave CONVERSATION_ID
job status irc-sync
job stop irc-sync
```

The common interface also supports `messages add irc community HOST PORT NICK`
and `messages configure community HOST PORT NICK`. For authentication, use
`messages configure community sasl ACCOUNT PASSWORD` or `sasl off`; server
passwords use `pass PASSWORD|off` and CA files use `ca FILE|default`. Chat accepts
the same arguments after `/add irc` and `/configure`. Setup commands are excluded
from input history. Nicknames must use IRC nickname characters; `nils-sol` is
valid, while `nils@sol` is rejected before configuration is saved.

Shell and Chat setup commands use the same quoting rules. Single quotes
preserve spaces and backslashes literally; double quotes allow backslash
escapes. Unclosed quotes are rejected without changing saved settings.

TLS always verifies the server certificate and hostname. The default trust
source is the firmware certificate bundle. `irc ca` selects a PEM CA file on
mounted storage, limited to 8191 bytes. Keep the system clock correct for
certificate validation. SASL uses PLAIN inside verified TLS and is required
when configured: authentication failure does not fall back to anonymous login.
The server password is separate from the SASL account password.

## Chat controls

Chat groups conversations by instance. Membership controls use the same API
for every provider class that supports them:

```text
/join community #general
/query community anothernick
/leave
/connect community
/disconnect community
/remove community
/server another-network irc.example.net 6697 pocket
/sasl another-network account password
/sasl another-network off
/status
```

`/server` creates or updates an IRC instance on mounted storage. `/connect` and
`/disconnect` save its desired connection state. The background job continues
when Chat closes. Entering a room waits for its join confirmation and reports
join failures. Private conversations do not require adding a contact first.
An IRC nickname is display and routing data, not proof of a person's identity.
TLS secures the connection to the server; it does not provide end-to-end
message encryption.

## Persistence and connection behavior

Profiles are stored in `.chat/profiles/NNN.bin` on the selected mounted
storage. They include the stable instance ID, server, nickname, authentication
settings, CA path, enabled state, and up to eight desired channels. Saving uses
a checked staged file and replacement with recovery of the previous file.
Saving requires writable mounted storage. IRC adds no NVS settings. Saved
authentication data is readable to someone with access to the volume; status
output does not display passwords.

`irc remove INSTANCE`, `messages remove INSTANCE`, and Chat's `/remove INSTANCE`
close the connection and remove its configuration, credentials, pending sends,
local messages, conversations, and associated Inbox notifications. The other
IRC instances keep running. A small credential-free record preserves the old
label and reserves its ID. A new instance with the same name gets a different
ID. Interrupted history deletion completes on the next boot.

`irc-sync` maintains independent connections, reconnects with exponential
backoff up to 60 seconds, and rejoins desired channels. Channel joins, parts,
and chat sends share a limit of one command per second per instance. Protocol
registration and keepalive replies take priority. Incoming lines and control
queues are bounded, and each instance receives a limited processing budget.

IRC receives live messages only. No history or echo-message capability is
requested, and no bouncer is required. Messages sent while disconnected can be
missed. Local retained history uses the shared Messaging store. Outbound
`sent` means a complete IRC command was written to the connection; it does not
confirm delivery to another user. An interrupted send becomes `failed` and
is not automatically replayed. Queued messages that have not begun sending
remain in the volatile shared outbox until the instance can send them or the
user cancels them. Queues do not survive a device reboot.

To start the worker after reboot, add `job start irc-sync` to the startup
script reported by `setterm startup`.
