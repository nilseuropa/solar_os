"""Build the optional isolated compiler archive in PlatformIO's SCons graph."""
from pathlib import Path
import shlex
import sys

Import('env')


def configure_builtin_cc():
    project = Path(env.subst('$PROJECT_DIR'))
    build = Path(env.subst('$BUILD_DIR'))
    config = build / 'generated/solar_os/solar_os_config.h'
    # The standalone emulator has no SolarOS flavor header.
    enabled = env.GetProjectOption('custom_solaros_builtin_cc', '') == 'true'
    if not enabled and (not config.is_file() or
                        '#define SOLAR_OS_PACKAGE_APP_CC 1' not in config.read_text()):
        return
    root = Path(env.GetProjectOption('custom_solaros_firmware_project', str(project)))
    if not root.is_absolute(): root = project / root
    root = root.resolve()
    source = root / 'modules/experimental/tinycc'
    inputs = [path for pattern in ('*.c', '*.h', '*.py', '*.json') for path in source.glob(pattern)]
    for directory, pattern in (('include', '*.h'), ('sdk', '*.h'), ('examples', '*.c')):
        inputs.extend((source / directory).rglob(pattern))
    inputs.extend((root / 'include').glob('solar_os_native*.h'))
    inputs.extend((root / 'include').glob('solar_os_*_types_v1.h'))
    inputs.append(root / 'include/solar_os_key_codes.h')
    inputs.extend(root / path for path in (
        'modules/cc/module.toml', 'modules/tinycc/module.toml',
        'modules/examples/hello-library/main/hello_library_api.h',
        'src/services/solar_os_native_elf.c', 'src/services/solar_os_native_elf.h',
        'scripts/platformio_cc.py'))
    output = build / 'tinycc'
    archive = output / 'libsolar_os_cc.a'
    command = [sys.executable, str(source / 'native_build.py'), '--builtin-output', str(output)]
    node = env.Command(str(archive), [str(path) for path in inputs],
        env.VerboseAction(' '.join(shlex.quote(arg) for arg in command),
                          'Building isolated built-in C compiler'))
    env.Depends('$BUILD_DIR/${PROGNAME}.elf', node)
    env.Append(LIBS=[env.File(str(archive))])


configure_builtin_cc()
