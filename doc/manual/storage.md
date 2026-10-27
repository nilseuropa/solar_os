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

## Browse and manage files

```text
pwd
ls
mkdir /notes
edit /notes/todo.txt
cp /notes/todo.txt /notes/backup.txt
df
```

Relative paths start in the current shell directory. A leading slash starts at
the default volume root. Use `/flash` or `/sdcard` to select a particular
mounted volume. Check free space with `df` before a large copy or capture.

## Startup script

Run `setterm startup` to see the script path. The selection is saved and takes
effect at the next boot:

- `auto`: use mounted board-owned SD, otherwise flash. This is the default.
- `flash`: use `/flash/.shell/startup`, or `/.shell/startup` on flash-root boards.
- `sd`: use `/sdcard/.shell/startup`.

Explicit `flash` and `sd` selections do not fall back. If SD is mounted but
has no startup file, `auto` does not run the flash script.

## Mounts

Use `disk` for internal and removable persistent storage:

```text
disk status
disk lsblk
disk mount flash
disk mount sd0p2 /mnt
disk umount sd0p2
```

`disk lsblk` names internal flash as `flash`, a removable card as `sd0`, and
its partitions as `sd0p1`, `sd0p2`, and so on.

After replacing or reinserting a card, close files that used it and run
`disk mount` to recover storage. The command checks the cached card, clears
stale SD mounts, and initializes the inserted card again. A board-defined
SDSPI or SDMMC slot also supports `expansion attach storage0` for recovery.

## Disk images

Mount a raw `.img` with a FAT12, FAT16, or FAT32 filesystem to edit its files:

```text
disk mount /sdcard/tiny386/harddisk.img /mnt/dos
ls /mnt/dos
cp /sdcard/notes.txt /mnt/dos/notes.txt
disk umount /mnt/dos
```

SolarOS accepts whole-volume images, such as FAT floppy images, and MBR disks.
It selects the first FAT partition by default. Add a partition number from 1
to 4 to select another primary partition, for example
`disk mount /sdcard/disk.img /mnt/dos 2`. Other filesystems and compressed disk
formats are not supported. Mounting never formats an existing image.

To create a new hard-drive image with one FAT partition:

```text
disk create /sdcard/tiny386/harddisk.img 32
```

The size is in MiB, from 1 to 2047. Creation allocates the full size and refuses
to overwrite an existing file. The new disk is formatted but is not bootable;
install a guest operating system separately. Images must use 512-byte sectors
and be smaller than 2 GiB. FAT mounts share the firmware's available volume
slots with SD and flash; `disk lsblk` lists mounted images as `imgN`.

Close files before unmounting. Unmount an image before giving it to Tiny386;
close the guest before mounting it again. Suspending a guest keeps its disk
claim. SolarOS refuses a second mount or guest using the same image, and keeps
the backing storage mounted while an image is in use. Do not edit or replace
the raw image file through another program while it is mounted or running.

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

SolarOS initializes erased internal storage on first use. If a non-empty
volume fails to mount, it leaves the contents intact. Format it only when you
can discard those contents.

## Inspect before writing

```python
import solaros

print(solaros.storage.status())
print(solaros.storage.blocks())
print(solaros.storage.usage())
```

Use `resolve(path)` when a native or library operation needs the resolved
internal path. Check free space before copying or producing a large capture.

Scripts should use `solaros.storage` for SolarOS paths and file operations.
See [Python storage](python.storage.md) or [Lua storage](lua.storage.md).

## Quick reference

Use solaros.storage, not host os or io APIs. Functions include status,
is_mounted, mount, unmount, mount_point, usage, resolve, rescan, blocks,
block_count, block, usage_for_block, mkdir, rmdir, remove, rename, copy,
mount_volume, and unmount_volume. SolarOS shell paths use slash for the active
default storage volume.
