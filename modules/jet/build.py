#!/usr/bin/env python3
"""Build the resident Jet provider with the validated private C++ runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def prepare(environment):
    build_root = ROOT / '.pio/jet-sdk/build'
    cached = build_root / environment
    env = {k: v for k, v in os.environ.items() if not k.startswith('SOLAR_OS_')}
    env.update(PLATFORMIO_BUILD_DIR=str(build_root), SOLAR_OS_FLAVOR='full',
               SOLAR_OS_FLAVOR_FILE=str(ROOT / 'flavors/full.toml'), SOLAR_OS_LAYOUT='ota')
    pio = os.environ.get('SOLAROS_PIO', str(Path.home() / '.platformio/penv/bin/pio'))
    log = ROOT / '.pio/jet-sdk/configure.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as stream:
        subprocess.run([pio, 'run', '--project-dir', str(ROOT), '-e', environment, '-t', 'envdump'],
                       env=env, stdout=stream, stderr=subprocess.STDOUT, check=True)
    return cached


def build(environment, output, cached=None):
    sys.path.insert(0, str(ROOT / 'modules/espdl'))
    from fix_frames import fix
    from verify import verify, remove_noop_relocations
    cached = cached or prepare(environment)
    sys.path.insert(0, str(ROOT / 'scripts'))
    from solaros_build_lock import acquire_project_build_lock
    acquire_project_build_lock(ROOT, environment)
    commands = json.loads((cached / 'compile_commands.json').read_text())
    template = next(c for c in commands if c['file'].endswith('.cpp'))
    base = shlex.split(template['command'])
    compiler = base[0]
    prefix = compiler[:-3]
    if 'xtensa-esp32s3-elf-g++' not in compiler:
        raise ValueError('Jet modules require an ESP32-S3 SDK')
    if subprocess.check_output([compiler, '-dumpfullversion'], text=True).strip() != '14.2.0':
        raise ValueError('Review the private C++ runtime for this compiler version')
    output.mkdir(parents=True, exist_ok=True)
    runtime = ROOT / 'modules/espdl'

    def run(tool, *args):
        subprocess.run([prefix + tool, *map(str, args)], check=True)

    def compile_source(source, target, c=False):
        args = list(base)
        args[args.index('-o') + 1] = str(output / target)
        args[-1] = str(source)
        args = [a for a in args if not a.startswith('-std=') and
                not a.endswith('/toolchain/cxxflags') and a not in ('-fexceptions', '-fno-rtti', '-fuse-cxa-atexit')]
        if c: args[0] = prefix + 'gcc'
        args += ['-std=gnu11' if c else '-std=c++17', '-O2', '-fPIC', '-fvisibility=hidden',
                 '-DJET_EXTERNAL_LIBRARY=1', '-I' + str(ROOT / 'include'),
                 '-I' + str(ROOT / 'src/services'), '-I' + str(ROOT / 'src'),
                 '-I' + str(HERE / 'main'), '-I' + str(HERE / 'main/vendor')]
        if not c: args += ['-fexceptions']
        subprocess.run(args, cwd=template['directory'], check=True)
        return output / target

    sources = sorted((HERE / 'main/vendor').glob('*.cpp'))
    objects = [compile_source(p, p.stem + '.o') for p in sources]
    objects += [compile_source(HERE / 'main/engine.cpp', 'engine.o'),
                compile_source(HERE / 'library.cpp', 'library.o'),
                compile_source(HERE / 'port.c', 'port.o', c=True)]
    for name in ('anchor', 'weak'):
        run('gcc', '-c', runtime / (name + '.S'), '-o', output / (name + '.o'))
        objects.append(output / (name + '.o'))
    archives = []
    for name in ('libstdc++.a', 'libm.a', 'libgcc.a'):
        source = subprocess.check_output([compiler, '-print-file-name=' + name], text=True).strip()
        target = output / name
        run('objcopy', '--set-section-flags', '.rodata*=alloc,load,data,contents',
            '--set-section-flags', '.gcc_except_table*=alloc,load,data,contents',
            '--set-section-flags', '.eh_frame*=alloc,load,data,contents', source, target)
        archives.append(target)
    run('g++', '-r', '-nostdlib', *objects, '-Wl,--start-group', *archives,
        '-Wl,--end-group', '-o', output / 'bundle.o')
    run('objcopy', '--keep-global-symbol=app_main', output / 'bundle.o', output / 'local.o')
    layout = (runtime / 'layout.ld').read_text()
    marker = '*(.solaros_cpp_anchor)'
    if layout.count(marker) != 1:
        raise ValueError('Review the private C++ linker layout')
    (output / 'layout.ld').write_text(layout.replace(marker, marker + ' *(.dram1 .dram1.*)'))
    run('g++', '-nostartfiles', '-nostdlib', '-shared', '-Wl,-Bsymbolic',
        '-Wl,-T,' + str(output / 'layout.ld'), '-Wl,-e,app_main', output / 'local.o',
        '-o', output / 'jet.debug.elf')
    artifact = output / 'jet.library.elf'
    shutil.copyfile(output / 'jet.debug.elf', artifact)
    remove_noop_relocations(artifact)
    fix(artifact)
    expected = {'solar_os_native_library_host_v1', 'memcpy', 'memmove', 'memset', 'memcmp', 'strlen'}
    imports = verify(artifact, expected)
    run('strip', '--strip-unneeded', '--remove-section=.got.loc', '--remove-section=.dynamic',
        '--remove-section=.xt.lit', '--remove-section=.xt.prop', '--remove-section=.xtensa.info', artifact)
    verify(artifact, expected)
    report = {'environment': environment, 'artifact_bytes': artifact.stat().st_size,
              'sha256': hashlib.sha256(artifact.read_bytes()).hexdigest(), 'imports': imports}
    (output / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--environment', default='freenove_esp32_s3_display_4_0')
    parser.add_argument('--firmware-project', type=Path, default=ROOT)
    parser.add_argument('--sdk-build-dir', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / '.pio/modules/jet')
    args = parser.parse_args()
    ROOT = args.firmware_project.resolve()
    build(args.environment, args.output.resolve(), args.sdk_build_dir.resolve() if args.sdk_build_dir else None)
