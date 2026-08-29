+++
id = "storage"
title = "Storage and shell paths"
section = "shell"
summary = "Use SolarOS volumes, files, directories, and shell-style paths"
aliases = ["filesystem", "files"]
keywords = "python lua storage filesystem files directories mount sd flash copy rename remove mkdir path disk volume"
packages_any = []
+++
# Storage and shell paths

SolarOS presents the default storage volume as `/`. On an SD-backed target this
normally means the SD card, while `/flash` remains the internal flash volume.
On a board without SD, `/` normally maps to internal flash.
Attaching an `sdmmc` or `sdspi` expansion does not change that mapping:
internal flash remains `/`, and the removable card mounts at `/sdcard`.

Boards with an integrated SDMMC slot expose it as the fixed expansion device
`storage0`. Its pins are claimed before storage starts, but card probing and
mounting remain in the normal storage initialization phase.

Shell startup is deliberately independent of whichever volume is currently the
default. `setterm startup flash` reads `/flash/.shell/startup` on SD-capable
boards and `/.shell/startup` on boards where flash is root. `setterm startup sd`
reads `/sdcard/.shell/startup`. The setting is stored in NVS, takes effect on the
next boot, and defaults to internal flash. If the selected volume is unavailable,
SolarOS does not run a startup script from the other volume.

Use `disk` for both internal and removable persistent storage:

```text
disk status
disk lsblk
disk mount flash
disk mount sd0p2 /mnt
disk umount sd0p2
```

`disk lsblk` names internal flash as `flash`, a removable card as `sd0`, and
its partitions as `sd0p1`, `sd0p2`, and so on.

## Formatting

Formatting permanently erases the selected target and creates a FAT
filesystem. The target must be unmounted and the explicit `--force` guard is
required:

```text
disk umount flash
disk format flash --force
disk mount flash
```

For removable media, use `disk umount` without a target to unmount all card
volumes before formatting `sd0` or one of its partitions. Formatting `sd0`
creates a whole-disk FAT filesystem; formatting `sd0pN` preserves the partition
table and formats only that partition.

SolarOS initializes a completely erased internal flash partition on first use,
but it does not automatically format a non-empty partition when mounting fails.
This keeps filesystem damage from silently turning into data loss; use the
guarded format command only when the existing contents can be discarded.

Shell paths are not host operating-system paths. Scripts should use
`solaros.storage` so the same code follows SolarOS mount and path rules.

Relative path arguments passed to shell commands and foreground applications
start at the directory shown in the shell prompt. This includes file editors,
readers, script runtimes, media applications, transfers, and `files [path]`.
Use a leading slash to start at the active volume root.

## Inspect before writing

```python
import solaros

print(solaros.storage.status())
print(solaros.storage.blocks())
print(solaros.storage.usage())
```

Use `resolve(path)` when a native or library operation needs the resolved
internal path. Check free space before copying or producing a large capture.

## Volumes and directories

The storage API can create and remove directories, copy or rename files, and
mount detected volumes. Destructive calls report SolarOS errors; do not assume
that a failed operation partially succeeded.

## Quick reference

Use solaros.storage, not host os or io APIs. Functions include status,
is_mounted, mount, unmount, mount_point, usage, resolve, rescan, blocks,
block_count, block, usage_for_block, mkdir, rmdir, remove, rename, copy,
mount_volume, and unmount_volume. SolarOS shell paths use slash for the active
default storage volume.
