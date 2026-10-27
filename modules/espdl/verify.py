"""Check the section/relocation subset actually mapped by the native loader."""
import struct
from pathlib import Path


def remove_noop_relocations(path):
    data=bytearray(Path(path).read_bytes())
    offset=struct.unpack_from('<I',data,32)[0]
    count=struct.unpack_from('<H',data,48)[0]
    for index in range(count):
        position=offset+40*index
        section=list(struct.unpack_from('<10I',data,position))
        if section[1]!=4:continue
        if section[5]%12:raise ValueError('relocation size')
        records=[data[pos:pos+12] for pos in range(section[4],section[4]+section[5],12)]
        kept=b''.join(record for record in records if struct.unpack_from('<I',record,4)[0]&255)
        data[section[4]:section[4]+len(kept)]=kept
        section[5]=len(kept)
        struct.pack_into('<10I',data,position,*section)
    Path(path).write_bytes(data)


def verify(path, allowed_imports):
    data=Path(path).read_bytes()
    if len(data)<52 or data[:7]!=b'\x7fELF\x01\x01\x01':raise ValueError('expected ELF32')
    if struct.unpack_from('<HH',data,16)!=(3,94):raise ValueError('expected Xtensa shared ELF')
    offset=struct.unpack_from('<I',data,32)[0]
    size,count,names_index=struct.unpack_from('<HHH',data,46)
    if size!=40 or not count or names_index>=count or offset+40*count>len(data):raise ValueError('section table')
    sections=[struct.unpack_from('<10I',data,offset+40*i) for i in range(count)]
    if any(s[1]!=8 and s[4]+s[5]>len(data) for s in sections):raise ValueError('section bounds')
    def string(table,index):
        if index>=table[5]:raise ValueError('string bounds')
        start=table[4]+index
        return data[start:data.index(0,start,table[4]+table[5])].decode()
    by_name={string(sections[names_index],s[0]):s for s in sections}
    mapped=[s for name,s in by_name.items() if name in ('.text','.data','.bss','.rodata','.data.rel.ro')]
    def region(address,width=1):
        for s in mapped:
            if s[3]<=address and address+width<=s[3]+s[5]:return s
        raise ValueError('unmapped relocation address '+hex(address))
    dynsym=by_name['.dynsym'];strings=sections[dynsym[6]]
    imports=set()
    for pos in range(dynsym[4],dynsym[4]+dynsym[5],16):
        name,value,_,info,_,section=struct.unpack_from('<IIIBBH',data,pos)
        if name and not section:
            imports.add(string(strings,name))
    unexpected=imports-set(allowed_imports)
    if unexpected:raise ValueError('unclosed imports: '+', '.join(sorted(unexpected)))
    for name in ('.rela.dyn','.rela.plt'):
        if name not in by_name:continue
        s=by_name[name]
        if s[5]%12:raise ValueError('relocation size')
        for pos in range(s[4],s[4]+s[5],12):
            address,info,addend=struct.unpack_from('<III',data,pos)
            kind=info&255
            if kind not in (2,3,4,5):raise ValueError('unsupported relocation '+str(kind))
            destination=region(address,4)
            if kind==5:
                if destination[1]==8:raise ValueError('relative relocation in BSS')
                value=struct.unpack_from('<I',data,destination[4]+address-destination[3])[0]
                if value:region(value)
    return sorted(imports)
