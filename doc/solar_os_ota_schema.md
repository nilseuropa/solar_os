# SolarOS OTA Manifest Schema

SolarOS release artifacts are described by two JSON documents:

- `index.json`: a compact release index listing all board/flavor artifacts in a release or channel.
- `manifest.json`: complete metadata for one firmware image.

The schemas live in:

- `schemas/solaros-release-index.schema.json`
- `schemas/solaros-firmware-manifest.schema.json`

The current firmware still supports the simple `version.txt` plus `firmware.bin` OTA layout. These JSON files are the forward contract for deployment tooling, CI, and future OTA selection.

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

A future manifest-aware OTA flow should be:

1. Device fetches `<base>/index.json`.
2. Device finds an artifact where `board_id` matches the compiled board id and `flavor` matches the OTA target flavor.
3. Device compares artifact `version` with the running app version.
4. Device downloads the selected `firmware` path.
5. Device verifies `sha256` and checks the image fits the inactive OTA partition.
6. Device writes the image to the inactive OTA partition and switches boot partition.

The per-artifact `manifest.json` can be fetched for detailed display, logging, or stricter verification, but the release index intentionally includes enough information for constrained device selection.
