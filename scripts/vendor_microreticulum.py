#!/usr/bin/env python3
"""Vendor the pinned microReticulum subset and its header-only dependencies.

Every upstream tree is checked out at a pinned revision, copied verbatim, and
then the SolarOS overlay in patches/microreticulum/overlay is copied on top.
The overlay's README.solaros.md must list every pin below, so the README and
the tree cannot drift apart.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import tempfile
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
OVERLAY = ROOT / "patches" / "microreticulum" / "overlay"

# name -> (repository, revision, [(source, destination)])
# A source ending in "/" copies the directory; destinations are relative to the output.
REPOSITORIES = {
    "microReticulum": (
        "https://github.com/zerokelvin-dev/microReticulum.git",
        "70df046ba7c1784d804cf0bcd956cfeddf45030e",
        [
            ("src/microReticulum.h", "microReticulum.h"),
            ("src/microReticulum/", "microReticulum/"),
            ("LICENSE", "LICENSE.microreticulum.txt"),
        ],
    ),
    "microStore": (
        "https://github.com/zerokelvin-dev/microStore.git",
        "03290c10be52242dd53e78192b2bb632d408f774",
        [
            ("include/microStore/", "microStore/"),
            ("LICENSE", "LICENSE.microstore.txt"),
        ],
    ),
    "MsgPack": (
        "https://github.com/hideakitai/MsgPack.git",
        "1f552c31b940d6e9063ee17a4b3fa10c47b27169",
        [
            ("MsgPack.h", "MsgPack.h"),
            ("MsgPack/Packer.h", "MsgPack/Packer.h"),
            ("MsgPack/Types.h", "MsgPack/Types.h"),
            ("MsgPack/Unpacker.h", "MsgPack/Unpacker.h"),
            ("MsgPack/Utility.h", "MsgPack/Utility.h"),
            ("LICENSE", "LICENSE.msgpack.txt"),
        ],
    ),
    "ArxContainer": (
        "https://github.com/hideakitai/ArxContainer.git",
        "d6affcd0bc83219b863c20abf7c269214db8db2a",
        [
            ("ArxContainer.h", "ArxContainer.h"),
            ("ArxContainer/", "ArxContainer/"),
            ("LICENSE", "LICENSE.arxcontainer.txt"),
        ],
    ),
    "ArxTypeTraits": (
        "https://github.com/hideakitai/ArxTypeTraits.git",
        "702de9cc59c7e047cdc169ae3547718b289d2c02",
        [
            ("ArxTypeTraits.h", "ArxTypeTraits.h"),
            ("ArxTypeTraits/", "ArxTypeTraits/"),
            ("LICENSE", "LICENSE.arxtypetraits.txt"),
        ],
    ),
    "DebugLog": (
        "https://github.com/hideakitai/DebugLog.git",
        "b581f7dde6c276c5df684e2328f406d9754d2f46",
        [
            ("DebugLog.h", "DebugLog.h"),
            ("DebugLog/", "DebugLog/"),
            ("DebugLogDisable.h", "DebugLogDisable.h"),
            ("DebugLogEnable.h", "DebugLogEnable.h"),
            ("DebugLogRestoreState.h", "DebugLogRestoreState.h"),
            ("LICENSE", "LICENSE.debuglog.txt"),
        ],
    ),
}

ARDUINOJSON_URL = (
    "https://github.com/bblanchon/ArduinoJson/releases/download/v7.4.2/"
    "ArduinoJson-v7.4.2.h"
)
ARDUINOJSON_SHA256 = "a05ac98f4481d2398c103ca5ffcce8e2fcd7fa2fa1d8d9f38d5757454f448d97"
ARDUINOJSON_LICENSE_URL = (
    "https://raw.githubusercontent.com/bblanchon/ArduinoJson/"
    "733bc4ee82630c88c0a619a883cd3a206efae977/LICENSE.txt"
)
ARDUINOJSON_LICENSE_SHA256 = "ebba0906d6c8b3daa8a1b59acb2d3a416485dd77fc10f6dd4c9ed2cbe467ee2d"

# Upstream paths that SolarOS does not build; removed after the verbatim copy.
EXCLUDED = [
    "microReticulum/main.cpp",
    "microReticulum/Provisioning",
    "microReticulum/Utilities/tlsf",
    "microReticulum/Utilities/heatshrink/greatest.h",
    "microReticulum/Cryptography/CBC.h",
    "microReticulum/Cryptography/CBC.cpp",
    "microReticulum/Utilities/heatshrink/dec_sm.dot",
    "microReticulum/Utilities/heatshrink/enc_sm.dot",
    "microStore/Adapters/FlashFSFileSystem.h",
    "microStore/Adapters/InternalFSFileSystem.h",
    "microStore/Adapters/LittleFSFileSystem.h",
    "microStore/Adapters/NoopFileSystem.h",
    "microStore/Adapters/SDFileSystem.h",
    "microStore/Adapters/SPIFFSFileSystem.h",
    "microStore/Adapters/StdioFileSystem.h",
]


def checkout(name: str, url: str, revision: str, source: Path | None, work: Path) -> Path:
    # An existing checkout is cloned from, not checked out in place.
    origin = url
    if source is not None and (source / name).is_dir():
        origin = str(source / name)
    path = work / name
    subprocess.run(["git", "clone", "--quiet", origin, str(path)], check=True)
    subprocess.run(["git", "-C", str(path), "checkout", "--quiet", revision], check=True)
    head = subprocess.run(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    if head != revision:
        raise ValueError(f"{name}: checked out {head}, expected {revision}")
    return path


def copy(checkout_path: Path, source: str, destination: str, output: Path) -> None:
    src = checkout_path / source
    dst = output / destination
    if source.endswith("/"):
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def download(url: str, expected_hash: str) -> bytes:
    with urllib.request.urlopen(url) as response:
        data = response.read()
    actual = hashlib.sha256(data).hexdigest()
    if actual != expected_hash:
        raise ValueError(f"SHA-256 mismatch for {url}: {actual} != {expected_hash}")
    return data


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        type=Path,
        help="directory holding existing checkouts named after each repository",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "src" / "vendor" / "microreticulum",
    )
    args = parser.parse_args()

    check_readme()

    with tempfile.TemporaryDirectory() as tmp:
        staging = Path(tmp) / "output"
        staging.mkdir()
        for name, (url, revision, files) in REPOSITORIES.items():
            path = checkout(name, url, revision, args.source, Path(tmp))
            for source, destination in files:
                copy(path, source, destination, staging)
        build(staging)
        if args.output.exists():
            shutil.rmtree(args.output)
        shutil.copytree(staging, args.output)
    print(args.output)


def check_readme() -> None:
    readme = (OVERLAY / "README.solaros.md").read_text()
    for name, (_url, revision, _files) in REPOSITORIES.items():
        if f"`{revision}`" not in readme:
            raise ValueError(f"README.solaros.md does not list {name} at {revision}")
    if ARDUINOJSON_URL.rsplit("/", 2)[-2] not in readme:
        raise ValueError("README.solaros.md does not list the ArduinoJson version")


def build(output: Path) -> None:
    (output / "ArduinoJson.h").write_bytes(
        download(ARDUINOJSON_URL, ARDUINOJSON_SHA256)
    )
    (output / "LICENSE.arduinojson.txt").write_bytes(
        download(ARDUINOJSON_LICENSE_URL, ARDUINOJSON_LICENSE_SHA256)
    )

    for relative in EXCLUDED:
        target = output / relative
        if target.is_dir():
            shutil.rmtree(target)
        else:
            target.unlink()

    shutil.copytree(OVERLAY, output, dirs_exist_ok=True)


if __name__ == "__main__":
    main()
