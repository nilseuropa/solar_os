+++
id = "help"
title = "Browsing and refreshing documentation"
section = "concept"
summary = "Browse the manual and refresh its signed Markdown pages"
aliases = ["documentation"]
keywords = "help documentation manual browser tui update refresh signed catalog sd fallback version"
packages_any = []
+++
# Browse and refresh documentation

SolarOS always carries a manual in firmware, so `man` and the agent reference
tool work without a network connection.

Run `help` to open the foreground documentation browser. Topics are grouped in
a tree with all groups initially folded. Use Left and Right or Enter on a group
to fold and unfold it, then select a topic and press Enter to read it. The tree
retains its selection, scroll position, and folded groups when the topic closes.
On a graphic display the topic opens in `reader`; text shells use `less`. Both
consume the same `man:TOPIC` source, so TOML frontmatter is never shown as
document content.

## Refresh from solar-os.eu

On devices with Wi-Fi, PSRAM, and an SD card, the same manual can be refreshed
without installing new firmware.

First connect Wi-Fi, inspect the available persistent disks, and mount the
default removable volume if necessary:

```text
disk lsblk
disk mount
```

Then run:

```text
help update
```

SolarOS requests the documentation published for its exact running firmware
version. It verifies the catalog with the OTA public key, downloads the single
`manual.zip` archive authenticated by that catalog, and extracts it into a
temporary revision. Every extracted Markdown page is then checked against its
signed size and SHA-256 before activation. The command shows width-aware
download and extraction progress, including on the narrow display shell at text
size 16. An interrupted or invalid download leaves the previous manual active.

`help status` shows whether the external revision or embedded fallback is in
use. `help reset` stops using the downloaded revision; it does not remove the
immutable cached files from the SD card.

The maintenance word `status` intentionally takes precedence over the bare
manual alias. Use the exact topic ID `help command.status` to open the shell
`status` command page.

## Why versions must match

Documentation can affect scripts produced by the agent. A page for a newer
firmware might describe APIs that do not exist on the running device. SolarOS
therefore rejects catalogs whose firmware version differs, even if their
signature is valid.

## Quick reference

`help` opens the foldable topic tree; `help TOPIC` expands the corresponding
group and selects that topic initially. Graphic display shells open topics with
`reader`; CDC, UART, Telnet, SSH, and other text shells use `less`.
`help status` reports the active source, firmware version, revision, page count,
update state, and last error. `help update` downloads one catalog-authenticated
archive, verifies every extracted page, stores the exact-version manual on SD,
and activates it only after signature, size, and SHA-256 verification. Use
`help command.status` for the command page rather than the maintenance status.
`help reset` immediately returns `man`, `help`, and the agent to the embedded
manual. Refreshing requires Wi-Fi, PSRAM, and SD.
