import importlib.util
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
HERE=ROOT/'modules/experimental/tinycc'
spec=importlib.util.spec_from_file_location('pack_native_sdk',ROOT/'scripts/pack_native_sdk.py')
pack=importlib.util.module_from_spec(spec);spec.loader.exec_module(pack)


def fixture():
    data=bytearray(256)
    data[:7]=b'\x7fELF\1\1\1'
    struct.pack_into('<HHI',data,16,3,94,1)
    struct.pack_into('<I',data,32,128)
    struct.pack_into('<HHHH',data,40,52,0,0,40)
    struct.pack_into('<HH',data,48,3,2)
    names=b'\0.text\0.shstrtab\0';data[80:80+len(names)]=names
    struct.pack_into('<10I',data,168,1,1,6,0x100,64,16,0,0,4,0)
    struct.pack_into('<10I',data,208,7,3,0,0,80,len(names),0,0,1,0)
    return bytes(data)


class NativeSdkTest(unittest.TestCase):
    def test_shared_headers_are_confined_to_explicit_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);public=root/'include';public.mkdir()
            (public/'api.h').write_text('#define SHARED 1\n')
            module=root/'module';module.mkdir()
            manifest=module/'module.toml'
            manifest.write_text('type="library"\nid="shared"\nversion="1.0"\n[sdk.headers]\n"api.h"="api.h"\n')
            self.assertEqual(pack.load_headers(manifest,public)[2][0][1],b'#define SHARED 1\n')
            (public/'api.h').unlink()
            (root/'secret.h').write_text('private')
            (public/'api.h').symlink_to(root/'secret.h')
            with self.assertRaises(ValueError):pack.load_headers(manifest,public)

    @classmethod
    def setUpClass(cls):
        cls.temporary=tempfile.TemporaryDirectory()
        cls.path=Path(cls.temporary.name)
        (cls.path/'api.h').write_text('#define ANSWER 42\n')
        (cls.path/'detail.h').write_text('#include "../api.h"\n')
        cls.manifest=cls.path/'module.toml'
        cls.manifest.write_text('type="library"\nid="hello-library"\nversion="1.0.0"\n'
                                '[sdk.headers]\n"api.h"="api.h"\n"detail/inner.h"="detail.h"\n')
        cls.valid=pack.pack_elf(fixture(),cls.manifest)
        cls.payload=256
        cls.binary=cls.path/'reader'
        harness=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdk_reader.h"
