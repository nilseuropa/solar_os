# SolarOS Jobs

This document covers the built-in background job registry. Jobs are autonomous
workers such as log followers, DAQ capture, HTTP serving, SLIP, chatd, or NTP
sync. Foreground applications are documented in [apps.md](apps.md), and shell
commands are documented in [commands.md](commands.md). Port shells are sessions,
started with `session create shell <port>` plus optional `--term` and `--size`
terminal settings. Display-target shells use `session create shell <target>`.

Job availability depends on the selected firmware flavor and board
capabilities. The running system is authoritative:

```text
jobs
job status [name]
```

`jobs` is intentionally compact so it fits on the built-in 65-column display
terminal. It shows job name, state, kind, event source, tick count, and resource
count. Use `job status <name>` for the summary, owner string, last error, and
claimed resources.

## Job Control

| Command | Description |
| --- | --- |
| `jobs` | List registered jobs in compact form. |
| `job status [name]` | Show all jobs, or one named job with details. |
| `job start <name> [args...]` | Start a job with optional arguments. |
| `job stop <name>` | Stop a running job. |

Only one instance of each job name can run at a time. Starting a running job
first stops the previous instance, then starts it again with the new arguments.

Jobs have stable owner strings in the form `job:<name>`. Jobs that claim ports,
files, streams, or network listeners publish those resources through the job
status model. This keeps port/resource conflict messages readable and avoids
job-specific inspection code in the shell.

Jobs that use byte-stream ports claim those ports while running. If a port is
already owned, SolarOS reports the owner, for example `job log owns cdc0`.
Radio listeners expose their radio as a custom job resource.

Compact list example:

```text
NAME         STATE    KIND        EVT  TICKS RES
batmon       running  background  tick    17   1
log          stopped  background  tick     0   0
```

Detailed status example:

```text
job status log
NAME         STATE    KIND        EVT  TICKS RES
log          running  background  tick     8   1
  summary: stream SolarOS logs to a port or file
  owner: job:log
  resources:
  - port   cdc0 rw
```

Useful ports:

```text
cdc0
uart0
```

List available streams with:

```text
stream
```

## Startup

Jobs can be started from the normal startup script:

```text
/.shell/startup
```

Example:

```text
wifi on
job start ntp-sync once
job start batmon 60
```

## batmon

Battery monitor. It periodically samples battery voltage, maintains a smoothed
trend, estimates power state, and can request light sleep when the configured
minimum voltage is reached.

Usage:

```text
job start batmon [interval-sec]
job stop batmon
job status batmon
```

Defaults:

| Setting | Value |
| --- | --- |
| Interval | `60` seconds |

Battery limits are configured with the `battery` shell command:

```text
battery capacity <mAh>
battery min_voltage <volts>
battery max_voltage <volts>
```

Notes:

- Discharging trend means battery power.
- Charging trend means external power.
- Voltage above `max_voltage` is a fast external-power shortcut.
- Three consecutive samples at or below `min_voltage` while on battery request
  light sleep.

Example:

```text
job start batmon 60
```

## bridge

Raw bidirectional byte bridge between two byte-stream ports.

Usage:

```text
job start bridge <port-a> <port-b>
job stop bridge
job status bridge
```

Example:

```text
job start bridge cdc0 uart0
```

Notes:

- The two ports must be different.
- Both ports are claimed by the bridge job until it stops.
- This is the clean base for USB-to-UART converter style workflows.

## chatd

Local SolarOS chat gateway server. It is useful for testing the `chat` app or
for small trusted local networks.

Usage:

```text
job start chatd [port] [token] [--history path]
job start chatd [port] [token] [path]
job stop chatd
job status chatd
```

Defaults:

| Setting | Value |
| --- | --- |
| Port | `7777` |
| Default channel | `general` |
| Maximum clients | `6` |
| Maximum channels | `32` |
| In-memory history | `64` events |

Arguments are intentionally flexible. The first numeric argument is the port.
The next non-option argument is the optional token. `--history` or `--log`
selects an optional append-only history dump file.

Examples:

```text
job start chatd
job start chatd 7777 secret
job start chatd 7777 secret --history /.shell/chatd.log
```

The local chat app can connect with:

```text
chat local
chat 127.0.0.1:7777
```

On another SolarOS device or host on the same network, use the server IP:

```text
chat 192.168.1.113:7777
```

Notes:

- If a token is configured, clients must present the same token.
- New clients receive the recent in-memory channel history.
- Channel deletion is supported by the chat protocol and client.
- The built-in server is a lightweight LAN gateway, not a hardened public chat
  service.

## daq

Data acquisition job. It captures scalar streams to timestamped CSV, or one
byte stream directly to a raw file.

The `daq` shell command is usually easier to remember:

```text
daq
daq streams
daq start <stream...> <file> [options]
daq start <file> <stream...> [options]
daq stop
daq status
```

Direct job usage:

```text
job start daq <stream...> <file> [--rate seconds|--rate-ms ms] [--append|--replace]
job start daq <file> <stream...> [--rate seconds|--rate-ms ms] [--append|--replace]
job start daq <byte-stream> <file> --raw [--rate-ms ms] [--append|--replace]
job stop daq
job status daq
```

Defaults:

| Mode | Default interval |
| --- | --- |
| Scalar CSV | `1000` ms |
| Raw byte stream | `25` ms |

Examples:

```text
daq start temperature /logs/temp.csv --rate 60
daq start /logs/env.csv temperature humidity battery --rate 60
daq start uart0 /logs/uart0.bin --raw --rate-ms 25
job start daq /logs/env.csv temperature humidity battery --rate 60
```

