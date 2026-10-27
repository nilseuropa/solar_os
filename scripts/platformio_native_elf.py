"""Reapply the ELF loader overlay after dependency restoration."""
from pathlib import Path
import re
import sys
from SCons.Script import GetOption

Import("env")
if not GetOption("clean"):
    project = Path(env.subst("$PROJECT_DIR"))
    config = Path(env.subst("$BUILD_DIR")) / "generated/solar_os/solar_os_config.h"
    if re.search(r"^#define SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES 1$", config.read_text(), re.M):
        sys.path.insert(0, str(project / "scripts"))
        from patch_native_elf import patch
        try:
            patch(project / "managed_components/espressif__elf_loader")
        except (OSError, ValueError) as error:
            raise SystemExit(f"SolarOS native ELF overlay failed: {error}") from error
