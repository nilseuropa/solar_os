#!/usr/bin/env python3
"""Build the isolated, bounded on-device compiler library and test client."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tomllib

from build import HERE, ROOT, CACHE, prepare, replace, run
from isolate import isolate

sys.path.insert(0, str(ROOT / 'scripts'))
from generate_native_api import BANNER
from pack_lz4 import compress_block


def sdk_headers(source):
    headers = {p.name: p for p in (ROOT / "include").glob("solar_os_native*abi.h")}
    headers.update({p.name: p for p in (ROOT / "include").glob("solar_os_native_client*.h")})
    headers.update({p.name: p for p in (ROOT / "include").glob("solar_os_*_types_v1.h")})
    headers.update({p.name: p for p in (ROOT / "include").glob("solar_os_robot*.h")})
    headers["solar_os_key_codes.h"] = ROOT / "include/solar_os_key_codes.h"
    headers["solar_os_cc_abi.h"] = HERE / "include/solar_os_cc_abi.h"
    headers.update({p.name: p for p in (HERE / "sdk").glob("*.h")})
    for name in ("stddef.h", "stdbool.h", "stdarg.h"):
        if name not in headers: headers[name] = source / "include" / name
    headers["hello_library_api.h"] = ROOT / "modules/examples/hello-library/main/hello_library_api.h"
    return headers


HEADER_COMMENTS_AND_LITERALS = re.compile(
    rb'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|/\*.*?\*/|//(?:\\\r?\n|[^\r\n])*''', re.DOTALL)


def omit_standalone_comments(data):
    def replace_comment(match):
        comment = match.group()
        if not comment.startswith(b"/*"):
            return comment
        line_start = data.rfind(b"\n", 0, match.start()) + 1
        line_end = data.find(b"\n", match.end())
        if line_end < 0:
            line_end = len(data)
        previous = data[max(0, line_start - 3):line_start]
        if (data[line_start:match.start()].strip() or data[match.end():line_end].strip() or
                previous.endswith((b"\\\n", b"\\\r\n"))):
            return comment
        # Preserve physical line numbers and leave inline comments in place.
        return b"\n" * comment.count(b"\n")

    return HEADER_COMMENTS_AND_LITERALS.sub(replace_comment, data)


def embedded_header(path):
    data = path.read_bytes()
    banner = BANNER.encode()
    if not data.startswith(banner):
        return data
    # Keep the generated header readable with two spaces per indentation
    # level. Keep spliced lines intact because their spaces can be token data.
    lines = []
    continued = False
    for line in omit_standalone_comments(data[len(banner):]).splitlines(keepends=True):
        if not continued:
            spaces = len(line) - len(line.lstrip(b" "))
            line = line[spaces // 2:]
        lines.append(line)
        continued = line.rstrip(b"\r\n").endswith(b"\\")
    return b"".join(lines)


def pack_sdk_headers(headers):
    # Match TinyCC's IO_BUF_SIZE. Its sequential reads decode directly into
    # the existing input buffer, including the shorter final block.
    block_bytes = 8192
    data = bytearray()
    offsets = []
    files = []
    for name, text in sorted(headers.items()):
        files.append(f"    {{{json.dumps(name)}, {len(text)}, {len(offsets)}}},")
        for start in range(0, len(text), block_bytes):
            offsets.append(len(data))
            data.extend(compress_block(text[start:start + block_bytes]))
    offsets.append(len(data))

    lines = [
        "/* Generated from the embedded C SDK. */",
        f"#define SOLAR_OS_CC_SDK_BLOCK_BYTES {block_bytes}U",
        "static const uint8_t sdk_data[] =",
    ]
    if not data:
        lines.append('    ""')
    for start in range(0, len(data), 20):
        lines.append('    "' + ''.join(f'\\x{byte:02x}' for byte in data[start:start + 20]) + '"')
    lines += ["    ;", "static const uint32_t sdk_offsets[] = {"]
    for start in range(0, len(offsets), 12):
        lines.append("    " + ", ".join(map(str, offsets[start:start + 12])) + ",")
    return "\n".join(lines + ["};", "static const solar_os_cc_embedded_header_t sdk[] = {", *files, "};", ""])


def pack_standard_runtime(source):
    data = source.read_bytes()
    packed = compress_block(data)
    if not (0 < len(packed) < len(data) < 2**31):
        raise ValueError('C runtime source must fit in a smaller LZ4 block')
    lines = ['/* Generated from sdk_runtime.c. */',
             f'#define SOLAR_OS_CC_STANDARD_SOURCE_BYTES {len(data)}',
             'static const uint8_t standard_runtime[] = {']
    for offset in range(0, len(packed), 16):
        lines.append('    ' + ', '.join(f'0x{byte:02x}' for byte in packed[offset:offset+16]) + ',')
    return '\n'.join(lines + ['};', ''])


def use_packed_standard_runtime(source):
    compiler = source / 'libtcc.c'
    replace(compiler,
        'static int tcc_compile(TCCState *s1, int filetype, const char *str, int fd)',
        'static int tcc_compile(TCCState *s1, int filetype, const char *str, int fd, '
        'const solar_os_cc_runtime_source_t *input)')
    replace(compiler,
        '            int len = strlen(str);\n'
        '            tcc_open_bf(s1, solar_os_cc_source_name(), len);\n'
        '            memcpy(file->buffer, str, len);',
        '            int len = input ? input->bytes : strlen(str);\n'
        '            tcc_open_bf(s1, solar_os_cc_source_name(), len);\n'
        '            if (input) {\n'
        '                if (!input->read(file->buffer, len)) {\n'
        '                    _tcc_error_noabort("could not unpack standard runtime");\n'
        '                    longjmp(s1->error_jmp_buf, 1);\n'
        '                }\n'
        '            } else {\n'
        '                memcpy(file->buffer, str, len);\n'
        '            }')
    replace(compiler, 'return tcc_compile(s, s->filetype, str, -1);',
                      'return tcc_compile(s, s->filetype, str, -1, NULL);')
    replace(compiler, 'return tcc_compile(s1, flags, filename, fd);',
                      'return tcc_compile(s1, flags, filename, fd, NULL);')
    bridge = source / 'standard-bridge.c'
    replace(bridge, 'solar_os_cc_add_standard(TCCState *s1,const char *source,const char **entry)',
                    'solar_os_cc_add_standard(TCCState *s1,const char **entry)')
    replace(bridge, 'if(tcc_compile_string(s1,source)) return -1;',
                    'if(tcc_compile(s1,s1->filetype,NULL,-1,&solar_os_cc_standard_source)) return -1;')


def build_builtin(cc, prefix, output, destination, libgcc):
    destination.mkdir(parents=True, exist_ok=True)
    # Resolve reset-region boundaries while retaining ordinary firmware
    # sections and relocations. Final ESP-IDF linking assigns all addresses.
    layout = destination / 'core.ld'
    layout.write_text('''SECTIONS {
  .data.solar_os_tinycc_core : ALIGN(16) {
    __cc_data_start = .; *(.tcccore.data*) __cc_data_end = .;
  }
  .bss.solar_os_tinycc_core (NOLOAD) : ALIGN(16) {
    __cc_bss_start = .; *(.tcccore.bss*) __cc_bss_end = .;
  }
}
''')
    objects = []
    for name, inputs in (
        ('tinycc', ['core.o', 'runtime.builtin.o', 'module.o']),
        ('cc', ['client.o', 'sdk_cli.o', 'sdk_reader.o', 'project.o', 'validator.o']),
    ):
        raw = destination / (name + '.raw.o')
        isolated = destination / (name + '.isolated.o')
        local = destination / (name + '.o')
        run(cc, '-r', '-nostdlib', *(['-Wl,-T,' + str(layout)] if name == 'tinycc' else []),
            *[output / obj for obj in inputs], libgcc, '-o', raw)
        entry = isolate(prefix + 'nm', prefix + 'objcopy', raw, isolated, 'solar_os_' + name + '_')
        run(prefix + 'objcopy', '--keep-global-symbol=' + entry, isolated, local)
        objects.append(local)
    archive = destination / 'libsolar_os_cc.a'
    archive.unlink(missing_ok=True)
    run(prefix + 'ar', 'rcs', archive, *objects)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--builtin-output", type=Path, help="also build isolated firmware objects")
    args = parser.parse_args()
    source = prepare(args.archive)
    replace(source / "libtcc.c", 'tcc_open_bf(s1, "<string>", len);',
            'tcc_open_bf(s1, solar_os_cc_source_name(), len);')
    use_packed_standard_runtime(source)
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
    (source / "embedded-hook.h").write_text(
        '#include "solar_os_cc_runtime_source.h"\n'
        'void solar_os_cc_checkpoint(void);\nconst char *solar_os_cc_source_name(void);\n')
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
    # Embed the declarations as bounded read-only files. The public SDK keeps
    # the full contract comments alongside the same C declarations.
    headers = {name: embedded_header(path) for name, path in sdk_headers(source).items()}
    sdk = []
    for name, data in sorted(headers.items()):
        sdk.append("{%s, %s, %d}," % (json.dumps(name), json.dumps(data.decode()), len(data)))
    (output / "sdk.inc").write_text("\n".join(sdk) + "\n")
    version = tomllib.loads((ROOT / "modules/tinycc/module.toml").read_text())["version"]
    (output / "sdk_version.inc").write_text(json.dumps(version) + "\n")
    (output / "sdk_runtime.inc").write_text(pack_standard_runtime(HERE / "sdk_runtime.c"))
    for name in ("library_probe", "scalar_probe", "sdk_check", "sdk_aux"):
        (output / (name + ".inc")).write_text(json.dumps((HERE / "examples" / (name + ".c")).read_text()) + "\n")
    for name, path in (("runtime", HERE / "runtime.c"), ("module", HERE / "module.c"), ("client", HERE / "client.c"),
                       ("sdk_cli",HERE/"sdk_cli.c"),
                       ("project",HERE/"project.c"),
                       ("sdk_reader",HERE/"sdk_reader.c"),
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
    for name, objects in (("tinycc.library", ["core.o", "runtime.o", "module.o"]), ("cc.app", ["client.o", "sdk_cli.o", "sdk_reader.o", "project.o", "validator.o"])):
        run(cc, "-r", "-nostdlib", *[output / p for p in objects], output / "libgcc.a", "-o", output / (name + ".o"))
        namespace = "solar_os_tinycc_" if name == "tinycc.library" else "solar_os_cc_"
        entry = isolate(prefix + "nm", prefix + "objcopy", output / (name + ".o"),
                        output / (name + ".isolated.o"), namespace)
        run(prefix + "objcopy", "--keep-global-symbol=" + entry,
            output / (name + ".isolated.o"), output / (name + ".local.o"))
        run(cc, "-nostdlib", "-nostartfiles", "-shared", "-Wl,-Bsymbolic", "-Wl,--gc-sections",
            "-Wl,-e," + entry, "-Wl,-T," + str(output / "layout.ld"), output / (name + ".local.o"),
            "-o", output / (name + ".elf"))
        run(prefix + "strip", "--strip-unneeded", "--remove-section=.got.loc", "--remove-section=.dynamic",
            "--remove-section=.xt.lit", "--remove-section=.xt.prop", "--remove-section=.xtensa.info", output / (name + ".elf"))
    sys.path.insert(0, str(ROOT / "modules/espdl"))
    from verify import verify
    run("cc", "-std=c11", "-O2", "-include", "stdbool.h", "-I", ROOT / "tests/host",
        "-I", ROOT / "include", "-I", ROOT / "src/services",
        HERE / "elf_check.c", ROOT / "src/services/solar_os_native_elf.c", "-o", CACHE / "elf-check")
    report = {"version":version,"client_version":tomllib.loads((ROOT/"modules/cc/module.toml").read_text())["version"],
              "upstream": json.loads((HERE / "upstream.json").read_text())["revision"],
              "support":json.loads((CACHE/"support/build.json").read_text()),
              "gcc": subprocess.check_output([str(cc), "-dumpfullversion"], text=True).strip(),
              "sdk": {name: hashlib.sha256(data).hexdigest() for name, data in headers.items()},
              "sources": {os.path.relpath(path, HERE): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in [ROOT / "src/services/solar_os_native_elf.c",
                                       ROOT / "src/services/solar_os_native_elf.h"] + [HERE / name for name in ("runtime.c", "embedded.c", "client.c", "module.c",
                                                               "native_build.py", "isolate.py", "xtensa-gen.c", "xtensa-link.c",
                                                               "include/solar_os_cc_abi.h","support.c","support_build.py",
                                                               "support-bridge.c","standard-bridge.c","build.py","driver.c","sdk_reader.c","sdk_reader.h","sdk_cli.c","sdk_cli.h","project.c","project.h","sdk_runtime.c","sdk_packed.h")]
                          +[ROOT/"include/solar_os_native_sdk.h", ROOT/"scripts/pack_lz4.py",
                            ROOT/"scripts/generate_native_api.py",
                            ROOT/"protocol/display_link/include/sdlp_lz4_block.h",
                            HERE/"include/solar_os_cc_runtime_source.h"]}, "artifacts": {}}
    for name in ("tinycc.library.elf", "cc.app.elf"):
        path = output / name
        imports = verify(path, {"solar_os_native_library_host_v1", "solar_os_native_host_v1",
                                "memcpy", "memmove", "memset", "memcmp", "strlen", "setjmp", "longjmp"})
        run(CACHE / "elf-check", path)
        report["artifacts"][name] = {"bytes": path.stat().st_size, "imports": imports,
                                     "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
    if args.builtin_output:
        (output / 'sdk_packed.inc').write_text(pack_sdk_headers(headers))
        run(cc, *flags, '-DSOLAR_OS_CC_PACKED_SDK=1',
            '-c', HERE / 'runtime.c', '-o', output / 'runtime.builtin.o')
        build_builtin(cc, prefix, output, args.builtin_output.resolve(), libgcc)
    (output / "build.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["artifacts"], indent=2))


if __name__ == "__main__": main()