Notes:

- Multi-stream mode supports scalar streams only.
- Raw capture is single-stream only and writes incoming bytes directly.
- CSV rows include a timestamp column and one value column per stream.
- Available streams depend on board capabilities.

## httpd

Static HTTP file server for a folder on mounted storage.

Usage:

```text
job start httpd <folder>
job stop httpd
job status httpd
```

Example:

```text
job start httpd /www
```

Notes:

- Relative paths resolve under the default storage mount.
- The server uses the ESP-IDF default HTTP port.
- It serves files and simple directory listings.
- MIME types are provided for common text, image, audio, JSON, JavaScript, and
  CSS files.

## log

Runtime SolarOS log follower. It mirrors log entries to a byte-stream port or
appends them to a file.

Usage:

```text
job start log <port> [error|warn|info|debug]
job start log file <path> [error|warn|info|debug]
job stop log
job status log
```

Examples:

```text
job start log cdc0
job start log uart0 debug
job start log file /.shell/log info
```

Notes:

- Port targets use CRLF line endings.
- File targets use LF line endings and are flushed periodically.
- If no level is specified, the current runtime log level is used.
- The log job starts from the latest entry, so it follows new logs rather than
  dumping the whole ring.

## ntp-sync

Network time synchronization job. It updates SolarOS time and the RTC from NTP.

Usage:

```text
job start ntp-sync [once] [interval-sec] [server]
job stop ntp-sync
job status ntp-sync
```

Defaults:

| Setting | Value |
| --- | --- |
| Interval | `60` seconds |
| Server | `pool.ntp.org` |

Examples:

```text
job start ntp-sync once
job start ntp-sync 300 time.cloudflare.com
job start ntp-sync once 60 pool.ntp.org
```

Notes:

- Wi-Fi must be connected before sync can succeed.
- In `once` mode, the job retries at the interval until the first successful
  sync, then stops itself.
- Without `once`, it keeps syncing periodically.

## pocsag

POCSAG pager receiver job. It configures a registered packet radio for a
continuous POCSAG byte stream, frames successive 64-byte batches, filters pages
to one receiver identity code (RIC), decodes alphanumeric or numeric payloads,
and publishes completed messages to the universal inbox.

Usage:

```text
job start pocsag <radio> <frequency-hz> <baud> <ric> [alpha|numeric] [normal|inverted]
job stop pocsag
job status pocsag
pocsag status
pocsag send <radio> <frequency-hz> <baud> <ric> <message> [alpha|numeric] [normal|inverted] [function]
```

Example:

```text
job start pocsag radio 448425000 1200 1841525 alpha
inbox list unread

job stop pocsag
pocsag send radio 448425000 1200 1841525 "SolarOS calling" alpha inverted
```

Notes:

- The decoder validates POCSAG parity and BCH and corrects up to two erroneous
  bits per codeword.
- Messages may continue across batch boundaries; the receiver follows the sync
  words between batches until the page is complete.
- Identical repeated pages received within 30 seconds produce one inbox entry.
- The default FSK polarity is `normal`; retry with `inverted` if batches remain
  at zero while the transmitter is active.
- `pocsag status` shows batch/message counts, corrections, receive errors, and
  the RSSI of the most recent batch.
- Stopping the job restores the radio configuration and state that were active
  when it started.
- Sending supports messages spanning multiple batches and restores the radio's
  previous configuration afterward. A receiver job using the same half-duplex
  radio must be stopped first.

## slip

IPv4 SLIP gateway on a byte-stream port. This is intended for retro machines,
headless boards, and serial networking experiments.

Usage:

```text
job start slip [port] [baud] [local-ip] [peer-ip] [netmask]
job stop slip
job status slip
```

Defaults:

| Setting | Value |
| --- | --- |
| Port | `uart0` |
| Baud | `115200` |
| Local IP | `192.168.7.1` |
| Peer IP | `192.168.7.2` |
| Netmask | `255.255.255.252` |

Examples:

```text
job start slip uart0 115200
job start slip cdc0 115200
job start slip uart0 38400 192.168.7.1 192.168.7.2 255.255.255.252
```

Notes:

- The peer should use the local IP as its gateway.
- SolarOS enables NAT on the SLIP-facing interface.
- The selected port is claimed by the SLIP job until it stops.
- `cdc0` is useful for Linux host testing; `uart0` is the natural expansion
  port path.

## sump

SUMP-compatible logic analyzer server on `cdc0`. It claims the CDC port and
uses the shared logic analyzer service for acquisition. PulseView and sigrok
can connect with the OpenBench Logic Sniffer/SUMP serial driver.

Usage:

```text
job start sump [pin ...]
job start sump [pin[,pin...]]
job stop sump
job status sump
```

Examples:

```text
job start sump
job start sump 1 2 3 17
job start sump 1,2,3,17
```

If no pins are supplied, the job uses up to eight runtime-safe GPIOs from the
active board profile. Host commands select the sample rate and capture size.
The current implementation supports 10 kHz to 2 MHz requests and up to 32768
one-byte samples; the status recorded with each capture reports the measured
effective rate.

Notes:

- The job requires both CDC and runtime-safe GPIO capabilities.
- `cdc0` cannot be used by a port shell, logger, bridge, or another job while
  SUMP is active. Start SUMP from the display shell, SSH, or another port after
  stopping any shell attached to `cdc0`.
- Basic trigger commands are accepted for host compatibility, but this first
  implementation captures immediately instead of waiting for a trigger.
- Captures remain available to the `logic` app after the job stops.
