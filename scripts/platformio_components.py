"""Track SDK-only component archives in PlatformIO's link graph."""
from pathlib import Path
import re
import shlex
import sys

Import('env')


def configure_components():
    root = Path(env.subst('$PROJECT_DIR'))
    build = Path(env.subst('$BUILD_DIR'))
    config = build / 'generated/solar_os/solar_os_config.h'
    if not config.is_file():
        return
    sys.path.insert(0, str(root / 'scripts'))
    from solaros_components import load_builtin_components
    from build_native_component import load_spec, inputs
    # Only the resolved configuration decides which manifests become code.
    enabled = {name.lower(): value == '1' for name, value in re.findall(
        r'^#define SOLAR_OS_PACKAGE_(\w+) ([01])$', config.read_text(), re.M)}
    for component in load_builtin_components(root, enabled):
        if not component.build or not enabled[component.package]:
            continue
        spec = load_spec(component.build)
        output = build / 'native-components' / component.id
        archive = output / ('libsolar_os_' + component.id + '.a')
        command = [sys.executable, str(root / 'scripts/build_native_component.py'),
                   '--manifest', str(component.build), '--output', str(output), '--mode', 'builtin',
                   '--target', env.BoardConfig().get('build.mcu'),
                   '--cc', env.WhereIs(env.subst('$CC')),
                   '--cxx', env.WhereIs(env.subst('$CXX'))]
        node = env.Command(str(archive), [str(path) for path in inputs(spec)],
            env.VerboseAction(shlex.join(command), 'Building native component ' + component.id))
        env.Depends('$BUILD_DIR/${PROGNAME}.elf', node)
        env.Append(LIBS=[env.File(str(archive))])


configure_components()
