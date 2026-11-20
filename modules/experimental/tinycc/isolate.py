"""Namespace a complete relocatable module before exposing its entry point."""
from pathlib import Path
import re
import subprocess
import tempfile


def defined_symbols(nm, path):
    output = subprocess.check_output(
        [str(nm), '-g', '--defined-only', '--format=posix', str(path)], text=True)
    return {line.split()[0] for line in output.splitlines() if line.strip()}


def imports(nm, path):
    output = subprocess.check_output(
        [str(nm), '-u', '--format=posix', str(path)], text=True)
    return {line.split()[0] for line in output.splitlines() if line.strip()}


def isolate(nm, objcopy, source, output, prefix, entry='app_main'):
    """Rename definitions and their relocations, leaving host imports intact.

    Run after partial linking: calls to the module's allocator, libc shims and
    compiler helpers must already resolve to their definitions in this object.
    Localize everything except the returned entry before final linking.
    """
    if not re.fullmatch(r'solar_os_[A-Za-z0-9_]+_', prefix):
        raise ValueError('module symbol prefix must start with solar_os_ and end with _')
    symbols = defined_symbols(nm, source)
    if entry not in symbols:
        raise ValueError(f'module entry {entry!r} is not defined')
    if any(not re.fullmatch(r'[A-Za-z_.$][A-Za-z0-9_.$]*', name) for name in symbols):
        raise ValueError('unsupported symbol name in relocatable module')
    before = imports(nm, source)
    with tempfile.TemporaryDirectory() as temporary:
        mapping = Path(temporary) / 'symbols.txt'
        mapping.write_text(''.join(f'{name} {prefix}{name}\n' for name in sorted(symbols)))
        subprocess.run([str(objcopy), '--redefine-syms=' + str(mapping),
                        str(source), str(output)], check=True)
    expected = {prefix + name for name in symbols}
    if defined_symbols(nm, output) != expected or imports(nm, output) != before:
        raise ValueError('module symbol isolation changed its host imports or missed a definition')
    return prefix + entry
