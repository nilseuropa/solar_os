+++
id = "identity"
title = "Device identity"
section = "service"
summary = "Set the username and hostname shown in the shell and network services"
aliases = ["hostname", "user"]
keywords = "python lua identity hostname user username nvs wifi advertised device name"
packages_any = []
+++
# Device identity

The device user and hostname are saved across reboots without an SD card. The shell prompt uses both values. Gateway Chat uses both values when it
connects, and network services use the hostname when advertising the device.
The default BLE device name also uses this hostname. A BLE peripheral app can
use its own name while active; the device hostname is restored when it closes.

## Inspect and change identity

From the shell:

```text
identity status
identity user nils
identity hostname solarterm
```

From Python:

```python
import solaros

print(solaros.identity.format())
solaros.identity.set_hostname("solarterm")
```

If Wi-Fi or BLE is already initialized, reboot before expecting every
advertisement or DHCP hostname to use a changed value.

If Gateway Chat is already connected, run `gateway connect` to reconnect with
the changed identity.

## Quick reference

solaros.identity provides user(), hostname(), set_user(name),
set_hostname(name), and format(). Values are NVS-backed. Reboot before
expecting an already initialized Wi-Fi or BLE interface to advertise a changed
hostname.
