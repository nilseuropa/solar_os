# SolarOS Shell Commands

This document covers built-in shell commands. Foreground applications are
documented separately in [apps.md](apps.md). Command availability depends on the
compiled firmware flavor and board capabilities; `help` shows the commands in
the running image.

## Shell Conventions

Paths are resolved relative to the current shell directory. The default storage
volume is presented as `/`. On SD-backed boards, the primary SD card volume also
has the internal mount path `/sdcard`, and the internal flash volume is mounted
at `/flash`. On boards without SD support, internal flash is mounted as `/`.

Wildcard patterns are supported by selected filesystem commands, for example
`*.txt`.

Tab completion covers commands, subcommands, filesystem paths, job names, port
names, and stream IDs where the command exposes enough structure.

History is kept in memory and cached at `/.shell/history` when storage is
available. Optional startup and alias files:

```text
/.shell/startup
/.shell/alias
/.shell/user
/.shell/hostname
```

The display-shell app exit chord is `CTRL+ALT+DEL`. Port shells use `Ctrl+]`.

## Shell Control

| Command | Usage | Description |
| --- | --- | --- |
| `help` | `help` | List built-in shell commands. |
| `clear` | `clear` | Clear the active shell terminal. |
| `watch` | `watch [-n seconds] <command> [args...]` | Repeat another shell command until `Esc`, `q`, or the app-exit key is pressed. |
| `sh` | `sh <file>` | Run a simple SolarOS shell script from storage. |
| `reboot` | `reboot` | Restart the board. |
| `sessions` | `sessions` | List display app sessions, display shell sessions, and port shell sessions. |
| `fg` | `fg <session-id>` | Resume a display app or display shell session. |
| `close` | `close <session-id>` | Close a display app or display shell session, or stop a port shell session. |

Sessions are foreground application state plus shell instances attached to a
display target or byte-stream port. Background services such as log followers,
SLIP, DAQ, and HTTP serving are jobs and are controlled with `job`.

Scripts are intentionally simple. `sh` skips blank lines and lines whose first
non-space character is `#`, then executes each remaining line as a normal shell
command. There are no variables, pipes, redirects, or conditionals yet.

Aliases are stored in `/.shell/alias`, one per line:

```text
name command-or-app fixed-args...
```

Arguments typed after the alias are appended.

## System And Diagnostics

| Command | Usage | Description |
| --- | --- | --- |
| `version` | `version` | Print SolarOS version, board, flavor, and package information. |
| `pkg` | `pkg` | Print compiled package groups and build units. |
| `board` | `board` | Print board ID, name, and capabilities. |
| `display` | `display [list]`; `display test <target>` | List drawable display targets or draw a test pattern. |
| `status` | `status` | Print a compact system status summary. |
| `uptime` | `uptime` | Print elapsed time since boot. |
| `mem` | `mem` | Print internal RAM, PSRAM, and DMA memory status. |
| `top` | `top` | Print FreeRTOS task resource information when available. |
| `sleep` | `sleep` | Enter explicit light sleep. |
| `power` | See below | Inspect and configure power policy. |
| `setterm` | See below | Configure terminal/input preferences. Without arguments, opens the display TUI when available. |

`power` usage:

```text
power status
power profile [performance|balanced|solar|offline]
power idle [off|seconds]
power key [off|light]
power sleep
```

Profiles:

| Profile | Behavior |
| --- | --- |
| `performance` | Maximum configured CPU frequency, no automatic light sleep. |
| `balanced` | CPU frequency scaling down to 80 MHz, no automatic light sleep. This is the default. |
| `solar` | CPU capped at 80 MHz with ESP-IDF automatic light sleep. |
| `offline` | CPU capped at 80 MHz, automatic light sleep, and display-shell idle sleep after 60 seconds. |

`setterm` usage:

```text
setterm
setterm orientation [0|90|180|270]
setterm font [mono|compact]
setterm textsize [12|14|16|18|20]
setterm brightness [0..100]
setterm backlight [0..100]
setterm profile [vt100|ansi|dumb]
setterm keyboard [us|de]
setterm keyrate [off|1..60 [delay-ms]]
setterm timezone [UTC|Europe/Berlin|POSIX-TZ]
setterm otaurl [url]
```

