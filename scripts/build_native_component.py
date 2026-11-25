#!/usr/bin/env python3
"""Build one SDK-only component as a firmware archive and/or portable ELF.

Both forms use the same objects. No firmware configuration or private headers
are inputs. The manifest owns the source list, entry and component identity.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import tempfile
import tomllib

ROOT = Path(__file__).resolve().parents[1]
HOST_IMPORTS = frozenset({
    'solar_os_native_host_v1', 'solar_os_native_job_host_v1',
    'solar_os_native_driver_host_v1', 'solar_os_native_library_host_v1',
    'memcpy', 'memmove', 'memset', 'memcmp', 'strlen', 'setjmp', 'longjmp',
})
IDENTIFIER = re.compile(r'[A-Za-z_][A-Za-z0-9_]*\Z')


@dataclass(frozen=True)
class BuildSpec:
    manifest: Path
    identity: str
    target: str
    artifact: str
    entry: str
    sources: tuple[Path, ...]
    archives: tuple[Path, ...]
    includes: tuple[Path, ...]
    defines: tuple[str, ...]
    exports: tuple[tuple[str, str], ...]


def local_path(root, name, directory=False):
    if not isinstance(name, str) or not name or Path(name).is_absolute():
        raise ValueError('build inputs must be relative to the module')
    path = (root / name).resolve()
    if not path.is_relative_to(root):
        raise ValueError('build input escapes module directory')
    if not (path.is_dir() if directory else path.is_file()):
        raise ValueError('missing build input: ' + name)
    return path


def load_spec(manifest):
    manifest = Path(manifest).resolve()
    meta = tomllib.loads(manifest.read_text())
    identity = meta.get('id', '')
    if not isinstance(identity, str) or not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,30}', identity):
        raise ValueError('invalid component id')
    if meta.get('native_abi') != 1 or meta.get('type') not in {'app', 'job', 'driver', 'library'}:
        raise ValueError('unsupported component native ABI or type')
    lifecycle = meta.get('lifecycle_abi')
    if lifecycle != 1 and not (lifecycle == 2 and meta['type'] == 'app'):
        raise ValueError('unsupported component lifecycle ABI')
    target = meta.get('target')
    if target not in {'esp32', 'esp32s3'}:
        raise ValueError('unsupported component target')
    artifact = meta.get('artifact', '')
    if not isinstance(artifact, str) or not re.fullmatch(r'[A-Za-z0-9_.-]+\.elf', artifact):
        raise ValueError('artifact must be an ELF filename')
    build = meta.get('build')
    if not isinstance(build, dict) or set(build) - {'sources', 'archives', 'include_dirs', 'defines', 'entry'}:
        raise ValueError('invalid or missing [build] table')
    entry = build.get('entry')
    if not isinstance(entry, str) or not IDENTIFIER.fullmatch(entry):
        raise ValueError('[build].entry must name a C symbol')
    root = manifest.parent
    paths = {}
    for key in ('sources', 'archives', 'include_dirs'):
        names = build.get(key, [])
        if not isinstance(names, list) or len(names) > 256:
            raise ValueError('build ' + key + ' must be a list of at most 256 inputs')
        paths[key] = tuple(local_path(root, name, key == 'include_dirs') for name in names)
        if len(set(paths[key])) != len(names):
            raise ValueError('duplicate build input in ' + key)
    if not paths['sources'] and not paths['archives']:
        raise ValueError('build requires sources or archives')
    if any(path.suffix not in {'.c', '.cc', '.cpp'} for path in paths['sources']):
        raise ValueError('component sources must be C or C++')
    if any(path.suffix != '.a' for path in paths['archives']):
        raise ValueError('component archives must end in .a')
    defines = build.get('defines', {})
    if not isinstance(defines, dict) or any(not IDENTIFIER.fullmatch(key) or
            not isinstance(value, (str, int)) or isinstance(value, bool) or
            '\0' in str(value) or '\n' in str(value) for key, value in defines.items()):
        raise ValueError('build defines must map C identifiers to integer or string values')
    prefix = 'solar_os_component_' + identity.encode().hex() + '_'
    builtin = meta.get('builtin', {})
    output_entry = builtin.get('entry', prefix + entry)
    exports = [(entry, output_entry)]
    if lifecycle == 2:
        exports.append(('solar_os_native_app_manifest', builtin.get('manifest', prefix + 'manifest')))
    if any(not isinstance(name, str) or not re.fullmatch(r'solar_os_[A-Za-z0-9_]+', name)
           for _, name in exports) or len({name for _, name in exports}) != len(exports):
        raise ValueError('invalid or duplicate built-in export')
    return BuildSpec(manifest, identity, target, artifact, entry, paths['sources'], paths['archives'],
                     paths['include_dirs'], tuple(key + '=' + str(value) for key, value in sorted(defines.items())),
                     tuple(exports))


def sdk_headers():
    headers = {}
    for pattern in ('solar_os_native*abi.h', 'solar_os_native_client*.h', 'solar_os_*_types_v1.h',
                    'solar_os_key_codes.h'):
        headers.update((path.name, path) for path in (ROOT / 'include').glob(pattern))
    return headers


def inputs(spec):
    # Track local included headers as well as explicit object/archive inputs.
    return tuple(sorted({spec.manifest, Path(__file__).resolve(),
        ROOT / 'scripts/pack_native_sdk.py', ROOT / 'modules/espdl/verify.py',
        ROOT / 'modules/espdl/layout.ld', *spec.sources, *spec.archives,
        ROOT / 'scripts/native_component_check.c', ROOT / 'src/services/solar_os_native_elf.c',
        ROOT / 'src/services/solar_os_native_elf.h', ROOT / 'src/services/solar_os_native_app_manifest.h',
        *sdk_headers().values(),
        *[p for p in spec.manifest.parent.rglob('*') if p.is_file() and p.suffix in {'.h', '.hpp', '.inc'}]}))


def support_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def symbols(nm, path, defined=False):
    flags = ['-g', '--defined-only'] if defined else ['-u']
    output = subprocess.check_output([str(nm), *flags, '--format=posix', str(path)], text=True)
    return {line.split()[0] for line in output.splitlines() if line.strip()}


def build(spec, output, cc, *, cxx=None, builtin=True, external=True):
    cc = Path(shutil.which(str(cc)) or cc).resolve()
    if not cc.name.endswith('gcc'):
        raise ValueError('use the Xtensa GCC toolchain for the component target')
    prefix = str(cc)[:-3]
    machine = subprocess.check_output([str(cc), '-dumpmachine'], text=True).strip()
    if not machine.startswith('xtensa-'):
        raise ValueError('component requires an Xtensa compiler')
    # A firmware archive must not accidentally contain code for another MCU.
    if machine not in {'xtensa-esp-elf', 'xtensa-' + spec.target + '-elf'}:
        raise ValueError('compiler target does not match module target')
    if external and spec.target != 'esp32s3':
        raise ValueError('external ELF deployment requires ESP32-S3; use builtin mode for ESP32')
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if output == spec.manifest.parent or any(path.is_relative_to(output) for path in (*spec.sources, *spec.archives)):
        raise ValueError('output directory contains build inputs')
    exports = dict(spec.exports)
    report = {'id': spec.identity, 'target': spec.target, 'entry': exports[spec.entry],
              'compiler': subprocess.check_output([str(cc), '-dumpfullversion'], text=True).strip(),
              'inputs': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs(spec)},
              'artifacts': {}}
    with tempfile.TemporaryDirectory(prefix='.component-', dir=output) as temporary:
        stage = Path(temporary)
        sdk = stage / 'sdk'; sdk.mkdir()
        for name, path in sdk_headers().items():
            shutil.copyfile(path, sdk / name)
        flags = ['-Os', '-g0', '-fPIC', '-fvisibility=hidden', '-ffunction-sections', '-fdata-sections',
                 '-fno-common', '-fno-builtin', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables',
                 '-I' + str(sdk), *['-I' + str(path) for path in spec.includes],
                 *['-D' + definition for definition in spec.defines]]
        objects = []
        for index, source in enumerate(spec.sources):
            obj = stage / (str(index) + '.o')
            dependencies = stage / (str(index) + '.d')
            cpp = source.suffix != '.c'
            compiler = (cxx or prefix + 'g++') if cpp else cc
            language = ['-std=gnu++17', '-fno-exceptions', '-fno-rtti'] if cpp else ['-std=gnu11']
            run(compiler, *flags, *language, '-MD', '-MF', dependencies, '-c', source, '-o', obj)
            # Check the actual include graph, including quoted relative paths.
            # Merely omitting firmware -I flags would still permit ../../src.
            dependency_text = dependencies.read_text().replace('\\\n', '')
            for name in shlex.split(dependency_text.split(':', 1)[1]):
                path = Path(name).resolve()
                if not any(path.is_relative_to(root) for root in (sdk, spec.manifest.parent, cc.parent.parent)):
                    raise ValueError('component includes a non-SDK header outside its module: ' + str(path))
            objects.append(obj)
        libgcc = subprocess.check_output([str(cc), '-print-libgcc-file-name'], text=True).strip()
        raw = stage / 'component.raw.o'
        run(cc, '-r', '-nostdlib', '-Wl,-u,' + spec.entry, '-Wl,--start-group', *objects,
            *spec.archives, libgcc, '-Wl,--end-group', '-o', raw)
        sections = subprocess.check_output([prefix + 'objdump', '-h', str(raw)], text=True)
        for line in sections.splitlines():
            fields = line.split()
            if len(fields) >= 3 and fields[1].startswith(('.init_array', '.fini_array', '.ctors', '.dtors')) and int(fields[2], 16):
                raise ValueError('component requires an explicit constructor/destructor lifecycle')
        definitions = symbols(prefix + 'nm', raw, True)
        if not set(exports) <= definitions:
            raise ValueError('missing component exports: ' + ', '.join(sorted(set(exports) - definitions)))
        imports = symbols(prefix + 'nm', raw)
        if imports - HOST_IMPORTS:
            raise ValueError('unclosed component imports: ' + ', '.join(sorted(imports - HOST_IMPORTS)))
        if not any(name.startswith('solar_os_native_') for name in imports):
            raise ValueError('component must import a versioned SolarOS host API')
        namespace = 'solar_os_component_' + spec.identity.encode().hex() + '_'
        if any(not re.fullmatch(r'[A-Za-z_.$][A-Za-z0-9_.$]*', name) for name in definitions):
            raise ValueError('unsupported component symbol')
        mapping = {name: exports.get(name, namespace + name) for name in definitions}
        if len(set(mapping.values())) != len(mapping):
            raise ValueError('component symbol namespace collision')
        names = stage / 'symbols.txt'
        names.write_text(''.join(name + ' ' + replacement + '\n' for name, replacement in sorted(mapping.items())))
        isolated = stage / 'component.isolated.o'
        run(prefix + 'objcopy', '--redefine-syms=' + str(names), raw, isolated)
        if symbols(prefix + 'nm', isolated) != imports:
            raise ValueError('component isolation changed imports')
        local = stage / 'component.o'
        run(prefix + 'objcopy', *['--keep-global-symbol=' + name for name in exports.values()], isolated, local)
        if symbols(prefix + 'nm', local, True) != set(exports.values()):
            raise ValueError('component isolation missed an export')
        if builtin:
            artifact = stage / ('libsolar_os_' + spec.identity + '.a')
            run(prefix + 'ar', 'rcsD', artifact, local)
            report['artifacts']['builtin'] = artifact.name
        if external:
            artifact = stage / spec.artifact
            # Firmware descriptors need globally unique names. ELF discovery
            # always uses the fixed lifecycle symbol, regardless of identity.
            external_object = local
            external_exports = list(exports.values())
            manifest_symbol = exports.get('solar_os_native_app_manifest')
            if manifest_symbol:
                external_object = stage / 'external.o'
                run(prefix + 'objcopy', '--redefine-sym=' + manifest_symbol + '=solar_os_native_app_manifest',
                    local, external_object)
                external_exports.remove(manifest_symbol)
                external_exports.append('solar_os_native_app_manifest')
            run(cc, '-nostdlib', '-nostartfiles', '-shared', '-Wl,-Bsymbolic', '-Wl,--gc-sections',
                '-Wl,-e,' + exports[spec.entry], '-Wl,-T,' + str(ROOT / 'modules/espdl/layout.ld'),
                *['-Wl,--export-dynamic-symbol=' + name for name in external_exports], external_object, '-o', artifact)
            # Preserve allocated sections: removing them with Xtensa strip can
            # leave stale dynamic-symbol section indices (including the app
            # manifest). The loader ignores unneeded metadata sections.
            run(prefix + 'strip', '--strip-unneeded', artifact)
            verify = support_module('component_elf_verify', ROOT / 'modules/espdl/verify.py')
            verify.remove_noop_relocations(artifact)
            report['imports'] = verify.verify(artifact, HOST_IMPORTS)
            if tomllib.loads(spec.manifest.read_text()).get('sdk', {}).get('headers'):
                from pack_native_sdk import pack_elf
                artifact.write_bytes(pack_elf(artifact.read_bytes(), spec.manifest))
            checker = stage / 'elf-check'
            run('cc', '-std=c11', '-O2', '-include', 'stdbool.h',
                '-I' + str(ROOT / 'tests/host'), '-I' + str(ROOT / 'include'),
                '-I' + str(ROOT / 'src/services'), ROOT / 'scripts/native_component_check.c',
                ROOT / 'src/services/solar_os_native_elf.c', '-o', checker)
            run(checker, artifact, spec.identity, '2' if manifest_symbol else '1')
            report['artifacts']['external'] = artifact.name
        # Publish artifacts only after every requested deployment form validates.
        for name in report['artifacts'].values():
            (stage / name).replace(output / name)
        (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--cc', default=str(Path.home() / '.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc'))
    parser.add_argument('--cxx')
    parser.add_argument('--target', choices=('esp32', 'esp32s3'))
    parser.add_argument('--mode', choices=('both', 'builtin', 'external'), default='both')
    parser.add_argument('--list-inputs', action='store_true')
    parser.add_argument('--describe', action='store_true')
    parser.add_argument('--identity', action='store_true')
    args = parser.parse_args()
    spec = load_spec(args.manifest)
    if args.target and args.target != spec.target:
        parser.error('component target does not match the firmware target')
    if args.identity:
        print(spec.identity, end='')
        return
    if args.describe:
        print(json.dumps({'id': spec.identity, 'target': spec.target, 'entry': dict(spec.exports)[spec.entry]}))
        return
    if args.list_inputs:
        print(';'.join(str(path) for path in inputs(spec)), end='')
        return
    if not args.output:
        parser.error('--output is required')
    report = build(spec, args.output, args.cc, cxx=args.cxx,
                   builtin=args.mode != 'external', external=args.mode != 'builtin')
    print(json.dumps(report['artifacts'], indent=2))


if __name__ == '__main__':
    main()
