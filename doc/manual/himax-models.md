+++
id = "himax-models"
title = "Himax model management"
section = "hardware"
summary = "Program storage bundles into HX6538 model slots and select a model"
keywords = "himax hx6538 model upload select slot vela sscma zoo"
packages_any = ["job_model_upload"]
+++
# Himax model management

## Quick reference

- Download a schema-two Himax bundle with Zoo before programming.
- `model upload himax camera0 /path/to/bundle.json 4 --replace` starts programming
  an explicit inactive slot; `job status model-upload` reports progress and errors.
- `job stop model-upload` cancels and attempts to restore the previous selection.
- `model select himax camera0 4` selects a programmed slot; select slot 1 to return
  to the recovery model. Slot 1 and the active slot cannot be uploaded over.
- `model slots himax camera0` lists live slot selection and recorded model identities.
- `model list` shows ESP-DL residents and the attached camera processor's slot overview.
- `pipeline start himax camera0 1` runs one inference with the current selection.

The optional `processor-himax` module programs and selects HX6538 models. Watcher
includes the same provider in its default image. The expansion driver owns the
SPI device, reset line and camera lease; bundle checks, slot policy, programming
and inference validation belong to the module. Model operations share exclusive
ownership with Photo, sampling, stream publishers and inference pipelines.

## Download, program, select

First use Zoo to download a Himax bundle to storage. Installing files does not
change the accelerator. Inspect the current firmware, slots and selected model:

```text
pipeline info himax camera0 version
pipeline info himax camera0 models
pipeline info himax camera0 model
model slots himax camera0
```

Program an explicit inactive slot with a schema-two Himax bundle:

```text
model upload himax camera0 /sdcard/dl/zoo/models/himax-person-detect-192/1.0.0/bundle.json 4 --replace
job status model-upload
```

The command starts a background job and returns to the shell. `job status
model-upload` reports stage, transferred bytes, completion and error. Use
`job stop model-upload` to cancel; cleanup restores camera operation before the
worker releases the provider. New uploads are refused until the prior worker finishes.
Only one model upload runs at a time. Closing the launching shell does not cancel it.

After a successful upload, select the slot explicitly and run inference:

```text
model select himax camera0 4
pipeline start himax camera0 1
pipeline result 1
pipeline destroy 1
model select himax camera0 1
```

`model select` verifies the firmware's selected-slot reply. Selection does not
program a model. Slot numbers are device slots, not ESP-DL resident handles.
In native, Python and Lua pipeline configurations, model zero uses the current
selection; a nonzero model selects that HX6538 slot while acquiring the camera.
The selection remains after the pipeline ends. The CLI uses the current selection.

Firmware with pipelines but without the `model` command can start the same job:

```text
job start model-upload himax camera0 /path/to/bundle.json 4 --replace
```

## Programming rules

Programming requires a reset line and SPI transfers of at least 262 bytes.
The supported SSCMA v0 firmware is dated `2024.06.03` or newer and must report
the four-slot layout at 4, 6, 8 and 10 MiB, with 2 MiB per slot. The provider
checks the actual layout before writing. A different layout is unsupported.
Reported type/size zero is not interpreted as an empty slot.

Slot 1 is reserved as a recovery model. Uploads can replace slots 2 through 4
and refuse the currently active slot. `--replace` explicitly authorizes replacing
the chosen slot, including when its existing contents are unknown. Uploading
never writes firmware, boot metadata, or flash addresses supplied by a bundle.
Keep a working model selected before uploading another one.

Before entering programming mode, the provider checks the manifest, all model
and asset hashes, labels, firmware bounds, provider ABI and capabilities, Vela
marker, and the public RGB input/output interface. Current model programming
supports quantized classifiers and Swift-YOLO detection heads. Other output
layouts, including points, are unsupported. Neural-network dimensions do not
change the sensor's capture mode.

The writer streams 256-byte pages with bounded buffers, pads the final page
with `0xff`, checks page-program counts and CRC error status, resets the chip,
selects the new slot, and runs one inference. It then restores and verifies the
previous selection. Success requires every step. This is a runtime smoke test;
it does not verify accuracy or read back the entire flash image for SHA-256.

The module stores one atomic inventory file per physical chip under internal
flash, for example `/flash/.himax/360779f5.json`. Each slot record contains the
chip ID, source, firmware and provider versions, model identity/hash, byte counts
and state. Updates preserve the other slots. `uploading` replaces any previous
success before programming starts. Only a completed programming, inference and
restoration sequence records `programmed`; ordinary failures record `failed`.
A storage error can leave `uploading`. Mounted, writable internal flash is
required before an upload can write the accelerator.

`model slots himax camera0` queries the chip ID, slot layout and active selection
live, then joins that information with this inventory. The JSON equivalent is
`pipeline info himax camera0 slots`. Missing records show `unknown`, never empty.
The inventory is a record of SolarOS uploads, not a flash readback: programming
with another tool can make it stale. Missing, invalid or unavailable inventory
is reported explicitly; invalid files prevent new uploads until repaired.
An interrupted atomic replacement can recover its last published backup.

`model list` combines ESP-DL resident handles with a separate overview of slots
reported by the attached camera's processor provider. Slot rows use `slot=N`,
so they cannot be mistaken for resident handles. The camera advertises its
provider and source as registration metadata; ordinary cameras add no accelerator section.
A busy camera or missing provider reports its inventory unavailable and leaves
the ESP-DL listing available. Listing does not interrupt capture or change selection.

Inventory survives removal of Zoo bundles or the SD card and is keyed by chip
ID rather than the `camera0` registration name. Older provider versions stored
`.himax-SOURCE-SLOT.json` receipts beside bundles; those remain historical records
and are not automatically imported or updated. No inventory record authorizes a write.

Programming is not a transaction on accelerator flash. Cancellation, transport
failure or power loss can leave the destination slot partly written. Recovery
attempts reset the device and restore the prior selection; a power loss can
interrupt recovery too. Retry the upload to the inactive slot before selecting
it. The recovery slot and previously active model bytes remain untouched.
Removing a Zoo storage bundle does not erase accelerator flash.

## Metadata

`pipeline info himax camera0 identity` returns the chip ID. Query `metadata`
returns the factory `INFO?` response without assigning it to every slot. Some
firmware stores that information globally. Uploading does not overwrite it with
labels from an unrelated model; the checked bundle and inventory identify the
programmed model. Pipeline results retain SSCMA coordinates, scores and class IDs.