`setterm profile` is runtime-only and applies to the current port shell. From
the display shell it prints guidance to set the profile from a port shell.

## Apps And Jobs

| Command | Usage | Description |
| --- | --- | --- |
| `apps` | `apps` | List registered foreground apps compiled into the firmware. |
| `jobs` | `jobs` | List registered jobs and their state. |
| `job` | `job status [name]` | Show one job or all jobs. |
| `job` | `job start <name> [args...]` | Start or restart a job. |
| `job` | `job stop <name>` | Stop a job. |
| `session` | `session list` | List display app sessions, display shell sessions, and port shell sessions. |
| `session` | `session create shell <port> [--term auto|vt100|ansi|dumb] [--size COLSxROWS]` | Start a shell session on a byte-stream port. |
| `session` | `session create shell <display-target>` | Attach a shell session to a ready display target such as `lcd0`. |
| `session` | `session fg <id>` or `session switch <id>` | Resume a display app or display shell session. |
| `session` | `session close <id>` | Close a display app or display shell session, or stop a port shell session. |
| `session` | `session background` | Print the current foreground/background-session note. |

Port shells default to `--term auto`. Auto mode sends a terminal Device
Attributes probe; a recognizable response enables VT100-style cursor controls
and a size probe, while no response falls back to a dumb line-oriented shell.
Use `--term vt100` or `--term ansi` to force escape-sequence output,
`--term dumb` for plain text, and `--size COLSxROWS` to set the terminal dimensions
without probing.

`jobs` prints a compact table that fits the built-in display terminal:

```text
NAME         STATE    KIND        EVT  TICKS RES
batmon       running  background  tick    17   1
log          stopped  background  tick     0   0
```

Columns:

| Column | Meaning |
| --- | --- |
| `NAME` | Job registry name. |
| `STATE` | `stopped`, `running`, or `failed`. |
| `KIND` | Job kind. Current registry jobs are background workers. |
| `EVT` | `tick` if the job receives periodic tick events, otherwise `-`. |
| `TICKS` | Number of dispatched tick events while running. |
| `RES` | Number of resources currently recorded for the job. |

Use `job status <name>` for the job summary, owner string, last error, and
resource details. Job-owned resources use owner strings such as `job:log`; port
conflicts are reported as readable messages such as `job log owns cdc0`.

Common job examples:

```text
session create shell cdc0
session create shell uart0 --term vt100 --size 100x30
session create shell lcd0
job start log cdc0
job start log file /.shell/log info
job start bridge cdc0 uart0
job start httpd /www
job start ntp-sync once
job start batmon 60
job start slip uart0 115200
job stop log
```

Only one instance of each built-in job name is active at a time. Starting the
same job again stops the previous instance and starts it with the new arguments.

## Filesystems And Storage

| Command | Usage | Description |
| --- | --- | --- |
| `sd` | `sd [status]` | Show SD/storage status. |
| `sd` | `sd lsblk` | List detected SD block devices and partitions. |
| `sd` | `sd mount [sd0pN] [mount]` | Mount the default volume or an explicit partition. |
| `sd` | `sd unmount [sd0pN|mount]` | Unmount the default volume or an explicit partition/mount point. |
| `ramfs` | `ramfs [status]` | List PSRAM-backed volatile filesystem mounts. |
| `ramfs` | `ramfs mount /path size` | Mount a volatile filesystem that reserves PSRAM, such as `ramfs mount / 1m`. |
| `ramfs` | `ramfs unmount /path` | Unmount a ramfs mount. |
| `df` | `df` | Show free space on mounted storage volumes. |
| `cd` | `cd [path]` | Change current shell directory. |
| `ls` | `ls [-a] [-h] [path|pattern]` | List files. Hidden files are shown only with `-a`; sizes are human-readable with `-h`. |
| `cat` | `cat <path|pattern>` | Print a small text file. |
| `mkdir` | `mkdir <path> [path...]` | Create directories. |
| `rm` | `rm [-f|-rf] <path|pattern> [path|pattern...]` | Remove files. `-f` allows directories; `-rf` removes directories recursively. |
| `mv` | `mv <source|pattern> <dest>` | Rename or move a file or matched set. |
| `cp` | `cp <source|pattern> <dest>` | Copy a file or matched set. |
| `zip` | `zip [-0] <archive.zip> <path|pattern> [path|pattern...]` | Create a ZIP archive. `-0` stores without compression. |
| `unzip` | `unzip [-l] <archive.zip> [dest]` | List or extract a ZIP archive. |

