#!/usr/bin/env python3
"""Build the standalone ESP-DL provider with a private S3 SDK build.

The SDK uses the normal SolarOS dependency overlays and a generated private
flavor. Private archive copies are used; only the library host and five standard
C helpers are imports.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from fix_frames import fix
from verify import verify, remove_noop_relocations


def prepare_sdk(environment):
    """Configure separately from firmware builds and build only SDK archives."""
    sdk_root = ROOT / '.pio/espdl-sdk'
    sdk_root.mkdir(parents=True, exist_ok=True)
    flavor, count = re.subn(r'^inference_backend = false$', 'inference_backend = true',
                           (ROOT / 'flavors/full.toml').read_text(), flags=re.M)
    if count != 1:
        raise ValueError('Expected full flavor to select downloadable inference')
    flavor_file = sdk_root / 'full.toml'
    if not flavor_file.exists() or flavor_file.read_text() != flavor:
        flavor_file.write_text(flavor)
    build_root = sdk_root / 'build'
    cached = build_root / environment
    # Do not inherit the caller's firmware flavor, board, or build directory.
    build_env = {key: value for key, value in os.environ.items()
                 if not key.startswith('SOLAR_OS_')}
    build_env.update(PLATFORMIO_BUILD_DIR=str(build_root), SOLAR_OS_FLAVOR='full',
                     SOLAR_OS_FLAVOR_FILE=str(flavor_file), SOLAR_OS_LAYOUT='ota')
    core = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
    pio = os.environ.get('SOLAROS_PIO') or str(core / 'penv/bin/pio')
    if not Path(pio).is_file():
        pio = shutil.which(pio) or shutil.which('pio')
    if not pio:
        raise ValueError('PlatformIO not found; set SOLAROS_PIO')
    print(f'ESP-DL: configuring SDK for {environment} in {cached}', flush=True)
    # envdump runs framework configuration without compiling firmware or writing
    # a second compilation database into the checkout root.
    log = sdk_root / 'configure.log'
    with log.open('w') as stream:
        result = subprocess.run([pio, 'run', '--project-dir', str(ROOT), '-e', environment,
                                 '-t', 'envdump'], env=build_env, stdout=stream,
                                stderr=subprocess.STDOUT)
    if result.returncode:
        raise ValueError(f'SDK configuration failed; see {log}\n{log.read_text()}')
    # Configuration itself takes the same lock in the PlatformIO subprocess.
    # Hold it for archive compilation and all subsequent provider compilation.
    sys.path.insert(0, str(ROOT / 'scripts'))
    from solaros_build_lock import acquire_project_build_lock
    acquire_project_build_lock(ROOT, environment)
    cache = (cached / 'CMakeCache.txt').read_text()
    match = re.search(r'^CMAKE_COMMAND:INTERNAL=(.+)$', cache, re.M)
    if match is None:
        raise ValueError(f'CMake executable missing from {cached / "CMakeCache.txt"}')
    cmake = match.group(1)
    print('ESP-DL: building SDK archives', flush=True)
    subprocess.run([cmake, '--build', str(cached), '--target',
                    '__idf_espressif__esp-dl', '__idf_espdl_schema'],
                   env=build_env, check=True)
    return cached


def load_sdk_commands(cached):
    required = ('compile_commands.json',
                'esp-idf/espressif__esp-dl/libespressif__esp-dl.a',
                'esp-idf/espdl_schema/libespdl_schema.a')
    missing = [name for name in required if not (cached / name).is_file()]
    if missing:
        raise ValueError(f'Incomplete ESP-DL SDK build at {cached}: missing {", ".join(missing)}. '
                         'Omit --sdk-build-dir to prepare the SDK automatically.')
    commands = json.loads((cached / 'compile_commands.json').read_text())
    adapter = next((c for name in ('solar_os_inference_espdl.cpp', 'dl_model_base.cpp')
                    for c in commands if c['file'].endswith(name)), None)
    if adapter is None:
        raise ValueError(f'No ESP-DL compile commands in {cached}; enable inference_backend '
                         'or omit --sdk-build-dir to prepare the SDK automatically.')
    return commands, adapter


def build(environment, output, sdk_build_dir=None):
    output.mkdir(parents=True, exist_ok=True)
    cached = sdk_build_dir if sdk_build_dir is not None else prepare_sdk(environment)
    commands, adapter = load_sdk_commands(cached)
    compiler = shlex.split(adapter['command'])[0]
    prefix = compiler[:-3]  # g++
    if 'xtensa-esp32s3-elf-g++' not in compiler:
        raise ValueError('This prototype requires an ESP32-S3 build')
    version=subprocess.check_output([compiler,'-dumpfullversion'],text=True).strip()
    if version!='14.2.0':raise ValueError('Review the private C++ runtime for compiler '+version)

    def run(tool, *arguments, **kwargs):
        return subprocess.run([prefix + tool, *map(str, arguments)], check=True, **kwargs)

    def compile_cpp(source, target, template=None, extra=()):
        entry = next(c for c in commands if c['file'].endswith(template)) if template else adapter
        args = shlex.split(entry['command'])
        args[args.index('-o') + 1] = str(output / target)
        args[-1] = str(source)
        args += ['-fPIC', '-fvisibility=hidden', '-DSOLAR_OS_INFERENCE_EXTERNAL=1',
                 '-I'+str(ROOT/'include'), '-I'+str(ROOT/'src/services'),
                 '-I'+str(ROOT/'src'), '-I'+str(cached/'generated/solar_os'),
                 '-I'+str(ROOT/'components/espdl_schema/include'), *extra]
        subprocess.run(args, cwd=entry['directory'], check=True)
        return output / target

    def compile_c(source, target, template=adapter):
        args = shlex.split(template['command'])
        args[0] = prefix + 'gcc'
        args = [a for a in args if not a.startswith('-std=') and a not in ('-fexceptions','-fno-rtti','-fuse-cxa-atexit') and not a.endswith('/toolchain/cxxflags')]
        args[args.index('-o') + 1] = str(output / target)
        args[-1] = str(source)
        args += ['-std=gnu11', '-fPIC', '-fvisibility=hidden',
                 '-I'+str(ROOT/'include'), '-I'+str(ROOT/'src/services'), '-I'+str(ROOT/'src')]
        subprocess.run(args, cwd=template['directory'], check=True)
        return output / target

    def private_archive(source, name):
        target = output / name
        run('objcopy', '--set-section-flags', '.rodata*=alloc,load,data,contents',
            '--set-section-flags', '.gcc_except_table*=alloc,load,data,contents',
            '--set-section-flags', '.eh_frame*=alloc,load,data,contents', source, target)
        return target

    engine = private_archive(cached/'esp-idf/espressif__esp-dl/libespressif__esp-dl.a', 'engine.a')
    fbs = private_archive(ROOT/'managed_components/espressif__esp-dl/fbs_loader/lib/esp32s3/libfbs_model.a', 'fbs.a')
    schema = private_archive(cached/'esp-idf/espdl_schema/libespdl_schema.a', 'schema.a')
    archives = [engine, fbs, schema]
    for name in ('libstdc++.a', 'libgcc.a'):
        source = subprocess.check_output([compiler, '-print-file-name='+name], text=True).strip()
        archives.append(private_archive(source, name))

    members = subprocess.check_output([prefix+'ar','t',str(engine)],text=True).splitlines()
    def member(source):
        matches=[name for name in members if name in (source+'.o',source+'.obj')]
        if len(matches)!=1:raise ValueError('Expected one archive member for '+source)
        return matches[0]
    base_source = ROOT/'managed_components/espressif__esp-dl/dl/module/src/dl_module_base.cpp'
    text = base_source.read_text()
    marker = '#include "solar_os_dl_workers.inc"'
    if text.count(marker) != 1:
        raise ValueError('ESP-DL worker overlay changed; review the prototype')
    (output/'base.cpp').write_text(text.replace(marker, (HERE/'workers.inc').read_text()))
    base = compile_cpp(output/'base.cpp', member('dl_module_base.cpp'), 'dl_module_base.cpp')
    run('ar', 'r', engine, base)
    # Replaced objects carry fresh read-only sections too.
    run('objcopy', '--set-section-flags', '.rodata*=alloc,load,data,contents',
        '--set-section-flags', '.gcc_except_table*=alloc,load,data,contents',
        '--set-section-flags', '.eh_frame*=alloc,load,data,contents', engine)
    objects = [compile_cpp(ROOT/'src/services/solar_os_inference_espdl.cpp', 'adapter.o'),
               compile_cpp(HERE/'library.cpp', 'library.o'),
               compile_cpp(ROOT/'src/services/solar_os_inference_espdl_api.cpp', 'api.o'),
               compile_c(HERE/'port.c', 'port.o'), compile_c(HERE/'dtype.c', 'dtype.o')]
    for name in ('anchor', 'weak'):
        run('gcc', '-c', HERE/(name+'.S'), '-o', output/(name+'.o'))
        objects.append(output/(name+'.o'))
    # BFD 2.43.1 aborts on final shared --gc-sections with C++ exception data.
    # Bind private definitions before the shared link; retain archive contents.
    run('g++', '-r', '-nostdlib', *objects, '-Wl,--start-group', *archives,
        '-Wl,--end-group', '-o', output/'bundle.o')
    # Upstream's max-pool assembly has an orphan .type declaration, with no
    # definition or relocation. objcopy refuses stripping a referenced symbol.
    run('objcopy', '--strip-symbol=dl_tie728_s8_max_pool2d', output/'bundle.o')
    run('objcopy', '--keep-global-symbol=app_main', output/'bundle.o', output/'local.o')
    run('g++', '-nostartfiles', '-nostdlib', '-shared', '-Wl,-Bsymbolic',
        '-Wl,-T,'+str(HERE/'layout.ld'), '-Wl,-e,app_main', output/'local.o',
        '-o', output/'espdl.debug.elf')
    artifact = output/'espdl.library.elf'
    shutil.copyfile(output/'espdl.debug.elf', artifact)
    remove_noop_relocations(artifact)
    fix(artifact)
    expected = {'solar_os_native_library_host_v1','memcpy','memmove','memset','memcmp','strlen'}
    imports=verify(artifact,expected)
    run('strip', '--strip-unneeded', '--remove-section=.got.loc', '--remove-section=.dynamic',
        '--remove-section=.xt.lit', '--remove-section=.xt.prop', '--remove-section=.xtensa.info', artifact)
    verify(artifact, expected)
    report = {'environment':environment,'artifact_bytes':artifact.stat().st_size,
              'imports':imports,
              'profile':'Full SDK operator registry, single/auto/dual, in-memory EDL1/EDL2'}
    fixtures=[]
    for short,name in [('int8','arithmetic_int8'),('float32','arithmetic_float32'),
                       ('simd','arithmetic_simd_int8'),('oversized','arithmetic_oversized')]:
        data=(ROOT/'tests/fixtures/inference'/(name+'.espdl')).read_bytes()
        fixtures.append('static const unsigned char fixture_'+short+'[]={'+','.join(map(str,data))+'};')
    (output/'fixtures.h').write_text('\n'.join(fixtures)+'\n')
    run('gcc','-Os','-mlongcalls','-fPIC','-fvisibility=hidden','-I'+str(ROOT/'include'),
        '-I'+str(output),'-c',HERE/'probe.c','-o',output/'probe.o')
    run('g++','-r','-nostdlib',output/'probe.o',output/'anchor.o',
        '-Wl,--start-group',output/'libgcc.a','-Wl,--end-group','-o',output/'probe-bundle.o')
    run('objcopy','--keep-global-symbol=app_main',output/'probe-bundle.o',output/'probe-local.o')
    run('g++','-nostartfiles','-nostdlib','-shared','-Wl,-Bsymbolic',
        '-Wl,-T,'+str(HERE/'layout.ld'),'-Wl,-e,app_main',output/'probe-local.o',
        '-o',output/'espdl-check.app.elf')
    remove_noop_relocations(output/'espdl-check.app.elf')
    verify(output/'espdl-check.app.elf',{'solar_os_native_host_v1','memcpy','memset','memcmp','strlen'})
    report['sha256']={name:hashlib.sha256((output/name).read_bytes()).hexdigest()
                      for name in ('espdl.library.elf','espdl-check.app.elf')}
    run('gcc','-Os','-mlongcalls','-fPIC','-fvisibility=hidden','-I'+str(ROOT/'include'),
        '-c',HERE/'vision_check.c','-o',output/'vision.o')
    run('g++','-r','-nostdlib',output/'vision.o',output/'anchor.o',
        '-Wl,--start-group',output/'libgcc.a','-Wl,--end-group','-o',output/'vision-bundle.o')
    run('objcopy','--keep-global-symbol=app_main',output/'vision-bundle.o',output/'vision-local.o')
    run('g++','-nostartfiles','-nostdlib','-shared','-Wl,-Bsymbolic',
        '-Wl,-T,'+str(HERE/'layout.ld'),'-Wl,-e,app_main',output/'vision-local.o',
        '-o',output/'espdl-vision-check.app.elf')
    remove_noop_relocations(output/'espdl-vision-check.app.elf')
    verify(output/'espdl-vision-check.app.elf',{'solar_os_native_host_v1','memcpy','memset','strlen'})
    report['sha256']['espdl-vision-check.app.elf']=hashlib.sha256((output/'espdl-vision-check.app.elf').read_bytes()).hexdigest()
    (output/'build.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    return prefix


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--environment', default='freenove_esp32_s3_display_4_0')
    parser.add_argument('--firmware-project', type=Path, default=ROOT)
    parser.add_argument('--sdk-build-dir', type=Path,
                        help='Reuse a completed SDK build with inference_backend enabled')
    parser.add_argument('--output', type=Path, default=ROOT/'.pio/modules/espdl')
    args = parser.parse_args()
    ROOT = args.firmware_project.resolve()
    try:
        build(args.environment, args.output.resolve(),
              args.sdk_build_dir.resolve() if args.sdk_build_dir else None)
    except (OSError, ValueError) as error:
        raise SystemExit(f'ESP-DL build failed: {error}') from None
    except subprocess.CalledProcessError as error:
        raise SystemExit(f'ESP-DL build failed: {Path(error.cmd[0]).name} exited {error.returncode}') from None
