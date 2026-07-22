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
`maintenance_jobs`, `audio`, `net`, `media`, `games`, `python`, `lua`, and
`utils`. Maintenance apps and jobs can therefore be selected independently.

Network ownership is intentionally split. `network.base`, `network.mqtt`,
`network.ssh`, `network.mail`, `network.chat`, `network.http-client`, and
`network.http-server` own their individual implementations. Image and document
decoding are separate `media.image` and `media.document` packages, so selecting
`app.curl`, for example, does not pull MQTT, SSH, mail, or image dependencies.

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