Examples:

```text
ls -ah /.ssh
cp *.txt /backup
rm -rf /tmp/old
sd mount sd0p2 /mnt
ramfs mount /tmp 1m
ramfs mount / 4m
ramfs unmount /tmp
zip /books/archive.zip /books/*.txt
unzip -l /books/archive.zip
```

## Streams, Logs, Ports, And Transfers

| Command | Usage | Description |
| --- | --- | --- |
| `stream` | `stream` or `stream list` | List timestamped data streams. |
| `stream` | `stream status <id>` | Show one stream. |
| `daq` | `daq help` | Print DAQ usage. |
| `daq` | `daq status` | Show DAQ job status. |
| `daq` | `daq streams` | List stream IDs. |
| `daq` | See below | Start or stop data acquisition. |
| `log` | `log status` | Show runtime log ring status. |
| `log` | `log show [count]` | Print recent SolarOS log entries. |
| `log` | `log follow [error|warn|info|debug]` | Follow logs in the current shell. |
| `log` | `log clear` | Clear the runtime log ring. |
| `log` | `log level [error|warn|info|debug]` | Show or change runtime log level. |
| `log` | `log sink cdc [on|off]` | Enable or disable CDC mirroring of SolarOS logs. |
| `port` | `port list` | List byte-stream ports. |
| `port` | `port status <name>` | Show port capabilities and owner. |
| `xfer` | See below | Send or receive files over a byte-stream port. |

DAQ usage:

```text
daq start <file.csv> <stream...> [--rate seconds|--rate-ms ms]
daq start <stream...> <file.csv> [--rate seconds|--rate-ms ms]
daq start <file.csv> <stream> --changes [--append|--replace]
daq start <file.bin> <byte-stream> --raw [--rate-ms ms]
daq stop
```

DAQ examples:

```text
daq start /logs/env.csv temperature humidity battery --rate 60
daq start /logs/key.csv gpio17 --changes
daq start /logs/uart0.bin uart0 --raw --rate-ms 25
```

`daq` CSV rows include `uptime_ms`, and include UTC `time_ms` when RTC time is
trusted. Raw mode is byte-stream only, single-stream only, and writes bytes
directly without CSV framing.

Transfer usage:

```text
xfer protocols
xfer send <port> <file> --raw [-d ms]
xfer recv <port> <file> --raw [--append|--replace] [--idle-ms ms]
xfer send <port> <file> --zmodem
xfer recv <port> <file> --zmodem [--append|--replace]
```

`raw` and `zmodem` are supported. `kermit` is reserved but not implemented.

## Networking

