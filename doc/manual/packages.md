+++
id = "packages"
title = "Firmware packages and flavors"
section = "build"
summary = "Understand package ownership, groups, flavors, and custom builds"
aliases = ["flavors"]
keywords = "packages flavors groups capabilities build custom firmware pkg"
packages_any = []
+++
# Firmware Packages and Flavors

SolarOS package selection is declared in `packages/solar_os_packages.toml`.
Flavor files select groups or individual packages; the generator resolves
package dependencies and then removes packages unsupported by the target board.

## Ownership Rules

- `bootstrap` is immutable and contains only the runtime and shell needed to
  start SolarOS. A flavor cannot disable its members.
- Groups are selection shortcuts only. They cannot own source files or ESP-IDF
  component requirements.
- Every source file and component requirement belongs to a package.
- A package lists other packages it needs with `depends`. Enabling an app or job
  automatically enables its transitive dependencies. Explicitly disabling a
  required package is an error.
- Board capability pruning is applied to the resolved graph. If a dependency is
  unavailable, its dependants are removed as well.

The standard selectors are `system`, `expansions`, `maintenance_apps`,
`maintenance_jobs`, `audio`, `net`, `agent`, `media`, `games`, `python`, `lua`,
and `utils`. Maintenance apps and jobs can therefore be selected independently.

Network ownership is intentionally split. `network.base`, `network.mqtt`,
`network.ssh`, `network.mail`, `network.chat`, `network.http-client`, and
`network.http-server` own their individual implementations. Image and document
decoding are separate `media.image` and `media.document` packages, so selecting
`app.curl`, for example, does not pull MQTT, SSH, mail, or image dependencies.

`network.http-client` owns the shared TLS-enabled HTTP transport used by `curl`
and `web`. It exposes request headers and bodies, redirects, streaming response
events, cross-task cancellation, per-I/O timeouts, and an end-to-end deadline.
Callers continue to own their worker task and response consumer; see
[HTTP Client Service](../http_client.md) for the native API and lifecycle.

The `agent` group selects `app.agent` and its `service.agent` dependency.
`service.agent` owns provider-neutral events, NVS-backed provider
configuration, bounded tool-loop policy, a declarative typed-tool registry,
and the OpenAI Responses/Chat-Completions adapter. The registry owns provider
schemas, output schemas, risk and availability metadata, and shared execution
for the system, storage, jobs, and policy-gated script tools. Its NVS-backed
`off`, `readonly`, `confirm`, and `all` policy filters advertised schemas and
is enforced again at execution time.
It depends on the shared HTTP and JSON services and is pruned from targets
without both Wi-Fi and PSRAM. Python and Lua are not dependencies. See
[Native Agent Service](agent.md).

`app.python` and `app.lua` each depend on `service.script_runner`. That service
defines the common source/file request, bounded output, cancellation, deadline,
and completion-status contract. Each interpreter owns its language adapter and
single-owner guard. `app.agent` supplies installed adapters to both the manual
`agent script` command and the typed agent registry without making either
interpreter a dependency of the agent package.

Chat is split further: `network.chat` owns the transport-neutral message store
and outbox, `chat.transport.gateway` owns the gateway wire protocol, and
`job.chat-sync` owns connection lifetime, retries, cursors, delivery, and inbox
notifications. The bounded store persists full messages on SD and uses the
Inbox's compact persistent records as its internal-flash fallback. Stable
producer IDs suppress transport replays before they reach either UI. `app.chat`
is only a foreground view over that shared state; `job.chatd` remains the
independent local gateway server.

Inbox storage and presentation are also separate. Producers such as mail and
POCSAG depend only on `service.inbox`; it owns the bounded persistent ring under
`/.inbox/`, durable read state, and producer-key replay suppression.
`app.inbox` adds the foreground browser and its shell command.

## Custom Flavor Example

This flavor adds only `curl` and the dependency closure needed by that app to
the immutable bootstrap:

```toml
[flavor]
name = "curl-only"
description = "Bootstrap plus the HTTP client app."

[packages]
app_curl = true
```

Use `pkg` on the device to inspect the resolved package list.

## Quick reference

Packages are the actual build units, groups are convenience bundles, and a
flavor selects packages for a board. Board capabilities remove packages that
cannot run. Use `pkg` on-device to inspect the resolved firmware and edit a
flavor TOML file when producing a custom build.
