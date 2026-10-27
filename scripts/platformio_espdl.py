#!/usr/bin/env python3
"""Repair restored ESP-DL dependencies after framework setup, before compilation."""

from pathlib import Path
import re
import sys

from SCons.Script import GetOption

Import("env")

if not GetOption("clean"):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    config = Path(env.subst("$BUILD_DIR")) / "generated/solar_os/solar_os_config.h"
    if re.search(r"^#define SOLAR_OS_PACKAGE_SERVICE_INFERENCE 1$",
                 config.read_text(encoding="utf-8"), re.M):
        sys.path.insert(0, str(project_dir / "scripts"))
        from patch_espdl import patch

        print("SolarOS: verifying ESP-DL overlay before compilation")
        try:
            patch(project_dir / "managed_components/espressif__esp-dl")
        except (OSError, ValueError) as error:
            raise SystemExit(f"SolarOS ESP-DL overlay failed: {error}") from error
