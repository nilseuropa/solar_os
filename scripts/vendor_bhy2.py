#!/usr/bin/env python3
"""Vendor the pinned Bosch BHy2 SensorAPI files used by the BHI260AP driver."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import urllib.request


REVISION = "177681d120ac42760c9f19505697ccd5c7103e87"
FILES = {
    "LICENSE": "2a54d2ef6beedf4da1cb20887b476bf5964e44cc47f20345635a18ff98b33d56",
    "bhy2.c": "f88d37ccb8a625c949bc429ecf507da8a6d0be964f81b6c95824a882394d2613",
    "bhy2.h": "19abba48fcaf50ddf4427c2875687b8e308f6265c80d53cf1516e9c7ee1ff598",
    "bhy2_defs.h": "129e8326cfc8409646bd86a6f2ee03bd0966198b4b47862d985ad4313b10cc83",
    "bhy2_hif.c": "e95fce15b24da31198fd24fc894cfa66511ca390c733df597d2200020744009b",
    "bhy2_hif.h": "0b51addee4840d65ba6c9b974470ddbb0fa45e4c0999c3b3f316a0ec265ab28e",
    "firmware/bhi260ap/BHI260AP.fw":
        "318def511fd8eb762bf1e61cf02cfd6ecac70f8627d3bacfa7d24a1641f9e915",
}


def fetch(relative: str, source: Path | None) -> bytes:
    if source is not None:
        return (source / relative).read_bytes()
    url = (
        "https://raw.githubusercontent.com/boschsensortec/"
        f"BHY2_SensorAPI/{REVISION}/{relative}"
    )
    with urllib.request.urlopen(url) as response:
        return response.read()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        type=Path,
        help="use an existing BHY2_SensorAPI checkout instead of downloading",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "src" / "vendor" / "bhy2",
    )
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    for relative, expected_hash in FILES.items():
        data = fetch(relative, args.source)
        actual_hash = hashlib.sha256(data).hexdigest()
        if actual_hash != expected_hash:
            raise ValueError(
                f"SHA-256 mismatch for {relative}: {actual_hash} != {expected_hash}"
            )
        destination = args.output / Path(relative).name
        destination.write_bytes(data)
        print(destination.relative_to(Path.cwd()))


if __name__ == "__main__":
    main()
