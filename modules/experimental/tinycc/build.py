#!/usr/bin/env python3
"""Prepare a pinned TinyCC checkout with the experimental Xtensa backend."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CACHE = ROOT / ".pio/tinycc"


def run(*args, cwd=None):
    subprocess.run([str(a) for a in args], cwd=cwd, check=True, timeout=120)


def replace(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        raise RuntimeError(f"upstream patch context changed: {path.name}: {old[:60]}")
    path.write_text(text.replace(old, new))


def prepare(archive=None):
    pin = json.loads((HERE / "upstream.json").read_text())
    CACHE.mkdir(parents=True, exist_ok=True)
    archive = Path(archive) if archive else CACHE / "tinycc.tar.gz"
    if not archive.exists():
        urllib.request.urlretrieve(pin["archive"], archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != pin["sha256"]:
        raise RuntimeError("TinyCC archive checksum mismatch")
    stage = CACHE / "source"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir()
    with tarfile.open(archive) as tar:
        tar.extractall(stage, filter="data")
    source = stage / ("tinycc-" + pin["revision"])
    for name in ("xtensa-gen.c", "xtensa-link.c"):
        shutil.copyfile(HERE / name, source / name)
    replace(source / "tcc.h",
            "!defined(TCC_TARGET_X86_64) && !defined(TCC_TARGET_RISCV64)",
            "!defined(TCC_TARGET_X86_64) && !defined(TCC_TARGET_RISCV64) && !defined(TCC_TARGET_XTENSA)")
    replace(source / "tcc.h", '# include "riscv64-asm.c"\n#else',
            '# include "riscv64-asm.c"\n#elif defined(TCC_TARGET_XTENSA)\n# include "xtensa-gen.c"\n# include "xtensa-link.c"\n#else')
    replace(source / "libtcc.c", '#include "riscv64-asm.c"\n#else',
            '#include "riscv64-asm.c"\n#elif defined(TCC_TARGET_XTENSA)\n#include "xtensa-gen.c"\n#include "xtensa-link.c"\n#else')
    replace(source / "tccgen.c", "    ind = cur_text_section->data_offset;\n    if (sym->a.aligned)",
            "    ind = cur_text_section->data_offset;\n#ifdef TCC_TARGET_XTENSA\n"
            "    while (ind & 3) gen_fill_nops(1);\n#endif\n    if (sym->a.aligned)")
    replace(source/"tccgen.c", "    if (t == VT_LLONG)\n        return REG_IRE2;",
            "    if (t == VT_LLONG\n#ifdef TCC_TARGET_XTENSA\n        || t == VT_DOUBLE || t == VT_LDOUBLE\n#endif\n       ) return REG_IRE2;")
    replace(source/"tccgen.c", "#ifdef TCC_TARGET_RISCV64\n    case TOK_builtin_va_start:",
            "#ifdef TCC_TARGET_XTENSA\n    case TOK_builtin_va_start:\n"
            '        parse_builtin_params(0, "ee");\n        gen_va_start();\n        break;\n#endif\n'
            "#ifdef TCC_TARGET_RISCV64\n    case TOK_builtin_va_start:")
    replace(source/"tcctok.h", "#elif defined TCC_TARGET_RISCV64\n     DEF(TOK_builtin_va_start,",
            "#elif defined TCC_TARGET_RISCV64 || defined TCC_TARGET_XTENSA\n     DEF(TOK_builtin_va_start,")
    replace(source/"tcc.h", "/* ------------ c67-gen.c ------------ */",
            "#ifdef TCC_TARGET_XTENSA\nST_FUNC void gen_va_start(void);\nST_FUNC void gen_opl_runtime(int op);\n#endif\n\n/* ------------ c67-gen.c ------------ */")
    replace(source/"tccgen.c", "    switch(op) {\n    case '/':\n    case TOK_PDIV:\n        func = TOK___divdi3;",
            "#ifdef TCC_TARGET_XTENSA\n    if(op=='*' || (op>=TOK_ULT && op<=TOK_GT)) { gen_opl_runtime(op); return; }\n#endif\n"
            "    switch(op) {\n    case '/':\n    case TOK_PDIV:\n        func = TOK___divdi3;")
    replace(source/"include/tccdefs.h", "#elif defined __riscv\n    typedef char *__builtin_va_list;",
            "#elif defined __XTENSA__\n    typedef struct { char *__va_stk, *__va_reg; int __va_ndx; } __builtin_va_list;\n"
            "#elif defined __riscv\n    typedef char *__builtin_va_list;")
    replace(source/"conftest.c", '    "__riscv",              "TCC_TARGET_RISCV64",',
            '    "__riscv",              "TCC_TARGET_RISCV64",\n    "__XTENSA__",           "TCC_TARGET_XTENSA",')
    (source / "config.h").write_text('''#define TCC_VERSION "0.9.28rc-solaros-xtensa"
#define CC_NAME CC_gcc
#define CONFIG_TCCDIR "."
#define CONFIG_TCC_PREDEFS 1
''')
    run("cc", "-DC2STR", "conftest.c", "-o", "c2str", cwd=source)
    run(source / "c2str", "include/tccdefs.h", "tccdefs_.h", cwd=source)
    from support_build import prepare_support
    prepare_support(HERE,CACHE,source)
    shutil.copyfile(HERE/"support-bridge.c",source/"support-bridge.c")
    with (source/"libtcc.c").open("a") as stream:
        stream.write('\n#include "support-bridge.c"\n')
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--s3-check", action="store_true", help="cross-compile the compiler core for ESP32-S3")
    parser.add_argument("--gcc-provider", action="store_true", help="build the unchanged GCC hello-library for hardware tests")
    args = parser.parse_args()
    source = prepare(args.archive)
    flags = ["-std=gnu11", "-O2", "-g", "-DTCC_TARGET_XTENSA=1",
             "-DCONFIG_TCC_STATIC=1", "-DCONFIG_TCC_SEMLOCK=0",
             "-DCONFIG_TCC_BACKTRACE=0", "-DCONFIG_TCC_BCHECK=0"]
    run("cc", *flags, "-c", "libtcc.c", "-o", CACHE / "libtcc-xtensa.o", cwd=source)
    run("cc", *flags, "-I", source, HERE / "driver.c", CACHE / "libtcc-xtensa.o",
        "-lm", "-ldl", "-o", CACHE / "solar-tcc", cwd=source)
    run("cc", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-DSOLAR_TCC_MODULE_TOOL",
        "-I", source, HERE / "module.c", "-o", CACHE / "module")
    run("cc", "-std=c11", "-O2", "-include", "stdbool.h", "-I", ROOT / "tests/host",
        "-I", ROOT / "include", "-I", ROOT / "src/services",
        HERE / "elf_check.c", ROOT / "src/services/solar_os_native_elf.c", "-o", CACHE / "elf-check")
    cc = Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc"
    if args.s3_check:
        cross_flags = flags[:1] + ["-Os", "-g0", "-fPIC", "-ffunction-sections", "-fdata-sections",
                                  "-fstack-usage", *flags[3:]]
        run(cc, *cross_flags, "-c", source / "libtcc.c", "-o", CACHE / "libtcc-on-s3.o")
        run(cc, *cross_flags, "-I", source, "-c", HERE / "host_width.c", "-o", CACHE / "host-width.o")
        run(cc, "-std=c11", "-Os", "-I", source, "-c", HERE / "module.c", "-o", CACHE / "module-on-s3.o")
    if args.gcc_provider:
        run(cc, "-std=c11", "-Os", "-fPIC", "-fvisibility=hidden", "-I", ROOT / "include",
            "-I", ROOT / "modules/examples/hello-library/main", "-c",
            ROOT / "modules/examples/hello-library/main/hello_library.c", "-o", CACHE / "hello-library-gcc.o")
        run(cc, "-nostdlib", "-nostartfiles", "-shared", "-Wl,-Bsymbolic", "-Wl,-e,main",
            "-Wl,-T," + str(ROOT / "modules/espdl/layout.ld"), CACHE / "hello-library-gcc.o",
            "-o", CACHE / "hello-library.library.elf")
    print(CACHE / "solar-tcc")


if __name__ == "__main__":
    main()
