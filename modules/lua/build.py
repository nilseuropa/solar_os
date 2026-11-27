#!/usr/bin/env python3
"""Build the Lua application with the repository's public C SDK."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib


MODULE = Path(__file__).resolve().parent


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def build(root, output, compiler):
    sys.path.insert(0, str(root / "scripts"))
    from build_native_component import HOST_IMPORTS, support_module

    verify = support_module("lua_elf_verify", root / "modules/espdl/verify.py")
    package = tomllib.loads((MODULE / "module.toml").read_text())
    vendor = root / "components/lua"
    lua = vendor / "lua/src"
    sdk = root / "include"
    flags = [
        "-std=gnu11", "-Os", "-fPIC", "-fvisibility=hidden", "-ffunction-sections",
        "-fdata-sections", "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
        "-I" + str(lua), "-I" + str(sdk), "-include", MODULE / "lua_platform.h",
    ]
    bin_prefix = str(compiler).removesuffix("gcc")
    sources = sorted(MODULE.glob("*.c"))
    sources.extend(lua / name for name in (vendor / "sources.txt").read_text().splitlines() if name)
    overrides = {
        "lauxlib.c": "-DluaL_loadfilex=solar_os_lua_unused_stdio_loader",
        "ldblib.c": "-Dfgets=solar_os_lua_console_fgets",
    }
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".lua-", dir=output) as staging:
        stage = Path(staging)
        objects = []
        for source in sources:
            obj = stage / (source.stem + ".o")
            warnings = ("-Wall", "-Wextra", "-Werror") if source.parent == MODULE else ("-Wno-format",)
            override = (overrides[source.name],) if source.name in overrides else ()
            run(compiler, *flags, *warnings, *override, "-c", source, "-o", obj)
            objects.append(obj)

        bundle = stage / "lua.o"
        local = stage / "lua-local.o"
        run(compiler, "-nostdlib", "-r", "-Wl,--gc-sections,-u,app_main,-u,solar_os_native_app_manifest",
            *objects, "-Wl,--start-group", "-lm", "-lc", "-lgcc", "-Wl,--end-group", "-o", bundle)
        # The vendor math/library archives contain fixed-address rodata
        # relocations. The ELF loader applies them to its private data image.
        run(bin_prefix + "objcopy", "--keep-global-symbol=app_main",
            "--keep-global-symbol=solar_os_native_app_manifest", "--remove-section=.eh_frame*",
            "--set-section-flags", ".rodata*=alloc,load,data,contents", bundle, local)
        artifact = stage / package["artifact"]
        run(compiler, "-nostdlib", "-nostartfiles", "-shared", "-Wl,-Bsymbolic,--gc-sections,-e,app_main",
            "-Wl,--export-dynamic-symbol=app_main,--export-dynamic-symbol=solar_os_native_app_manifest",
            "-Wl,-T," + str(root / "modules/espdl/layout.ld"), local, "-o", artifact)
        run(bin_prefix + "strip", "--strip-unneeded", artifact)
        verify.remove_noop_relocations(artifact)
        imports = verify.verify(artifact, HOST_IMPORTS)
        if "solar_os_native_host_v1" not in imports:
            raise RuntimeError("Lua did not import the versioned host ABI")

        check = stage / "elf-check"
        run("cc", "-std=c11", "-O2", "-include", "stdbool.h",
            "-I" + str(root / "tests/host"), "-I" + str(sdk), "-I" + str(root / "src/services"),
            root / "scripts/native_component_check.c", root / "src/services/solar_os_native_elf.c",
            "-o", check)
        run(check, artifact, package["id"], str(package["lifecycle_abi"]))
        artifact.replace(output / artifact.name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware", type=Path, default=MODULE.parents[1])
    parser.add_argument("--cc", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build(args.firmware.resolve(), args.output.resolve(), args.cc.absolute())
