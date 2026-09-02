#!/usr/bin/env python3
"""Shared SolarOS flash/update layout definitions."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


MIB = 1024 * 1024


@dataclass(frozen=True)
class UpdateLayout:
    key: str
    label: str
    description: str
    partition_file: str
    app_bytes: int
    ota: bool


_LAYOUTS = {
    16 * MIB: (
        UpdateLayout("ota", "OTA updates", "two 7 MiB application slots",
                     "partitions.csv", 0x700000, True),
        UpdateLayout("single", "Serial updates", "one 14 MiB application slot",
                     "partitions_16mb_single.csv", 0xE00000, False),
    ),
    8 * MIB: (
        UpdateLayout("ota", "OTA updates", "two 3.875 MiB application slots",
                     "partitions_8mb.csv", 0x3E0000, True),
        UpdateLayout("single", "Serial updates", "one 7 MiB application slot",
                     "partitions_8mb_single.csv", 0x700000, False),
    ),
    4 * MIB: (
        UpdateLayout("single", "Serial updates only", "one 3.8125 MiB application slot",
                     "partitions_4mb.csv", 0x3D0000, False),
    ),
}


def layouts_for_flash(flash_bytes: int) -> tuple[UpdateLayout, ...]:
    try:
        return _LAYOUTS[flash_bytes]
    except KeyError as exc:
        raise ValueError(
            f"no SolarOS update layouts for {flash_bytes} bytes of flash"
        ) from exc


def select_layout(configured_partition: str,
                  requested: str | None = None) -> UpdateLayout:
    partition_name = Path(configured_partition).name
    matching_flash = next(
        (
            flash_bytes
            for flash_bytes, layouts in _LAYOUTS.items()
            if any(layout.partition_file == partition_name for layout in layouts)
        ),
        None,
    )
    if matching_flash is None:
        raise ValueError(
            f"SolarOS update layouts do not recognize partition table: {configured_partition}"
        )
    layouts = layouts_for_flash(matching_flash)
    default = next(
        layout.key for layout in layouts if layout.partition_file == partition_name
    )
    key = requested or default
    if key not in ("ota", "single"):
        raise ValueError(
            f"unsupported SolarOS update layout {key!r}; expected 'ota' or 'single'"
        )
    result = next((layout for layout in layouts if layout.key == key), None)
    if result is None:
        raise ValueError(
            f"SolarOS {key} layout is not available with {partition_name}"
        )
    return result
