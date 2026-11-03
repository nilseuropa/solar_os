"""Read interpreter sources with their local, per-domain binding includes.

Preserve include lines so registration/include-count assertions still inspect
the translation unit. The shared API descriptor is intentionally not expanded:
it is a macro input, not a per-runtime implementation.
"""
from pathlib import Path
import re


def script_source(path):
    path = Path(path)
    active = set()

    def read(source):
        if source in active:
            raise ValueError(f'cyclic binding include: {source}')
        active.add(source)
        text = source.read_text(encoding='utf-8')

        def expand(match):
            return match[0] + '\n' + read(source.parent / match[1])

        text = re.sub(r'^#include "(solar_os_(?:python|lua)_\w+\.inc)"$', expand, text, flags=re.M)
        active.remove(source)
        return text

    return read(path)