static unsigned char *data;static uint32_t length;
static int read_data(void *ctx,uint32_t offset,void *out,uint32_t bytes)
{(void)ctx;assert(offset<=length && bytes<=length-offset);memcpy(out,data+offset,bytes);return 0;}
static int resolve(void *ctx,const char *name,char *out,size_t size)
{(void)ctx;assert(strlen(name)<size);strcpy(out,name);return 0;}
static int file_size(void *ctx,const char *name,uint32_t *bytes)
{(void)ctx;assert(!strcmp(name,"/modules/libraries/hello-library.elf"));*bytes=length;return 0;}
static int storage_read(void *ctx,const char *name,uint32_t offset,void *out,size_t size,size_t *bytes)
{(void)name;int code=read_data(ctx,offset,out,size);*bytes=code?0:size;return code;}
int main(int argc,char **argv)
{
 assert(argc==6);FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);length=ftell(f);rewind(f);
 data=malloc(length?length:1);assert(fread(data,1,length,f)==length);fclose(f);
 struct {unsigned char before[16];solar_os_cc_sdk_index_t index;unsigned char after[16];} indexed;
 memset(&indexed,0x5a,sizeof(indexed));
 int index_code=solar_os_cc_sdk_index(read_data,NULL,length,argv[2],&indexed.index);
 for(unsigned i=0;i<16;i++)assert(indexed.before[i]==0x5a && indexed.after[i]==0x5a);
 if(!index_code) {
  assert(indexed.index.count && indexed.index.count<=32 && memchr(indexed.index.version,0,32));
  for(unsigned i=0;i<indexed.index.count;i++) {
   assert(memchr(indexed.index.entries[i].name,0,96));
   assert(indexed.index.entries[i].offset<=length && indexed.index.entries[i].bytes<=length-indexed.index.entries[i].offset);
  }
 }
 uint32_t bytes=0;int expected=atoi(argv[4]);
 solar_os_native_storage_api_v1_t storage={.resolve_path=resolve,.file_size=file_size,.read_file=storage_read};
 int dispatch=!strcmp(argv[5],"dispatch");
 int code=dispatch ? solar_os_cc_sdk_read(&storage,argv[3],NULL,0,&bytes) :
   solar_os_cc_sdk_extract(read_data,NULL,length,argv[2],argv[3],NULL,0,&bytes);
 if(expected==778) assert(!code);
 else if(expected!=777) assert(code==expected);
 if(!code) {
  unsigned char *out=malloc(bytes+32);memset(out,0x5a,bytes+32);uint32_t actual=0;
  code=dispatch ? solar_os_cc_sdk_read(&storage,argv[3],out+16,bytes,&actual) :
   solar_os_cc_sdk_extract(read_data,NULL,length,argv[2],argv[3],out+16,bytes,&actual);
  if(expected==778) assert(code==SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
  else if(expected!=777) assert(!code && actual==bytes);
  for(unsigned i=0;i<16;i++) assert(out[i]==0x5a && out[bytes+16+i]==0x5a);
  if(expected==0) assert(bytes==18 && !memcmp(out+16,"#define ANSWER 42\n",18));
  if(bytes && !code) assert(solar_os_cc_sdk_extract(read_data,NULL,length,"hello-library","hello-library/api.h",out,0,&actual)==-10);
  free(out);
 }
 free(data);return 0;
}
'''
        subprocess.run(['cc','-x','c','-std=c11','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
                        '-I',str(ROOT/'include'),'-I',str(HERE),'-',str(HERE/'sdk_reader.c'),'-o',str(cls.binary)],
                       input=harness,text=True,check=True)

    @classmethod
    def tearDownClass(cls):cls.temporary.cleanup()

    def check_data(self,data,expected,header='hello-library/api.h',module='hello-library',mode='extract'):
        path=self.path/'test.elf';path.write_bytes(data)
        subprocess.run([str(self.binary),str(path),module,header,str(expected),mode],check=True,timeout=10)

    def test_packaging_and_namespaced_parent_includes(self):
        self.check_data(self.valid,0)
        self.check_data(self.valid,0,'/module-sdk/hello-library/detail/../api.h',mode='dispatch')
        self.check_data(self.valid,-5,'/module-sdk/hello-library/../../api.h',mode='dispatch')
        self.check_data(self.valid,-5,'x',mode='dispatch')
        self.check_data(self.valid,-5,'hello-library/missing.h')
        self.check_data(fixture(),-5)

    def test_rejects_corrupt_metadata_and_incorrect_module(self):
        self.check_data(self.valid,-10,module='another-library')
        for offset,value in ((self.payload,ord('X')),(self.payload+8,0),(self.payload+12,33),
                             (self.payload+96+96,0),(self.payload+96+100,255)):
            data=bytearray(self.valid);data[offset]=value
            with self.subTest(offset=offset):self.check_data(data,-10)
        data=bytearray(self.valid);data[self.payload+96:self.payload+96+96]=b'A'*96
        self.check_data(data,-10)
        data=bytearray(self.valid);data[self.payload+96+2*104]=0
        self.check_data(data,778)

    def test_truncated_and_mutated_files_stay_within_bounds(self):
        for size in (0,1,16,51,52,127,255,256,len(self.valid)-1):self.check_data(self.valid[:size],777)
        rng=random.Random(0x53444b)
        for _ in range(100):
            data=bytearray(self.valid);offset=rng.randrange(len(data));data[offset]^=rng.randrange(1,256)
            self.check_data(data,777)

    def test_manifest_rejects_escape_and_oversize_and_repack(self):
        with self.assertRaises(ValueError):pack.pack_elf(self.valid,self.manifest)
        original=self.manifest.read_text()
        for path in ('../api.h','/api.h','detail/../api.h'):
            self.manifest.write_text(original.replace('"api.h"="api.h"',f'"{path}"="api.h"'))
            with self.assertRaises(ValueError):pack.load_headers(self.manifest)
        self.manifest.write_text(original)
        old=(self.path/'api.h').read_text();(self.path/'api.h').write_text('x'*32769)
        with self.assertRaises(ValueError):pack.load_headers(self.manifest)
        (self.path/'api.h').write_text(old)

    def test_cmake_packages_original_artifact_deterministically(self):
        project=self.path/'cmake-project';project.mkdir(exist_ok=True)
        for name in ('module.toml','api.h','detail.h'):
            (project/name).write_bytes((self.path/name).read_bytes())
        (project/'raw.elf').write_bytes(fixture())
        (project/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\n'
            'project(sdk_test NONE)\nadd_custom_target(elf COMMAND "${CMAKE_COMMAND}" -E copy '
            '"${CMAKE_CURRENT_SOURCE_DIR}/raw.elf" input.elf)\n'
            f'include("{ROOT}/cmake/solar_os_native_sdk.cmake")\n'
            'solar_os_native_sdk(input.elf output.elf)\n')
        build=project/'build'
        subprocess.run(['cmake','-S',str(project),'-B',str(build)],check=True,stdout=subprocess.DEVNULL)
        for _ in range(2):
            subprocess.run(['cmake','--build',str(build)],check=True,stdout=subprocess.DEVNULL)
            self.assertEqual((build/'output.elf').read_bytes(),self.valid)

    def test_cmake_packages_headers_from_explicit_shared_root(self):
        project=self.path/'cmake-shared-project';project.mkdir()
        shared=self.path/'shared-headers';shared.mkdir()
        for name in ('api.h','detail.h'):
            (shared/name).write_bytes((self.path/name).read_bytes())
        (project/'module.toml').write_bytes(self.manifest.read_bytes())
        (project/'raw.elf').write_bytes(fixture())
        (project/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.16)\n'
            'project(shared_sdk_test NONE)\nadd_custom_target(elf COMMAND "${CMAKE_COMMAND}" -E copy '
            '"${CMAKE_CURRENT_SOURCE_DIR}/raw.elf" input.elf)\n'
            f'include("{ROOT}/cmake/solar_os_native_sdk.cmake")\n'
            f'solar_os_native_sdk(input.elf output.elf SOURCE_ROOT "{shared}")\n')
        build=project/'build'
        subprocess.run(['cmake','-S',str(project),'-B',str(build)],check=True,stdout=subprocess.DEVNULL)
        subprocess.run(['cmake','--build',str(build)],check=True,stdout=subprocess.DEVNULL)
        self.assertEqual((build/'output.elf').read_bytes(),self.valid)


if __name__=='__main__':unittest.main()
