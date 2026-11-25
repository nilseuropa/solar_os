#!/usr/bin/env python3
"""Inventory OS service declarations and their public system ABI coverage.

Scope comes from OS packages, not application or language call sites.
Unclassified declarations remain pending. Optional packages are included.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from fnmatch import fnmatch
from pathlib import Path
import re
import sys
import tomllib
from generate_native_api import ROOT, COMMENTS, load, snapshot, tokens

PROTOTYPE = re.compile(r'^[ \t]*(?!typedef\b|static\b|return\b)([\w\s*]+?)\b(solar_os_\w+)\s*\(', re.M)

def declarations(text):
    text=COMMENTS.sub('',text)
    result={}
    for match in PROTOTYPE.finditer(text):
        prefix=match[1].strip()
        if not prefix or prefix.split()[0] in {'if','while','switch','return','static','typedef'}: continue
        depth,end=1,match.end()
        while end<len(text) and depth:
            depth+=(text[end]=='(')-(text[end]==')')
            end+=1
        tail=text[end:].lstrip()
        while not depth and tail.startswith('__attribute__'):
            start=tail.find('(')
            if start<0: break
            nested,position=1,start+1
            while position<len(tail) and nested:
                nested+=(tail[position]=='(')-(tail[position]==')');position+=1
            if nested: break
            tail=tail[position:].lstrip()
        if not depth and tail.startswith(';'):
            result[match[2]]=' '.join(text[match.start():end].split())
    return result

def interface_revision(root, relative, cache):
    """Track declarations, records and constants, including local type headers.

    Ignore build configuration and external SDK implementation headers. These
    revisions require coverage review when an existing OS record changes even
    if no new function is declared. They are not a public binary fingerprint.
    """
    pending=[relative];seen={};configuration={'solar_os_config.h','solar_os_packages.h'}
    while pending:
        name=pending.pop()
        if name in seen: continue
        if name not in cache:
            source=(root/name).read_text();dependencies=[]
            for include in re.findall(r'^\s*#\s*include\s+"(solar_os_[^"/]+\.h)"',source,re.M):
                if include in configuration: continue
                for path in ((root/name).parent/include, root/'include'/include,
                             root/'src'/include, root/'src/services'/include):
                    if path.is_file():
                        dependencies.append(path.relative_to(root).as_posix());break
            digest=hashlib.sha256(' '.join(tokens(source)).encode()).hexdigest()
            cache[name]=(digest,dependencies)
        digest,dependencies=cache[name];seen[name]=digest;pending.extend(dependencies)
    return hashlib.sha256(json.dumps(seen,sort_keys=True).encode()).hexdigest()

def inventory(root):
    metadata=tomllib.loads((root/'api/native/system.toml').read_text())
    packages=tomllib.loads((root/'packages/solar_os_packages.toml').read_text())['packages']
    headers={}
    for package,spec in packages.items():
        scope=metadata['scope']
        if package in scope['exclude_packages'] or not any(fnmatch(package,p) for p in scope['packages']): continue
        for source in spec.get('sources',[]):
            for path in (root/'src').glob(source):
                header=path.with_suffix('.h')
                relative=header.relative_to(root).as_posix()
                if not header.is_file() or any(fnmatch(relative,p) for p in scope['exclude_paths']): continue
                headers.setdefault(relative,set()).add(package)
    for relative in metadata['scope'].get('extra_headers',[]):
        if not (root/relative).is_file(): raise ValueError('missing OS interface: '+relative)
        headers.setdefault(relative,set()).add('system')
    public,services=load(root)
    types=snapshot(public,services)['types']
    operations=set()
    for service in services:
        for field in types[service['type']]['table']:
            match=re.search(r'\( \* (\w+) \)',field)
            if match: operations.add(service['id']+'.'+match[1])
    entries={};revisions={}
    for relative,owners in sorted(headers.items()):
        for name,signature in declarations((root/relative).read_text()).items():
            entry=entries.setdefault(name,{'headers':[],'packages':set(),'signature':signature,'revisions':{}})
            entry['headers'].append(relative)
            entry['packages'].update(owners)
            entry['revisions'][relative]=interface_revision(root,relative,revisions)
    for name,targets in metadata.get('operation',{}).items():
        if name not in entries: raise ValueError('coverage refers to unknown OS declaration: '+name)
        if not targets or any(target not in operations for target in targets):
            raise ValueError('coverage refers to unknown ABI operation: '+name)
    for name,entry in entries.items():
        entry['operations']=metadata.get('operation',{}).get(name,[])
        entry['packages']=sorted(entry['packages'])
        entry['review_revision']=hashlib.sha256(json.dumps(
            [entry['signature'],entry['revisions']],sort_keys=True).encode()).hexdigest()
        entry['internal_reason']=''
    for name,review in metadata.get('internal',{}).items():
        if name not in entries: raise ValueError('internal review refers to unknown OS declaration: '+name)
        entry=entries[name]
        if entry['operations']: raise ValueError('OS declaration cannot be both mapped and internal: '+name)
        if not isinstance(review,dict) or set(review)!={'reason','review'} or \
            not isinstance(review['reason'],str) or not review['reason'].strip():
            raise ValueError('internal review needs a reason and revision: '+name)
        if review['review']!=entry['review_revision']:
            raise ValueError('internal review changed; review the OS interface again: '+name)
        entry['internal_reason']=review['reason'].strip()
    return entries

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--check', action='store_true', help='Verify the recorded interface inventory')
    action.add_argument('--update-lock', action='store_true', help='Record the reviewed interface inventory')
    parser.add_argument('--require-full', action='store_true', help='Fail if any declaration is unclassified')
    parser.add_argument('--report', type=Path, help='Write the detailed inventory as JSON')
    args = parser.parse_args(argv)
    entries = inventory(args.root)
    covered = sum(bool(entry['operations']) for entry in entries.values())
    internal = sum(bool(entry['internal_reason']) for entry in entries.values())
    pending = len(entries) - covered - internal
    print(f'system ABI: {covered}/{len(entries)} OS declarations mapped; {internal} reviewed internal; {pending} pending')

    if args.report:
        args.report.write_text(json.dumps(entries, indent=2, sort_keys=True) + '\n')
    if args.require_full and pending:
        raise ValueError('system ABI coverage is incomplete')

    if args.check or args.update_lock:
        lock = args.root / 'api/native/coverage-lock.json'
        digest = hashlib.sha256(json.dumps(entries, sort_keys=True).encode()).hexdigest()
        current = {'schema': 1, 'sha256': digest}
        if args.check:
            if not lock.is_file() or json.loads(lock.read_text()) != current:
                raise ValueError('system ABI coverage changed; review and update the coverage lock')
        else:
            lock.write_text(json.dumps(current, indent=2) + '\n')

if __name__=='__main__':
    try: main()
    except ValueError as error:
        print(error,file=sys.stderr);sys.exit(1)