| Command | Usage | Description |
| --- | --- | --- |
| `wifi` | `wifi` | Open the Wi-Fi display TUI when launched from the display shell. |
| `wifi` | `wifi status` | Show station/AP/NAT state. |
| `wifi` | `wifi on` | Start Wi-Fi station mode and connect to remembered networks. |
| `wifi` | `wifi off` | Stop Wi-Fi station mode. |
| `wifi` | `wifi scan` | Scan access points. |
| `wifi` | `wifi connect [ssid [password]]` | Connect and save/update a station profile. |
| `wifi` | `wifi disconnect` | Disconnect station mode. |
| `wifi` | `wifi known` | List remembered station profiles. |
| `wifi` | `wifi forget [ssid|all]` | Remove one or all remembered station profiles. |
| `wifi ap` | `wifi ap [status]` | Show SoftAP status. |
| `wifi ap` | `wifi ap on [ssid [password [open|wpa|wpa2|wpa/wpa2]]]` | Start and save SoftAP settings. |
| `wifi ap` | `wifi ap off` | Stop SoftAP. |
| `wifi nat` | `wifi nat [status|on|off]` | Configure IPv4 NAT for APSTA. |
| `ble` | `ble [status]` | Show BLE keyboard state. |
| `ble` | `ble scan` | Scan nearby BLE devices. |
| `ble` | `ble pair` | Start keyboard pairing. |
| `ble` | `ble cancel` | Cancel pairing or pending pairing. |
| `ble` | `ble forget` | Forget the remembered keyboard. |
| `ble gatt` | See below | Generic BLE GATT client. |
| `mqtt` | See below | MQTT/MQTTS client. |
| `ping` | `ping <host> [count]` | Send ICMP echo requests. Without count, ping runs until app-exit. |
| `netscan` | `netscan <host|range> [ports]` | Scan TCP ports on one host or a capped IPv4 range. |
| `ntp` | `ntp [server]` | Sync RTC from NTP. |

BLE GATT usage:

```text
ble gatt status
ble gatt connect <aa:bb:cc:dd:ee:ff> <public|random|rpa_public|rpa_random>
ble gatt disconnect
ble gatt services
ble gatt chars <service-index>
ble gatt read <handle>
ble gatt write <handle> <hex...>
ble gatt write-nr <handle> <hex...>
```

MQTT usage:

```text
mqtt status
mqtt connect [url [username [password]]]
mqtt disconnect
mqtt publish <topic> <payload> [qos] [retain]
mqtt subscribe <topic> [qos]
```

`mqtt connect mqtt://host[:port] [username [password]]` and
`mqtt connect mqtts://host[:port] [username [password]]` save broker settings
in NVS. Later `mqtt connect` reuses the saved settings.

`netscan` accepts a host, same-subnet range, or compact range form. Examples:

```text
netscan 192.168.1.10 22,80,443
netscan 192.168.1.1-50 22
netscan wintermute 22
```

## OTA And Keys

| Command | Usage | Description |
| --- | --- | --- |
| `ota` | `ota status` | Show running and configured OTA state. |
| `ota` | `ota check` | Check signed board/flavor release metadata. |
| `ota` | `ota upgrade` | Download firmware into the inactive OTA partition and reboot into it. |
| `ota` | `ota url [url]` | Show or set the OTA base URL. |
| `ota` | `ota flavor [flavor]` | Show or set target OTA flavor. |
| `ota` | `ota boot 0|1` | Select an OTA slot and reboot. |
| `sshkey` | `sshkey [status]` | Show default SSH key status. |
| `sshkey` | `sshkey gen [-f] [2048|3072|4096]` | Generate `/.ssh/id_rsa` and `/.ssh/id_rsa.pub`. |
| `sshkey` | `sshkey pub` | Print the default public key. |
| `sshkey` | `sshkey rm` | Remove the default key pair. |

OTA resolves the artifact for the compiled board and target flavor from the
configured release index, verifies the signed index, verifies firmware SHA-256,
and writes the inactive ESP-IDF OTA partition.

## Hardware And Time

