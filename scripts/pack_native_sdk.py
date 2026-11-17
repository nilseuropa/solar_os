#!/usr/bin/env python3
"""Embed a library's declared C headers in its signed native ELF artifact."""
import argparse
from pathlib import Path
import re
import struct
import tempfile
import os
import tomllib

SECTION=b'.solaros.sdk\0'
HEADER=96
RECORD=104


def safe_path(name):
    return (isinstance(name,str) and name and not name.startswith('/') and
            all(part not in ('.','..') and re.fullmatch(r'[A-Za-z0-9_.-]+',part)
                for part in name.split('/')))


def load_headers(manifest, source_root=None):
    manifest=Path(manifest).resolve()
    source_root=Path(source_root).resolve() if source_root else manifest.parent
    meta=tomllib.loads(manifest.read_text())
    declarations=meta.get('sdk',{}).get('headers',{})
    if meta.get('type')!='library' or not isinstance(declarations,dict) or not 1<=len(declarations)<=32:
        raise ValueError('SDK requires a library with 1..32 [sdk.headers] entries')
    module_id=meta['id'];version=meta['version']
    if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,46}',module_id): raise ValueError('invalid SDK module id')
    if not version or len(version.encode('ascii'))>=32: raise ValueError('SDK version exceeds 31 ASCII bytes')
    entries=[]
    for name,relative in sorted(declarations.items()):
        if not safe_path(name) or not safe_path(relative): raise ValueError('SDK paths must be safe relative paths')
        path=(source_root/relative).resolve()
        path.relative_to(source_root)
        text=path.read_bytes();text.decode('utf-8')
        full=module_id+'/'+name
        if len(full.encode())>=96 or len(text)>32768 or b'\0' in text:
            raise ValueError('SDK header exceeds path/text bounds or contains NUL')
        entries.append((full,text,path))
    if sum(len(text) for _,text,_ in entries)>262144: raise ValueError('SDK exceeds 256 KiB of header text')
    return module_id,version,entries


def section_data(manifest, source_root=None):
    module_id,version,entries=load_headers(manifest,source_root)
    offset=HEADER+len(entries)*RECORD
    directory=bytearray();text=bytearray()
    for name,data,_ in entries:
        directory.extend(struct.pack('<96sII',name.encode(),offset+len(text),len(data)))
        text.extend(data)
    return struct.pack('<8sII48s32s',b'SOLSDK1\0',offset+len(text),len(entries),
                       module_id.encode(),version.encode())+directory+text


def pack_elf(data,manifest,source_root=None):
    if len(data)<52 or data[:7]!=b'\x7fELF\1\1\1' or struct.unpack_from('<HH',data,16)!=(3,94):
        raise ValueError('SDK input must be little-endian Xtensa ELF32 ET_DYN')
    table=struct.unpack_from('<I',data,32)[0]
    entry_size,count,names=struct.unpack_from('<HHH',data,46)
    if entry_size!=40 or not 1<count<128 or names>=count or table+count*40>len(data):
        raise ValueError('invalid ELF section table')
    sections=[list(struct.unpack_from('<10I',data,table+i*40)) for i in range(count)]
    strings=sections[names]
    if strings[4]+strings[5]>len(data): raise ValueError('invalid ELF section names')
    names_data=data[strings[4]:strings[4]+strings[5]]
    for s in sections:
        if s[0]>=len(names_data) or b'\0' not in names_data[s[0]:]: raise ValueError('invalid ELF section name')
        if names_data[s[0]:].split(b'\0',1)[0]==SECTION[:-1]:
            raise ValueError('pack the original build artifact, not an already packaged ELF')
    payload=section_data(manifest,source_root)
    result=bytearray(data)
    result.extend(b'\0'*(-len(result)%4));payload_offset=len(result);result.extend(payload)
    names_offset=len(result);result.extend(names_data+SECTION)
    sections[names][4]=names_offset;sections[names][5]=len(names_data)+len(SECTION)
    sections.append([len(names_data),1,0,0,payload_offset,len(payload),0,0,1,0])
    result.extend(b'\0'*(-len(result)%4));new_table=len(result)
    for section in sections:result.extend(struct.pack('<10I',*section))
    struct.pack_into('<I',result,32,new_table)
    struct.pack_into('<H',result,48,len(sections))
    return bytes(result)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',required=True,type=Path)
    parser.add_argument('--source-root',type=Path,help='Explicit root for shared public headers')
    parser.add_argument('--input',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--list-inputs',action='store_true')
    args=parser.parse_args()
    if args.list_inputs:
        print(';'.join(str(p) for _,_,p in load_headers(args.manifest,args.source_root)[2]),end='');return
    if not args.input or not args.output:parser.error('--input and --output are required')
    data=pack_elf(args.input.read_bytes(),args.manifest,args.source_root)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=args.output.parent,delete=False) as stream:
        temporary=Path(stream.name)
        try:
            stream.write(data);stream.flush();os.fsync(stream.fileno())
            os.fchmod(stream.fileno(),args.input.stat().st_mode&0o777)
            os.replace(temporary,args.output)
        finally:temporary.unlink(missing_ok=True)


if __name__=='__main__':main()
