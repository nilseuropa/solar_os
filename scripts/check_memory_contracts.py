#!/usr/bin/env python3
"""Inventory every package's memory boundary and reject unaudited raw allocation.

This is a lexical policy check, not a proof of C control flow or OOM handling.
Vendor code retains SDK/library placement and is listed as a boundary.
"""
from __future__ import annotations
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import tomllib

ROOT = Path(__file__).resolve().parents[1]
TOKENS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
APIS = re.compile(
    r'\b(solar_os_memory_(?:alloc|calloc|realloc|buffer_create)|'
    r'solar_os_paged_array_reserve|solar_os_task_create_\w+|solar_os_queue_create\w*|'
    r'heap_caps_(?:malloc|calloc|realloc|aligned_alloc|aligned_calloc)|'
    r'malloc|calloc|realloc|xTaskCreate\w*|xQueueCreate\w*|'
    r'xSemaphoreCreate\w*|xEventGroupCreate\w*)\s*\(')
RAW = re.compile(r'^(?:heap_caps_|malloc$|calloc$|realloc$|xTaskCreate|xQueueCreate)')
CPP_NEW = re.compile(r'\bnew\b\s*(?:\(|[A-Za-z_])')
INCLUDE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
SUFFIXES = {'.c', '.cpp', '.cc', '.h', '.inc'}

def mask(source: str) -> str:
    return TOKENS.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), source)

def sites(path: Path) -> list[dict]:
    code = mask(path.read_text())
    result = []
    for match in APIS.finditer(code):
        api = match[1]
        if api == 'solar_os_memory_buffer_create':
            contract = 'movable-with-pin'
        elif api == 'solar_os_paged_array_reserve':
            contract = 'paged-pinned'
        elif api.startswith(('solar_os_task_', 'xTaskCreate')):
            contract = 'pinned-stack'
        elif api.startswith(('solar_os_queue_', 'xQueue', 'xSemaphore', 'xEventGroup')):
            contract = 'pinned-rtos'
        elif RAW.match(api):
            contract = 'pinned-sdk'
        else:
            contract = 'pinned-policy'
        result.append(dict(line=code.count('\n', 0, match.start()) + 1, api=api, contract=contract))
    for match in CPP_NEW.finditer(code):
        result.append(dict(line=code.count('\n', 0, match.start()) + 1,
                           api='new', contract='pinned-cpp'))
    return result

def source_paths(root: Path) -> list[Path]:
    paths = []
    for base in (root / 'src', root / 'modules'):
        for path in sorted(base.rglob('*')):
            relative = path.relative_to(base)
            if path.suffix not in SUFFIXES or not path.is_file(): continue
            if any(p in {'vendor', 'managed_components', 'build', '.pio'} for p in relative.parts): continue
            if base.name == 'modules' and 'main' not in relative.parts: continue
            paths.append(path)
    return paths

def inventory(root: Path) -> tuple[dict, list[str]]:
    exceptions = json.loads((root / 'packages/memory_contracts.json').read_text())
    catalog = tomllib.loads((root / 'packages/solar_os_packages.toml').read_text())
    files, errors = {}, []
    for path in source_paths(root):
        relative = path.relative_to(root).as_posix()
        allocations = sites(path)
        files[relative] = dict(sites=allocations, default='pinned')
        raw = Counter(s['api'] for s in allocations if RAW.match(s['api']) or s['api'] == 'new')
        approved = exceptions['exceptions'].get(relative, {})
        if dict(raw) != approved.get('calls', {}):
            errors.append(f'{relative}: raw allocation contract changed: {dict(raw)}; '
                          'use shared memory/task/queue APIs or audit the pinned exception')
        if approved and not approved.get('reason', '').strip():
            errors.append(f'{relative}: pinned exception needs a concrete reason')
    for path in exceptions['exceptions']:
        if path not in files: errors.append(f'{path}: stale pinned exception')
    packages = {}
    for name, package in catalog['packages'].items():
        owned = set()
        todo = [root / 'src' / p for p in package.get('sources', [])]
        while todo:
            path = todo.pop()
            relative = path.relative_to(root).as_posix()
            if relative in owned or relative not in files: continue
            owned.add(relative)
            for included in INCLUDE.findall(path.read_text()):
                local = path.parent / included
                if local.is_file() and local.resolve().is_relative_to(root):
                    todo.append(local.resolve())
        packages[name] = dict(default='pinned', files=sorted(owned),
                              allocation_sites=sum(len(files[p]['sites']) for p in owned))
    return dict(schema=1, packages=packages, files=files, exceptions=exceptions['exceptions'],
                boundaries=exceptions['boundaries']), errors

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, default=ROOT)
    parser.add_argument('--json', type=Path, help='write a complete package/site inventory')
    args = parser.parse_args()
    report, errors = inventory(args.repository.resolve())
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + '\n')
    for error in errors: print(error)
    print(f"Memory contracts: {len(report['packages'])} packages, "
          f"{len(report['files'])} source files, "
          f"{sum(len(f['sites']) for f in report['files'].values())} API sites; "
          f"{len(errors)} errors")
    return bool(errors)

if __name__ == '__main__':
    raise SystemExit(main())
