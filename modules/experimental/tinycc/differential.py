#!/usr/bin/env python3
"""Compare TinyCC code and ABI behavior against ESP32-S3 GCC at -O0 and -O2."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import shutil

from build import HERE, ROOT, CACHE, run
sys.path.insert(0,str(HERE / "differential"))
from corpus import cases, vectors


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu",type=Path,required=True)
    parser.add_argument("--seed",type=lambda value:int(value,0),default=0x534f4c41)
    parser.add_argument("--random-cases",type=int,default=16)
    parser.add_argument("--vectors",type=int,default=128)
    parser.add_argument("--case",help="run one named case, keeping its generated source")
    parser.add_argument("--inject-mismatch",action="store_true",help="negative control: corrupt one host output word; must fail")
    args=parser.parse_args()
    if not 0<=args.random_cases<=128 or not 57<=args.vectors<=4096:
        parser.error("expected 0..128 generated cases and 57..4096 vectors")
    if args.inject_mismatch and not args.case: parser.error("--inject-mismatch requires --case")
    output=CACHE / "differential"; output.mkdir(parents=True,exist_ok=True)
    report=output / "report.json"
    if report.exists(): report.unlink()
    # Regenerate compilers so a passing report cannot silently reuse an earlier
    # backend or runtime. Preserve sources and the console when a test fails.
    for script in ("build.py","native_build.py"):
        with (output / (script+".log")).open("w") as log:
            subprocess.run([sys.executable,str(HERE / script)],stdout=log,stderr=subprocess.STDOUT,check=True)
    suite=cases(args.seed,args.random_cases)
    if args.case:
        if args.case not in suite: parser.error("unknown case: "+args.case)
        suite={args.case:suite[args.case]}
    source=CACHE / "source" / ("tinycc-"+json.loads((HERE / "upstream.json").read_text())["revision"])
    includes=[HERE / "sdk",source / "include",ROOT / "include",output]
    cc=Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc"
    gcc_flags=["-std=c11","-fno-lto","-fPIC","-fvisibility=hidden","-mlongcalls","-ffp-contract=off"]
    header=(HERE / "differential/abi.h").read_text()
    (output / "diff_abi.h").write_text(header)
    generated=["static const char abi_header[]="+json.dumps(header)+";\n"]
    rows=[]; hashes={}
    sys.path.insert(0,str(ROOT / "modules/espdl"))
    from verify import verify
    for name,text in suite.items():
        path=output / (name+".c"); path.write_text(text)
        obj=path.with_suffix(".o"); elf=path.with_suffix(".elf")
        compiled_text=text
        if args.inject_mismatch:
            compiled_text=text.replace('(void)solar_os_native_host_v1();','(void)solar_os_native_host_v1(); out[31]^=1U;')
            compiled_path=output / "negative-control.c"; compiled_path.write_text(compiled_text)
        else: compiled_path=path
        run(CACHE / "solar-tcc",compiled_path,obj,*includes)
        run(CACHE / "module",obj,elf,"diff_probe")
        run(CACHE / "elf-check",elf)
        verify(elf,{"solar_os_native_host_v1","memcpy","memset","memmove","memcmp","strlen"})
        reference_hashes={}
        for level in (0,2):
            reference=output / f"reference-{level}-{name}"
            run(cc,*gcc_flags,f"-O{level}",
                "-I",ROOT / "include","-I",output,"-c",path,"-o",str(reference)+".o")
            run(cc,"-nostdlib","-nostartfiles","-shared","-Wl,-Bsymbolic","-Wl,-e,diff_probe",
                "-Wl,--exclude-libs,ALL","-Wl,-T,"+str(ROOT / "modules/espdl/layout.ld"),str(reference)+".o",
                CACHE/"support/libgcc.a","-o",str(reference)+".elf")
            reference_data=Path(str(reference)+".elf").read_bytes()
            run(CACHE / "elf-check",str(reference)+".elf")
            verify(Path(str(reference)+".elf"),{"solar_os_native_host_v1","memcpy","memset","memmove","memcmp","strlen"})
            reference_hashes[str(level)]=hashlib.sha256(reference_data).hexdigest()
            generated.append(f"static const uint8_t gcc{level}_{name}[]={{"+",".join(str(b) for b in reference_data)+"};\n")
        data=elf.read_bytes()
        generated.append("static const uint8_t elf_"+name+"[]={"+",".join(str(b) for b in data)+"};\n")
        generated.append("static const char source_"+name+"[]="+json.dumps(text)+";\n")
        rows.append('{"'+name+'",source_'+name+',elf_'+name+',sizeof(elf_'+name+'),gcc0_'+name+',gcc2_'+name+',sizeof(gcc0_'+name+'),sizeof(gcc2_'+name+')},\n')
        hashes[name]={"source":hashlib.sha256(text.encode()).hexdigest(),"host_source":hashlib.sha256(compiled_text.encode()).hexdigest(),
                      "host_elf":hashlib.sha256(data).hexdigest(),"gcc_elf":reference_hashes}
    generated.append("static const struct diff_case cases[]={\n"+"".join(rows)+"};\n")
    input_pairs=vectors(args.seed,args.vectors)
    generated.append("static const uint32_t inputs[][2]={\n"+
                     "".join(f"{{0x{x:08x}U,0x{y:08x}U}},\n" for x,y in input_pairs)+"};\n")
    (output / "suite.inc").write_text("".join(generated))
    (output / "inputs.json").write_text(json.dumps(input_pairs)+"\n")
    manifest={"seed":args.seed,"vectors":args.vectors,"cases":hashes,"injected_mismatch":args.inject_mismatch,
              "gcc":subprocess.check_output([str(cc),"-dumpfullversion"],text=True).strip(),
              "gcc_flags":{"common":gcc_flags,"optimization":["-O0","-O2"]},
              "host_tools":{name:hashlib.sha256((CACHE / name).read_bytes()).hexdigest()
                            for name in ("solar-tcc","module","elf-check")},
              "compiler_build":json.loads((CACHE / "native/build.json").read_text()),
              "test_sources":{str(p.relative_to(HERE)):hashlib.sha256(p.read_bytes()).hexdigest()
                              for p in [Path(__file__),HERE / "qemu.py",HERE / "driver.c",HERE / "build.py",
                                        HERE / "native-qemu/main/main.c",HERE / "native-qemu/main/CMakeLists.txt",
                                        HERE / "native-qemu/platformio.ini",*sorted((HERE / "differential").glob("*.*"))] if p.is_file()}}
    (output / "manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
    archive=output / "runs" / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")+f"-{args.seed:08x}")
    archive.mkdir(parents=True)
    for name in suite:
        for filename in (name+".c",name+".elf",f"reference-0-{name}.elf",f"reference-2-{name}.elf"):
            shutil.copyfile(output / filename,archive / filename)
    for filename in ("manifest.json","diff_abi.h","inputs.json"):
        shutil.copyfile(output / filename,archive / filename)
    if args.inject_mismatch: shutil.copyfile(output / "negative-control.c",archive / "negative-control.c")
    try:
        subprocess.run([sys.executable,str(HERE / "qemu.py"),"--differential","--qemu",str(args.qemu)],check=True,timeout=600)
    finally:
        console=CACHE / "differential-qemu/console.log"
        if console.exists(): shutil.copyfile(console,archive / "console.log")
    manifest.update(passed=True,execution=json.loads((CACHE / "differential-qemu/report.json").read_text()))
    report.write_text(json.dumps(manifest,indent=2)+"\n")
    shutil.copyfile(report,archive / "report.json")
    print(f"SOLAR_CC_DIFFERENTIAL_OK: {len(suite)} cases x {args.vectors} vectors x 2 TinyCC hosts")


if __name__=="__main__": main()
