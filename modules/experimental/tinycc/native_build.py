#!/usr/bin/env python3
"""Build the isolated, bounded on-device compiler library and test client."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

from build import HERE, ROOT, CACHE, prepare, replace, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    source = prepare(args.archive)
    replace(source / "libtcc.c", 'tcc_open_bf(s1, "<string>", len);',
            'tcc_open_bf(s1, solar_os_cc_source_name(), len);')
    # set_elf_sym can append a name already inside its own string table.
    # An always-moving bounded realloc must rebase that alias after growth.
    replace(source / "tccelf.c",
            "    len = strlen(sym) + 1;\n    offset = s->data_offset;\n    ptr = section_ptr_add(s, len);\n    memmove(ptr, sym, len);",
            "    uintptr_t alias = (uintptr_t)sym - (uintptr_t)s->data;\n"
            "    int internal = s->data && alias < s->data_allocated;\n"
            "    len = strlen(sym) + 1;\n    offset = s->data_offset;\n"
            "    ptr = section_ptr_add(s, len);\n"
            "    if (internal) sym = (const char *)s->data + alias;\n    memmove(ptr, sym, len);")
    # Cooperative checks cover preprocessor expansion as well as emitted code.
    for file, old, new in (
        ("tccpp.c", "static void next_nomacro(void)\n{", "static void next_nomacro(void)\n{\n    solar_os_cc_checkpoint();"),
        ("tccpp.c", "ST_FUNC void next(void)\n{", "ST_FUNC void next(void)\n{\n    solar_os_cc_checkpoint();"),
        ("xtensa-gen.c", "static void xt_byte(unsigned value)\n{", "static void xt_byte(unsigned value)\n{\n    solar_os_cc_checkpoint();"),
    ):
        replace(source / file, old, new)
    with (source / "libtcc.c").open("a") as stream:
        stream.write('\n#include "embedded.c"\n')
    shutil.copyfile(HERE / "embedded.c", source / "embedded.c")
    # Declarations precede the single-source compiler includes.
    (source / "embedded-hook.h").write_text("void solar_os_cc_checkpoint(void);\nconst char *solar_os_cc_source_name(void);\n")
    output = CACHE / "native"; output.mkdir(parents=True, exist_ok=True)
    cc = Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc"
    prefix = str(cc)[:-3]
    flags = ["-std=gnu11", "-Os", "-g0", "-fPIC", "-fvisibility=hidden", "-ffunction-sections",
             "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-fno-builtin",
             "-I" + str(ROOT / "include"), "-I" + str(HERE / "include"), "-I" + str(source)]
    # The standalone ELF validator only needs ESP error constants/compiler
    # attributes; it has no configured ESP-IDF runtime dependencies.
    (output / "sdkconfig.h").write_text("#pragma once\n")
    flags += ["-I" + str(ROOT / "src/services"), "-I" + str(output),
              "-I" + str(Path.home() / ".platformio/packages/framework-espidf/components/esp_common/include")]
    run(cc, *flags, "-finstrument-functions", "-DTCC_TARGET_XTENSA=1", "-DCONFIG_TCC_STATIC=1",
        "-DCONFIG_TCC_SEMLOCK=0", "-DCONFIG_TCC_BACKTRACE=0", "-DCONFIG_TCC_BCHECK=0",
        "-include", source / "embedded-hook.h", "-c", source / "libtcc.c", "-o", output / "core.raw.o")
    raw = (output / "core.raw.o").read_bytes()
    shoff = struct.unpack_from("<I", raw, 32)[0]
    count, names = struct.unpack_from("<HH", raw, 48)
    sections = [struct.unpack_from("<10I", raw, shoff + i*40) for i in range(count)]
    strings = sections[names]
    rename = []
    for i, s in enumerate(sections):
        if not (s[2] & 1): continue
        at = strings[4] + s[0]; name = raw[at:raw.index(0, at)].decode()
        group = "bss" if s[1] == 8 else "data"
        rename += ["--rename-section", f"{name}=.tcccore.{group}.{i}"]
    run(prefix + "objcopy", *rename, output / "core.raw.o", output / "core.o")
    # Embed a target SDK as bounded read-only files, without a host filesystem.
    headers = {p.name: p for p in (ROOT / "include").glob("solar_os_native*abi.h")}
    headers.update({p.name: p for p in (HERE / "sdk").glob("*.h")})
    for name in ("stddef.h", "stdbool.h", "stdarg.h"):
        headers[name] = source / "include" / name
    headers["hello_library_api.h"] = ROOT / "modules/examples/hello-library/main/hello_library_api.h"
    sdk = []
    for name, path in sorted(headers.items()):
        data = path.read_bytes()
        sdk.append("{%s, %s, %d}," % (json.dumps(name), json.dumps(data.decode()), len(data)))
    (output / "sdk.inc").write_text("\n".join(sdk) + "\n")
    for name in ("library_probe", "scalar_probe"):
        (output / (name + ".inc")).write_text(json.dumps((HERE / "examples" / (name + ".c")).read_text()) + "\n")
    for name, path in (("runtime", HERE / "runtime.c"), ("module", HERE / "module.c"), ("client", HERE / "client.c"),
                       ("validator", ROOT / "src/services/solar_os_native_elf.c")):
        run(cc, *flags, "-I", output, "-c", path, "-o", output / (name + ".o"))
    layout = (ROOT / "modules/espdl/layout.ld").read_text()
    marker = "*(.solaros_cpp_anchor)"
    layout = layout.replace(marker, marker + " . = ALIGN(16); __cc_data_start = .; *(.tcccore.data*) __cc_data_end = .;")
    marker = "*(.bss .bss.* .gnu.linkonce.b.*)"
    if layout.count(marker) != 1: raise RuntimeError("review module BSS linker layout")
    layout = layout.replace(marker, ". = ALIGN(16); __cc_bss_start = .; *(.tcccore.bss*) __cc_bss_end = .; " + marker)
    (output / "layout.ld").write_text(layout)
    libgcc = Path(subprocess.check_output([str(cc), "-print-libgcc-file-name"], text=True).strip())
    run(prefix + "objcopy", "--set-section-flags", ".rodata*=alloc,load,data,contents",
        "--remove-section=.eh_frame", libgcc, output / "libgcc.a")
    # Hide all private runtime/compiler names; only the versioned host imports escape.
    for name, objects in (("tinycc.library", ["core.o", "runtime.o", "module.o"]), ("cc.app", ["client.o", "validator.o"])):
        run(cc, "-r", "-nostdlib", *[output / p for p in objects], output / "libgcc.a", "-o", output / (name + ".o"))
        run(prefix + "objcopy", "--keep-global-symbol=app_main", output / (name + ".o"), output / (name + ".local.o"))
        run(cc, "-nostdlib", "-nostartfiles", "-shared", "-Wl,-Bsymbolic", "-Wl,--gc-sections",
            "-Wl,-e,app_main", "-Wl,-T," + str(output / "layout.ld"), output / (name + ".local.o"),
            "-o", output / (name + ".elf"))
        run(prefix + "strip", "--strip-unneeded", "--remove-section=.got.loc", "--remove-section=.dynamic",
            "--remove-section=.xt.lit", "--remove-section=.xt.prop", "--remove-section=.xtensa.info", output / (name + ".elf"))
    import sys
    sys.path.insert(0, str(ROOT / "modules/espdl"))
    from verify import verify
    run("cc", "-std=c11", "-O2", "-include", "stdbool.h", "-I", ROOT / "tests/host",
        "-I", ROOT / "include", "-I", ROOT / "src/services",
        HERE / "elf_check.c", ROOT / "src/services/solar_os_native_elf.c", "-o", CACHE / "elf-check")
    report = {"upstream": json.loads((HERE / "upstream.json").read_text())["revision"],
              "gcc": subprocess.check_output([str(cc), "-dumpfullversion"], text=True).strip(),
              "sdk": {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in headers.items()},
              "sources": {os.path.relpath(path, HERE): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in [ROOT / "src/services/solar_os_native_elf.c",
                                       ROOT / "src/services/solar_os_native_elf.h"] + [HERE / name for name in ("runtime.c", "embedded.c", "client.c", "module.c",
                                                               "native_build.py", "xtensa-gen.c", "xtensa-link.c",
                                                               "include/solar_os_cc_abi.h")]}, "artifacts": {}}
    for name in ("tinycc.library.elf", "cc.app.elf"):
        path = output / name
        imports = verify(path, {"solar_os_native_library_host_v1", "solar_os_native_host_v1",
                                "memcpy", "memmove", "memset", "memcmp", "strlen", "setjmp", "longjmp"})
        run(CACHE / "elf-check", path)
        report["artifacts"][name] = {"bytes": path.stat().st_size, "imports": imports,
                                     "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["artifacts"], indent=2))


if __name__ == "__main__": main()