| Command | Usage | Description |
| --- | --- | --- |
| `battery` | `battery [status]` | Show voltage, estimated charge, power source, config, and monitor trend. |
| `battery` | `battery config` | Show battery capacity and voltage thresholds. |
| `battery` | `battery capacity [mAh]` | Show or set capacity estimate. |
| `battery` | `battery min_voltage [V|mV]` | Show or set low-voltage threshold. |
| `battery` | `battery max_voltage [V|mV]` | Show or set full/external-power shortcut threshold. |
| `audio` | `audio status` | Show audio service state and global speaker level. |
| `audio` | `audio tone [hz] [ms] [volume]` | Play a diagnostic tone. |
| `audio` | `audio level [volume]` | Show or set global speaker level. |
| `audio` | `audio mic [ms]` | Sample microphone level. |
| `audio` | `audio loopback [ms] [volume]` | Run microphone-to-speaker loopback. |
| `audio` | `audio off` | Stop audio output. |
| `led` | `led [status|on|off|toggle]` | Inspect or control the built-in status LED when available. |
| `expansion` | `expansion [status]` | Show expansion capabilities, connector resources, active devices, and resource claims. |
| `expansion` | `expansion scan` | List expansion resources and probe-capable drivers. |
| `expansion` | `expansion drivers` | List compiled expansion drivers. |
| `expansion` | `expansion devices` | List manually attached expansion devices. |
| `expansion` | `expansion attach <driver> <name> <resource...>` | Attach a compiled expansion driver or manual resource profile. |
| `expansion` | `expansion detach <name>` | Detach an active expansion device and release its resource claims. |
| `radio` | `radio` | Open the packet-radio TUI with live status and editable common config. |
| `radio` | `radio status|list` | List packet radios registered by expansion drivers. |
| `radio` | `radio status <name>` | Show one packet radio, its capabilities, state, and current config. |
| `radio` | `radio config <name> [field value]` | Show or update common packet-radio configuration. |
| `radio` | `radio state <name> [sleep|standby|rx|tx]` | Show or change radio operating state. |
| `radio` | `radio send <name> <text|byte...>` | Send one packet. |
| `radio` | `radio recv <name> [timeout-ms]` | Receive one packet and print metadata plus payload. |
| `uart` | `uart status` | Show UART service state. |
| `uart` | `uart baud [rate]` | Show or set UART baud rate. |
| `uart` | `uart mode [raw|line]` | Show or set UART service mode. |
| `uart` | `uart write <text>` | Write text to `uart0` for diagnostics. |
| `uart` | `uart read [ms]` | Read UART bytes for diagnostics. |
| `gpio` | `gpio status` or `gpio list` | List runtime-accessible GPIOs. |
| `gpio` | `gpio mode <pin> <in|out> [none|up|down]` | Configure a runtime GPIO. |
| `gpio` | `gpio read <pin>` | Read a runtime GPIO. |
| `gpio` | `gpio write <pin> <0|1>` | Write a runtime GPIO configured as output. |
| `adc` | `adc status` | Show ADC service status. |
| `adc` | `adc read <pin>` | Read an ADC-capable runtime pin. |
| `pwm` | `pwm status` | Show PWM state. |
| `pwm` | `pwm set <pin> <freq-hz> <duty-percent>` | Start LEDC PWM on a runtime pin. |
| `pwm` | `pwm off <pin>` | Stop PWM on a pin. |
| `i2c` | `i2c status` | Show I2C bus state. |
| `i2c` | `i2c speed [hz]` | Show or set bus speed. |
| `i2c` | `i2c scan` | Scan the I2C bus. |
| `i2c` | `i2c probe <addr>` | Probe one address. |
| `i2c` | `i2c read <addr> <reg> [len]` | Read bytes from a register. |
| `i2c` | `i2c write <addr> <reg> <byte...>` | Write bytes to a register. |
| `spi` | `spi [status]` | Show SPI bus pins and chip-select slots. |
| `spi` | `spi xfer <cs> <mode> <hz> <byte...>` | Full-duplex SPI transfer. |
| `spi` | `spi read <cs> <mode> <hz> <len> [fill]` | Read bytes over SPI. |
| `spi` | `spi write <cs> <mode> <hz> <byte...>` | Write bytes over SPI. |
| `date` | `date [YYYY-MM-DD]` | Show or set local RTC date. |
| `time` | `time [HH:MM[:SS]]` | Show or set local RTC time. |
| `temperature` | `temperature` | Read the board temperature sensor when available. |
| `humidity` | `humidity` | Read the board humidity sensor when available. |

