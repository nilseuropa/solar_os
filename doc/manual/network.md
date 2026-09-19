+++
id = "network"
title = "Network interfaces, routing, Wi-Fi, WireGuard, and APIs"
section = "network"
summary = "Connect, inspect, and communicate over installed network services"
aliases = ["routing", "router", "wifi", "repeater", "wireguard", "vpn", "mqtt", "net"]
keywords = "python lua wifi wireless repeater wireguard vpn tunnel kill switch station access point ap nat scan connect mqtt network ping"
packages_any = ["service_network", "service_wifi", "service_wireguard", "service_mqtt", "service_net"]
+++
# Network interfaces, routing, Wi-Fi, WireGuard, and APIs

Network modules are package-gated. Inspect their status before assuming Wi-Fi,
cellular PPP, WireGuard, MQTT, or diagnostic networking exists in the current
firmware.

## Network model

SolarOS separates three networking concepts:

- An **interface** is a connection such as `wifi-sta`, `modem0`, `wifi-ap`, or
  the WireGuard tunnel.
- A **route** decides which interface carries traffic to a destination. One
  route is the default used when no more-specific route matches.
- The **router** forwards traffic for other devices from the local downstream
  interface through the same route table SolarOS uses for its own traffic.

Use `network status` for the combined view, `network interfaces` for interface
state and addresses, and `network routes` to see the automatic base path and
WireGuard routes. Transport commands configure their own interfaces: `wifi`
manages the Wi-Fi radio and SoftAP, `modem` manages cellular PPP, and
`wireguard` manages the VPN tunnel.

`network router on` starts the saved Wi-Fi SoftAP and enables IPv4 forwarding
with NAT. Packets from AP clients follow the route table; this can send ordinary
traffic through Wi-Fi station mode or cellular PPP, and matching traffic
through WireGuard. Router mode remains ready while no default route exists and
activates when a route becomes available. `network router off` disables NAT and
stops the downstream AP.

Configure the AP name and password first when the default open `SolarOS-sol`
network is not appropriate:

```text
wifi ap on FieldTerminal downstream-password wpa2
wifi ap off
modem connect modem0
network router on
network status
```

Carrier filtering and SIM-specific ACLs remain properties of the selected
network path; router mode does not add destination restrictions of its own.

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
shell. In the TUI, `scan` opens a selectable network list; select an SSID and
enter its password to connect. `saved stations` lists remembered station
profiles and can forget them. `saved access points` adds, edits, or removes the
stored SoftAP configuration, including its password. `repeater` starts or stops
repeating the current or preferred saved station and shows whether forwarding
is waiting or active. Routing the AP through another interface is configured
with `network router`, not the Wi-Fi controls. A script can scan before
connecting:

```python
import solaros

for network in solaros.wifi.scan():
    print(network)
print(solaros.wifi.status())
```

Connecting or stopping Wi-Fi can interrupt an active agent, SSH, chat, or HTTP
session. Confirm disruptive changes locally.

`wifi repeater on` enables IPv4 layer-2 forwarding between a station and
SoftAP. It
uses the current upstream station or connects the preferred remembered station.
The downstream SoftAP automatically uses the same SSID and saved password as
that upstream profile, so repeater mode needs only an on/off control. It does
not read or overwrite the independent `wifi ap` configuration. For example:

```text
wifi connect HomeNetwork upstream-password
wifi repeater on
wifi repeater
wifi repeater off
```

The upstream DHCP server assigns downstream clients addresses on the upstream
subnet; SolarOS does not run AP DHCP or NAT in this mode. Because ordinary
three-address Wi-Fi cannot carry downstream client MAC addresses through a
station association, SolarOS translates link-layer addresses, learns each
client's IPv4-to-MAC mapping, and proxies ARP upstream. This provides same-subnet
IPv4 connectivity, but is not a fully transparent WDS bridge. IPv6 and other
non-IPv4 Ethernet protocols are not repeated.

The repeated SSID matches the upstream SSID; roaming decisions are made by each
client. The ESP32 station and SoftAP share one 2.4 GHz radio and the
upstream channel, so repeated traffic consumes airtime in both directions and
throughput is lower than a dedicated dual-radio extender. `wifi repeater off`
leaves the station connection running. Repeater and NAT modes are mutually
exclusive; the lower-level `wifi ap` and `wifi nat` commands remain available
for AP setup and diagnostics. While repeater mode is active, SolarOS
automatically retries a lost upstream connection with bounded backoff.

Forwarded client traffic bypasses SolarOS IP services, including a SolarOS
WireGuard tunnel. Configure VPN service on the clients or upstream router when
repeated clients must use it.

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
nat, repeater_start, and repeater_stop. solaros.net provides router_start,
router_stop, ping, and socket APIs. WireGuard intentionally has no Python or Lua binding. solaros.mqtt
provides status, connect, disconnect, publish, subscribe,
and read. solaros.net.ping(host, optional count, timeout_ms, interval_ms,
data_size) returns statistics. These modules are package-gated.
