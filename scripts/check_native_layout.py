#!/usr/bin/env python3
"""Check public Xtensa C layouts with both firmware GCC and the OS C frontend.

The layout baseline describes the target C ABI, independently of board/flavor
configuration. Updates allow new types and append-only table tails; existing
record sizes, alignments and field offsets must remain compatible.
"""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import sys
from generate_native_api import ROOT, load, snapshot

BASELINE = Path('api/native/layout-xtensa.json')


def members(body):
    groups=[];start=depth=0
    for index,char in enumerate(body):
        depth+=(char in '({[')-(char in ')}]')
        if char==';' and not depth:
            groups.append(body[start:index]);start=index+1
    if body[start:].strip():groups.append(body[start:])
    result=[]
    for group in groups:
        callback=re.search(r'\(\s*\*\s*(\w+)\s*\)',group)
        if callback and '{' not in group:
            result.append(callback[1]);continue
        children=[]
        if '{' in group:
            children=members(group[group.index('{')+1:group.rindex('}')])
            group=group[group.rindex('}')+1:]
            if not group.strip():result.extend(children);continue
        start=depth=0;declarators=[]
        for index,char in enumerate(group):
            depth+=(char in '({[')-(char in ')}]')
            if char==',' and not depth:
                declarators.append(group[start:index]);start=index+1
        declarators.append(group[start:])
        for declaration in declarators:
            match=re.search(r'\b(\w+)\s*((?:\[[^\]]*\]\s*)*)$',declaration)
            if not match:raise ValueError('unsupported public layout field: '+declaration)
            name=match[1];result.append(name)
            prefix=name+'[0]' if match[2] else name
            result.extend(prefix+'.'+child for child in children)
    return result


def specification(root):
    headers,services=load(root);types=snapshot(headers,services)['types'];checks=[];tables=[]
    for name,spec in sorted(types.items()):
        declaration=spec.get('declaration','')
        if re.fullmatch(r'(struct|union) \w+ \w+',declaration):continue
        checks.extend([('sizeof('+name+')','sizeof('+name+')'),('alignof('+name+')','__alignof__('+name+')')])
        if 'table' in spec:
            fields=members(';'.join(spec['table']));tables.append(name)
        elif declaration.startswith(('struct ','union ')) and '{' in declaration:
            fields=members(declaration[declaration.index('{')+1:declaration.rindex('}')])
        else:fields=[]
        checks.extend(('offsetof('+name+','+field+')','offsetof('+name+','+field+')') for field in fields)
    if len({name for name,_ in checks})!=len(checks):raise ValueError('duplicate public layout check')
    return headers,checks,tables


def object_values(path):
    data=path.read_bytes()
    if data[:6]!=b'\x7fELF\x01\x01' or struct.unpack_from('<H',data,18)[0]!=94:
        raise ValueError('layout probe is not a little-endian ELF32 Xtensa object')
    offset=struct.unpack_from('<I',data,32)[0];entry_size,count=struct.unpack_from('<HH',data,46)
    sections=[struct.unpack_from('<10I',data,offset+index*entry_size) for index in range(count)]
    for section in sections:
        if section[1]!=2:continue
        strings_section=sections[section[6]]
        strings=data[strings_section[4]:strings_section[4]+strings_section[5]]
        for at in range(section[4],section[4]+section[5],section[9]):
            name,value,size,_,_,index=struct.unpack_from('<IIIBBH',data,at)
            if strings[name:strings.find(b'\0',name)]!=b'solar_os_native_abi_layout':continue
            target=sections[index];raw=data[target[4]+value:target[4]+value+size]
            return struct.unpack('<'+'I'*(size//4),raw)
    raise ValueError('layout symbol missing from probe')


def compatibility_errors(old,new):
    errors=[]
    if old.get('target')!=new.get('target'):return ['layout target changed']
    for name,value in old['values'].items():
        current=new['values'].get(name)
        if name.startswith('sizeof(') and name[7:-1] in old['tables']:
            valid=current is not None and current>=value
        else:valid=current==value
        if not valid:errors.append('incompatible public layout: '+name)
    if not set(old['tables']).issubset(new['tables']):errors.append('public table layout removed')
    return errors


def compile_layout(root,compiler,frontend):
    headers,checks,tables=specification(root)
    source=''.join('#include "'+name+'"\n' for name in sorted(headers))
    source+='uint32_t solar_os_native_abi_layout[] = {\n'+',\n'.join(expression for _,expression in checks)+'\n};\n'
    source+='int main(int argc,char **argv){(void)argc;(void)argv;return solar_os_native_abi_layout[0]==0;}\n'
    pin=json.loads((root/'modules/experimental/tinycc/upstream.json').read_text())['revision']
    with tempfile.TemporaryDirectory() as directory:
        source_path=Path(directory,'layout.c');gcc=Path(directory,'gcc.o');tcc=Path(directory,'tcc.o');source_path.write_text(source)
        subprocess.run([str(compiler),'-std=c11','-c','-I',str(root/'modules/experimental/tinycc/sdk'),
            '-I',str(root/'include'),str(source_path),'-o',str(gcc)],check=True)
        subprocess.run([str(frontend),str(source_path),str(tcc),str(root/'modules/experimental/tinycc/sdk'),
            str(root/'.pio/tinycc/source'/('tinycc-'+pin)/'include'),str(root/'include')],check=True)
        firmware,modules=object_values(gcc),object_values(tcc)
    if len(firmware)!=len(checks) or len(modules)!=len(checks):raise ValueError('layout probe length mismatch')
    mismatches=[name for (name,_),a,b in zip(checks,firmware,modules) if a!=b]
    if mismatches:raise ValueError('firmware GCC and OS C frontend disagree: '+', '.join(mismatches))
    return {'target':'xtensa-esp32s3-ilp32-le','tables':tables,'values':{name:value for (name,_),value in zip(checks,firmware)}}


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=ROOT)
    parser.add_argument('--compiler',type=Path,default=Path.home()/'.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc')
    parser.add_argument('--frontend',type=Path)
    parser.add_argument('--update-baseline',action='store_true')
    parser.add_argument('--baseline',type=Path,help='Also verify a previous release/base-branch layout')
    args=parser.parse_args(argv);root=args.root.resolve();path=root/BASELINE
    current=compile_layout(root,args.compiler,args.frontend or root/'.pio/tinycc/solar-tcc')
    for previous in ([path] if path.is_file() else [])+([args.baseline] if args.baseline else []):
        errors=compatibility_errors(json.loads(previous.read_text()),current)
        if errors:raise ValueError('\n'.join(errors))
    if args.update_baseline:path.write_text(json.dumps(current,indent=2,sort_keys=True)+'\n')
    elif not path.is_file() or json.loads(path.read_text())!=current:
        raise ValueError('new public layouts need review: run check_native_layout.py --update-baseline')
    print(f'native layout: {len(current["values"])} sizes/alignments/offsets agree between firmware GCC, OS C frontend and target baseline')


if __name__=='__main__':
    try:main()
    except (ValueError,FileNotFoundError) as error:
        print(error,file=sys.stderr);sys.exit(1)
