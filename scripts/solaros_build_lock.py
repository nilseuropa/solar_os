#!/usr/bin/env python3
"""Serialize PlatformIO builds that share ESP-IDF component-manager state."""

from __future__ import annotations

import builtins
import os
from pathlib import Path
from typing import TextIO

if os.name == "nt":
    fcntl = None
else:
    import fcntl


_REGISTRY_NAME = "_solaros_platformio_build_locks"


def _lock_registry() -> dict[str, TextIO]:
    registry = getattr(builtins, _REGISTRY_NAME, None)
    if registry is None:
        registry = {}
        setattr(builtins, _REGISTRY_NAME, registry)
    return registry


def acquire_project_build_lock(project_dir: Path, build_environment: str) -> None:
    """Hold the project build lock until the PlatformIO process exits."""
    if fcntl is None:
        print(
            "SolarOS build locking is unavailable on Windows; "
            "do not run concurrent PlatformIO builds from the same checkout"
        )
        return

    lock_path = project_dir.resolve() / ".pio" / "solaros-build.lock"
    lock_key = str(lock_path)
    registry = _lock_registry()
    if lock_key in registry:
        return

    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_file = lock_path.open("a+", encoding="utf-8")
    try:
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock_file.seek(0)
        owner = lock_file.read().strip() or "unknown build"
        print(
            "SolarOS build lock busy "
            f"({owner}); waiting to protect shared ESP-IDF component state"
        )
        fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
        print("SolarOS build lock acquired")

    lock_file.seek(0)
    lock_file.truncate()
    lock_file.write(f"pid={os.getpid()} env={build_environment}\n")
    lock_file.flush()
    registry[lock_key] = lock_file
