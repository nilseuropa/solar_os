"""Reapply the ELF loader overlay after dependency restoration."""
from pathlib import Path
import re
import sys
from SCons.Script import GetOption

Import("env")
if not GetOption("clean"):
    project = Path(env.subst("$PROJECT_DIR"))
    # PlatformIO builds with SCons; CMake target dependencies alone do not run
    # these checks. Use the same check entry points as ordinary ESP-IDF builds.
    sys.path.insert(0, str(project / "scripts"))
    from generate_native_api import main as check_native_api
    from audit_native_system import main as check_native_system
    try:
        check_native_api(["--root", str(project), "--check"])
        check_native_system(["--root", str(project), "--check"])
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(f"SolarOS native API check failed: {error}") from error
    config = Path(env.subst("$BUILD_DIR")) / "generated/solar_os/solar_os_config.h"
    if re.search(r"^#define SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES 1$", config.read_text(), re.M):
        sys.path.insert(0, str(project / "scripts"))
        from patch_native_elf import patch
        try:
            patch(project / "managed_components/espressif__elf_loader")
        except (OSError, ValueError) as error:
            raise SystemExit(f"SolarOS native ELF overlay failed: {error}") from error
