+++
id = "network"
title = "Wi-Fi, WireGuard, MQTT, and network APIs"
section = "network"
summary = "Connect, inspect, and communicate over installed network services"
aliases = ["wifi", "wireguard", "vpn", "mqtt", "net"]
keywords = "python lua wifi wireless wireguard vpn tunnel kill switch station access point ap nat scan connect mqtt network ping"
packages_any = ["service_wifi", "service_wireguard", "service_mqtt", "service_net"]
+++
# Wi-Fi, WireGuard, MQTT, and network APIs

Network modules are package-gated. Inspect their status before assuming Wi-Fi,
WireGuard, MQTT, or diagnostic networking exists in the current firmware.

## Wi-Fi

Wi-Fi is enabled by default. `wifi disable` prevents the Wi-Fi driver and its
station/AP network interfaces from initializing on the next boot. `wifi enable`
enables them again for the next boot. Both commands leave the current boot and
saved network profiles unchanged. `wifi on` and `wifi off` remain live radio
controls for the current boot.

The `espnow-link` job uses a connectionless Wi-Fi lease. In automatic mode it
follows the active station or AP channel and otherwise uses channel 6. While
the lease is active, scanning is rejected. A fixed ESP-NOW channel also rejects
a new station connection or an AP configured for another channel. `wifi off`
turns off station/AP networking but reports that the radio remains active until
the ESP-NOW job stops. Optional `phy=lr500` and `phy=lr250` modes temporarily
add Espressif Long Range support to the station interface; stopping the job
restores the Wi-Fi protocol selection that was active before it started.

From the shell, `wifi` opens the display TUI and `wifi status` works on every
shell. A script can scan before connecting:

```python
import solaros

for network in solaros.wifi.scan():
    print(network)
print(solaros.wifi.status())
```

Connecting or stopping Wi-Fi can interrupt an active agent, SSH, chat, or HTTP
session. Confirm disruptive changes locally.

## WireGuard

WireGuard is a native ESP-IDF/lwIP client service. Python and Lua do not own the
tunnel or the socket stack. Import a conventional configuration file and start
the tunnel:

```text
wireguard import /sd/vpn/solar.conf
wireguard up
wireguard status
wireguard down
wireguard forget
```

The supported client subset has one `[Interface]` section and one `[Peer]`
section. It accepts `PrivateKey`, one IPv4 `Address`, optional `ListenPort`,
`MTU`, and one numeric IPv4 `DNS`; the peer accepts `PublicKey`, optional
`PresharedKey`, up to eight IPv4 `AllowedIPs` prefixes, `Endpoint`, and optional
`PersistentKeepalive`. IPv6, multiple addresses or peers, hostnames in `DNS`,
and keys such as `PostUp` are rejected. The endpoint can be an IPv4 address or a
DNS hostname.

`wireguard import` validates key encoding without printing secret values. It
saves the private key and optional preshared key in NVS, then wipes temporary
decoded buffers. The source configuration file remains where it was imported
from. `wireguard forget` logically removes the saved NVS profile after the
tunnel is down. SolarOS does not currently enable flash or NVS encryption, so
physical flash access can recover deleted NVS secrets and any retained source
file.

The allowed-prefix table controls IPv4 destination routing. A `0.0.0.0/0`
prefix makes the WireGuard interface the default route. The encrypted outer UDP
flow stays bound to the Wi-Fi station interface to avoid routing it back into
the tunnel. A full tunnel uses fail-closed behavior by default. While its
hostname is being resolved, only endpoint-resolution DNS and DHCP traffic may
use Wi-Fi directly. After resolution, only WireGuard endpoint UDP and DHCP
remain permitted. This also blocks direct local-LAN and IPv6 traffic. Select
`wireguard up fail-open` to restore direct Wi-Fi routing if the peer is down.
For split tunnels the default is fail-open; `fail-closed` prevents matching
prefixes from falling through but does not block unrelated direct Wi-Fi
traffic.

The service stops its lwIP interface before light sleep and recreates it after
Wi-Fi resumes. It also tears down on a lost station address and retries after a
new address arrives. Handshake timestamps prefer synchronized wall time. A
persisted forward-only reservation supplies replay-safe timestamps when wall
time is not synchronized.

## MQTT

Connect to a broker, subscribe, then read messages with bounded timeouts. MQTT
settings are stored by the service; do not embed credentials in a public
script.

## Quick reference

solaros.wifi provides status, status_text, start, stop, connect, connect_saved,
disconnect, forget, forget_ssid, forget_all, known, scan, ap_start, ap_stop,
and nat. WireGuard intentionally has no Python or Lua binding. solaros.mqtt
provides status, connect, disconnect, publish, subscribe,
and read. solaros.net.ping(host, optional count, timeout_ms, interval_ms,
data_size) returns statistics. These modules are package-gated.
