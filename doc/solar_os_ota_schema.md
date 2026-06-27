# SolarOS OTA Manifest Schema

SolarOS release artifacts are described by two JSON documents:

- `index.json`: a compact release index listing all board/flavor artifacts in a release or channel.
- `manifest.json`: complete metadata for one firmware image.

The schemas live in:

- `schemas/solaros-release-index.schema.json`
- `schemas/solaros-firmware-manifest.schema.json`

The firmware reads `index.json` from the configured OTA base URL, selects the matching board/flavor artifact, and downloads that artifact's `firmware` path. `version.txt` is still produced beside each firmware image for humans and simple tooling, but OTA selection is driven by the release index.

## Release Layout

Recommended hosted layout:

```text
solaros/
  latest/
    index.json
    waveshare_esp32_s3_rlcd_4_2/
      full/
        manifest.json
        version.txt
        firmware.bin
        firmware.factory.bin
      core/
        manifest.json
        version.txt
        firmware.bin
    esp32_s3_devkitc1_n16r8/
      core/
        manifest.json
        version.txt
        firmware.bin
      netrunner/
        manifest.json
        version.txt
        firmware.bin
  2.6.0/
    index.json
    ...
```

`latest` may be a symlink to a versioned directory or a real directory populated by the deploy job. Paths inside `index.json` are relative to `base_url`. Paths inside a per-artifact `manifest.json` are relative to the directory that contains that manifest.

## Release Index

`index.json` is used to select an artifact by board and flavor without fetching every per-artifact manifest.

Required top-level fields:

- `schema`: `solaros.release_index`
- `schema_version`: currently `1`
- `project`: `SolarOS`
- `release.version`: release version
- `artifacts`: one entry per board/flavor build

Each artifact entry includes:

- `board_id`
- `flavor`
- `version`
- `manifest`
- `firmware`
- `version_file`
- `size`
- `sha256`

Optional but recommended fields:

- `base_url`
- `release.channel`
- `release.created_utc`
- `release.git_commit`
- `board_name`
- `factory_firmware`
- `capabilities`
- `groups`
- `packages`

## Firmware Manifest

`manifest.json` is the complete record for one binary.

Required top-level fields:

- `schema`: `solaros.firmware_manifest`
- `schema_version`: currently `1`
- `project`: `SolarOS`
- `version`
- `board.id`
- `board.name`
- `board.capabilities`
- `flavor.name`
- `flavor.groups`
- `flavor.packages`
- `artifact.firmware`
- `artifact.version_file`
- `artifact.size`
- `artifact.sha256`

Optional but recommended fields:

- `channel`
- `created_utc`
- `board.psram_bytes`
- `flavor.description`
- `artifact.factory_firmware`
- `artifact.content_type`
- `build.git_commit`
- `build.git_dirty`
- `build.platformio_env`
- `build.idf_version`
- `build.compiler`
- `build.partition_table`
- `build.app_partition_size`

## OTA Selection

The firmware OTA flow is:

1. Device fetches `<base>/index.json`.
2. Device finds an artifact where `board_id` matches the compiled board id and `flavor` matches the OTA target flavor.
3. Device compares artifact `version` with the running app version.
4. Device downloads the selected `firmware` path and streams it into the inactive OTA partition while hashing the received bytes.
5. Device verifies the received firmware stream against artifact `sha256`.
6. `esp_https_ota` finalizes image validation, then switches the boot partition.

The release index includes `sha256` and image `size`, so constrained devices can select and verify an artifact without first fetching the per-artifact manifest. The per-artifact `manifest.json` can be fetched later for detailed display, logging, signatures, or stricter verification.
