import struct
from pathlib import Path

def fix(path):
    data=bytearray(Path(path).read_bytes())
    if len(data)<52 or data[:7]!=b'\x7fELF\x01\x01\x01':
        raise ValueError('expected a little-endian ELF32 file')
    if struct.unpack_from('<HH',data,16)!=(3,94):
        raise ValueError('expected an Xtensa shared ELF')
    shoff=struct.unpack_from('<I',data,32)[0]
    count,names_index=struct.unpack_from('<HH',data,48)
    if not count or names_index>=count or struct.unpack_from('<H',data,46)[0]!=40 or shoff+40*count>len(data):
        raise ValueError('invalid ELF section table')
    sections=[list(struct.unpack_from('<10I',data,shoff+40*i)) for i in range(count)]
    if any(s[1]!=8 and s[4]+s[5]>len(data) for s in sections):
        raise ValueError('section data outside ELF')
    names=sections[names_index]
    def string(offset):
        end=data.index(0,offset);return bytes(data[offset:end]).decode()
    by_name={string(names[4]+s[0]):s for s in sections}
    symbols={}
    sym=by_name['.symtab']; strings=sections[sym[6]]
    for pos in range(sym[4],sym[4]+sym[5],16):
        name,value=struct.unpack_from('<II',data,pos)
        if name:symbols[string(strings[4]+name)]=value
    def filepos(addr,width=1):
        for s in sections:
            if s[2]&2 and s[1]!=8 and s[3]<=addr and addr+width<=s[3]+s[5]:return s[4]+addr-s[3]
        raise ValueError('unmapped address '+hex(addr))
    def u32(addr):return struct.unpack_from('<I',data,filepos(addr,4))[0]
    def byte(addr):return data[filepos(addr)]
    def uleb(addr):
        value=0; shift=0
        for i in range(5):
            b=byte(addr);addr+=1;value|=(b&127)<<shift
            if b<128:return value,addr
            shift+=7
        raise ValueError('large LEB')
    def sleb(addr):
        value=0;shift=0
        for i in range(5):
            b=byte(addr);addr+=1;value|=(b&127)<<shift;shift+=7
            if b<128:return value-(1<<shift) if b&64 else value,addr
        raise ValueError('large LEB')
    extra=[]; patched=set()
    existing=by_name['.rela.dyn']
    old_relocs=bytes(data[existing[4]:existing[4]+existing[5]])
    existing_offsets={struct.unpack_from('<I',old_relocs,i)[0]:struct.unpack_from('<I',old_relocs,i+4)[0]&255 for i in range(0,len(old_relocs),12)}
    def pointer(addr,encoding):
        if encoding==255:return None
        if encoding&15 not in (0,3,11):raise ValueError('unsupported pointer encoding '+hex(encoding))
        value=u32(addr)
        if not value:return 0
        if encoding&0x70==0x10:
            value=(addr+struct.unpack('<i',struct.pack('<I',value))[0])&0xffffffff
        elif encoding&0x70:raise ValueError('unsupported pointer base')
        if addr not in patched:
            if addr in existing_offsets:
                if encoding&0x70==0 and existing_offsets[addr]==5:return value
                raise ValueError('pointer already has a dynamic relocation '+hex(addr))
            struct.pack_into('<I',data,filepos(addr),value)
            extra.append(struct.pack('<III',addr,5,0));patched.add(addr)
        return value
    def encoding(addr):
        old=byte(addr)
        if old!=255:data[filepos(addr)]=(old&0x80)|3
        return old
    lsda_seen=set()
    def lsda(addr):
        if not addr or addr in lsda_seen:return
        lsda_seen.add(addr)
        if not symbols['__module_lsda_start']<=addr<symbols['__module_lsda_end']:
            raise ValueError('LSDA outside its section')
        lp=encoding(addr);addr+=1
        if lp!=255:pointer(addr,lp);addr+=4
        tt=encoding(addr);addr+=1
        ttbase=None
        if tt!=255:
            skip,addr=uleb(addr);ttbase=addr+skip
        call_encoding=byte(addr);addr+=1
        if call_encoding!=1:raise ValueError('non-LEB call site encoding')
        length,addr=uleb(addr);end=addr+length;actions=[]
        while addr<end:
            for i in range(4):
                value,addr=uleb(addr)
                if i==3 and value:actions.append(value)
        if addr!=end:raise ValueError('call site length')
        max_type=0;visited=set()
        for action in actions:
            pos=end+action-1
            while pos not in visited:
                visited.add(pos)
                filter,pos=sleb(pos);offset_addr=pos;next,pos=sleb(pos)
                if filter>0:max_type=max(max_type,filter)
                elif filter<0:
                    if ttbase is None:raise ValueError('exception spec without types')
                    spec=ttbase-filter-1
                    while True:
                        value,spec=uleb(spec)
                        if not value:break
                        max_type=max(max_type,value)
                if not next:break
                pos=offset_addr+next
        if max_type and ttbase is None:raise ValueError('type filter without types')
        for index in range(1,max_type+1):pointer(ttbase-4*index,tt)
    start,end=symbols['__module_eh_start'],symbols['__module_eh_end']
    cies={};addr=start
    while addr<end:
        length=u32(addr)
        if length==0:break
        if length==0xffffffff or addr+4+length>end:raise ValueError('frame length')
        finish=addr+4+length;identity=u32(addr+4);pos=addr+8
        if identity==0:
            version=byte(pos);pos+=1
            augmentation=''
            while byte(pos):augmentation+=chr(byte(pos));pos+=1
            pos+=1
            code,pos=uleb(pos);align,pos=sleb(pos)
            if version==1:pos+=1
            else:reg,pos=uleb(pos)
            if augmentation and not augmentation.startswith('z'):raise ValueError('non-z augmentation '+augmentation)
            record={'encoding':0,'lsda':255,'aug':bool(augmentation)}
            if augmentation:size,pos=uleb(pos);augend=pos+size
            else:augend=pos
            for kind in augmentation[1:]:
                if kind=='R':record['encoding']=encoding(pos);pos+=1
                elif kind=='L':record['lsda']=encoding(pos);pos+=1
                elif kind=='P':
                    old=encoding(pos);pos+=1;pointer(pos,old);pos+=4
                else:raise ValueError('augmentation '+kind)
            if pos!=augend:raise ValueError('augmentation length')
            cies[addr]=record
        else:
            record=cies[addr+4-identity]
            pointer(pos,record['encoding']);pos+=8
            if record['aug']:size,pos=uleb(pos)
            if record['lsda']!=255:
                target=pointer(pos,record['lsda']);lsda(target)
        addr=finish
    align=(-len(data))%4;data.extend(bytes(align))
    offset=len(data);data.extend(old_relocs+b''.join(extra))
    index=sections.index(existing);existing[4]=offset;existing[5]=len(old_relocs)+12*len(extra)
    struct.pack_into('<10I',data,shoff+40*index,*existing)
    Path(path).write_bytes(data)
    print('Converted',len(extra),'exception pointers across',len(cies),'CIEs and',len(lsda_seen),'LSDAs')

if __name__=='__main__':
    import sys
    fix(sys.argv[1])