Runtime GPIO access is board-filtered. On the Waveshare ESP32-S3-RLCD-4.2,
runtime user GPIO access is intentionally limited to GPIO1, GPIO2, GPIO3, and
GPIO17. On the ESP32-S3-DevKitC-1-N16R8, runtime GPIO access is available on
GPIO1, GPIO2, GPIO4, GPIO5, GPIO6, GPIO7, GPIO10, GPIO14, GPIO15, GPIO16,
GPIO17, GPIO18, GPIO21, GPIO39, GPIO40, GPIO41, GPIO42, and GPIO47. The
DevKitC I2C bus uses GPIO8/GPIO9, SPI uses GPIO12/GPIO13/GPIO11 with
GPIO10/GPIO5/GPIO6/GPIO7 chip-select slots, GPIO19/GPIO20 are native USB,
GPIO35-GPIO37 are reserved by N16R8 Octal PSRAM, GPIO43/GPIO44 are `uart0`,
and GPIO0/GPIO3/GPIO45/GPIO46 are boot strapping pins. On ODROID-GO, runtime
GPIO access is limited to GPIO4 and GPIO15; both are external IO pins and SPI
chip-select slots. GPIO2 is the status LED, GPIO14 is the LCD backlight,
GPIO25/GPIO26 are speaker amplifier/DAC pins, GPIO18/GPIO19/GPIO23 are VSPI,
GPIO5/GPIO21 are TFT control pins, GPIO22 is SD card chip select, GPIO34/GPIO35
are ADC D-pad axes, GPIO36 is battery ADC, GPIO39 is the board key input, and
GPIO32/GPIO33/GPIO13/GPIO27/GPIO0 are built-in buttons.

Physical displays are listed by `display list`. A built-in board panel registers
as a board display target such as `display0`; an expansion display driver stays
in `expansion drivers` as attachable hardware and registers a display target
after it is attached. The built-in board panel is not an expansion driver.
`display list` includes the current owner when a target is claimed. `display
test <target>` claims the target while it draws a visible frame/test pattern,
then releases it.

Manual expansion profiles claim resources without initializing external
hardware:

```text
expansion attach manual radio0 spi0 cs=gpio10 irq=gpio4 reset=gpio5
expansion attach manual sensor0 i2c0 addr=0x40
expansion detach radio0
```

RFM69 433 MHz packet radios can be attached as active expansion devices. Only
`spi` and `cs` are required; `irq` and `reset` are optional:

```text
expansion attach rfm69 radio0 spi=spi0 cs=gpio10
expansion attach rfm69 radio0 spi=spi0 cs=gpio10 irq=gpio4 reset=gpio5
expansion detach radio0
```

PCD8544 84x48 SPI LCD modules can be attached as auxiliary display targets with
the `pcd8544` driver. The driver
requires an expansion SPI bus, a CS/CE pin, a DC pin, and a reset/RST pin. On
the ESP32-S3 DevKitC-1 target:

```text
display pins: VCC->3V3 GND->GND CLK/SCLK->GPIO12 DIN/MOSI->GPIO11
display pins: CE/CS->GPIO10 DC->GPIO4 RST->GPIO5
expansion attach pcd8544 lcd0 spi=spi0 cs=gpio10 dc=gpio4 reset=gpio5
display list
display test lcd0
```

`ce=gpio10` is accepted as an alias for `cs=gpio10`, and `rst=gpio5` is
accepted as an alias for `reset=gpio5`. If the module has an LED/backlight pin,
wire it according to the module board; use suitable current limiting when tying
it to 3V3. Boards without expansion SPI, such as the Waveshare RLCD target,
will not compile the active `pcd8544` expansion driver.

Packet radio devices are datagram endpoints registered by expansion drivers, not
byte-stream ports. The common radio layer preserves packet metadata such as RSSI
and optional source/destination IDs. Radio frequency values are Hz by default
and also accept `k`, `kHz`, `M`, and `MHz` suffixes:

```text
radio status radio0
radio config radio0 frequency 433MHz
radio config radio0 modulation gfsk
radio send radio0 hello
radio recv radio0 5000
```

## Quick Examples

```text
help
version
pkg
board
wifi on
ping wintermute
sshkey gen 2048
ota check
ota upgrade
watch -n 1 battery
daq start /logs/env.csv temperature humidity battery --rate 60
session create shell cdc0 --term auto
session create shell lcd0
xfer send uart0 /logs/payload.bin --zmodem
```
