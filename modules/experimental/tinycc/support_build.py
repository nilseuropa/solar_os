"""Pack a closed, GCC-built runtime into TinyCC section/word relocations."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def prepare_support(here, cache, source):
    output=cache / "support"; output.mkdir(exist_ok=True)
    cc=Path.home()/".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc"
    prefix=str(cc)[:-3]
    def run(*args): subprocess.run(list(map(str,args)),check=True,timeout=120)
    flags=["-std=c11","-Os","-fPIC","-mlongcalls","-ffp-contract=off","-fno-unwind-tables","-fno-asynchronous-unwind-tables"]
    run(cc,*flags,"-c",here/"support.c","-o",output/"support.o")
    libgcc=subprocess.check_output([str(cc),"-print-libgcc-file-name"],text=True).strip()
    run(prefix+"objcopy","--remove-section=.eh_frame",libgcc,output/"libgcc.a")
    layout=here.parents[1]/"espdl/layout.ld"
    run(cc,"-nostdlib","-nostartfiles","-shared","-Wl,-Bsymbolic","-Wl,--exclude-libs,ALL",
        "-Wl,-e,__solar_cc_divdi3","-Wl,-T,"+str(layout),
        output/"support.o",output/"libgcc.a","-o",output/"support.elf")
    data=(output/"support.elf").read_bytes()
    if data[:7]!=b"\x7fELF\1\1\1" or struct.unpack_from("<HH",data,16)!=(3,94):
        raise ValueError("runtime must be Xtensa ELF32 little-endian ET_DYN")
    table=struct.unpack_from("<I",data,32)[0]; count,names=struct.unpack_from("<HH",data,48)
    sections=[struct.unpack_from("<10I",data,table+i*40) for i in range(count)]
    strings=sections[names]
    def name(s):
        at=strings[4]+s[0]; return data[at:data.index(0,at)].decode()
    groups={".text":0,".data.rel.ro":1,".data":2,".got":2,".bss":3}
    metadata={".hash",".gnu.hash",".dynsym",".dynstr",".rela.dyn",".rela.plt",
              ".dynamic",".got.loc",".note.gnu.build-id"}
    for s in sections:
        if s[2]&2 and s[5] and name(s) not in groups and name(s) not in metadata:
            raise ValueError("runtime has an unsupported allocated section: "+name(s))
    selected={i:s for i,s in enumerate(sections) if name(s) in groups and s[5]}
    indices={i:n for n,i in enumerate(selected)}
    def locate(address):
        for i,s in selected.items():
            if s[3]<=address<s[3]+s[5]: return indices[i],address-s[3]
        for i,s in selected.items():
            if address==s[3]+s[5]: return indices[i],s[5]
        raise ValueError(f"runtime relocation outside copied sections: {address:x}")
    definitions=[]; relocations=[]
    for i,s in enumerate(sections):
        if s[1]==11:
            syms=sections[s[6]]; text=data[syms[4]:syms[4]+syms[5]]
            for at in range(s[4]+16,s[4]+s[5],16):
                n,value,size,info,other,index=struct.unpack_from("<IIIBBH",data,at)
                symbol=text[n:].split(b"\0",1)[0].decode()
                if not index and symbol: raise ValueError("runtime must be closed: "+symbol)
                if symbol.startswith("__solar_cc_") and index in selected:
                    definitions.append((symbol,indices[index],value-sections[index][3],size))
        if s[1]==4 and s[2]&2:
            for at in range(s[4],s[4]+s[5],12):
                offset,info,addend=struct.unpack_from("<IIi",data,at)
                if info!=5 or addend!=0:
                    raise ValueError("runtime requires internal relative word relocations")
                patch,within=locate(offset); section=selected[list(selected)[patch]]
                if section[1]==8 or within+4>section[5]:
                    raise ValueError("runtime relocation must patch a stored word")
                value=struct.unpack_from("<I",data,section[4]+within)[0]
                target,displacement=locate(value)
                relocations.append((patch,within,target,displacement))
    rows=[]
    for n,(i,s) in enumerate(selected.items()):
        if not 0<s[8]<=16 or s[8]&(s[8]-1):
            raise ValueError("runtime section alignment exceeds the module writer's limit")
        values=data[s[4]:s[4]+s[5]] if s[1]!=8 else b""
        rows.append(f"static const unsigned char cc_support_bytes_{n}[]={{"+",".join(str(v) for v in values)+"};\n")
    rows.append("static const struct cc_support_section cc_support_sections[]={\n")
    rows.extend(f"{{{groups[name(s)]},{s[8]},{s[5]},cc_support_bytes_{n}}},\n" for n,s in enumerate(selected.values()))
    rows.append("};\nstatic const struct cc_support_symbol cc_support_symbols[]={\n")
    conversion_aliases={
        "u64_f32":["__floatundisf"],"u64_f64":["__floatundidf","__floatundixf"],
        "i64_f32":["__floatdisf"],"i64_f64":["__floatdidf","__floatdixf"],
        "f32_u64":["__fixunssfdi"],"f64_u64":["__fixunsdfdi","__fixunsxfdi"],
        "f32_i64":["__fixsfdi"],"f64_i64":["__fixdfdi","__fixxfdi"],
    }
    for symbol,index,value,size in definitions:
        rows.append(f'{{"{symbol}",{index},{value},{size}}},\n')
        for alias in conversion_aliases.get(symbol[len("__solar_cc_"):],[]):
            rows.append(f'{{"{alias}",{index},{value},{size}}},\n')
        if symbol[len("__solar_cc_"):] in ("divdi3","udivdi3","moddi3","umoddi3","ashrdi3","lshrdi3","ashldi3"):
            rows.append(f'{{"__{symbol[len("__solar_cc_"):]}",{index},{value},{size}}},\n')
    rows.append("};\nstatic const struct cc_support_relocation cc_support_relocations[]={\n")
    rows.extend("{"+",".join(map(str,r))+"},\n" for r in relocations)
    rows.append("};\n")
    (source/"support.inc").write_text("".join(rows))
    report={"source":hashlib.sha256((here/"support.c").read_bytes()).hexdigest(),
            "layout":hashlib.sha256(layout.read_bytes()).hexdigest(),"flags":flags,
            "libgcc_sha256":hashlib.sha256(Path(libgcc).read_bytes()).hexdigest(),
            "gcc":subprocess.check_output([str(cc),"-dumpfullversion"],text=True).strip(),
            "elf_sha256":hashlib.sha256(data).hexdigest(),"sections":len(selected),"relocations":len(relocations),
            "definitions":[row[0] for row in definitions]}
    (output/"build.json").write_text(json.dumps(report,indent=2)+"\n")
