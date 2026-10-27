import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]


def module(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'modules/espdl'/(name+'.py'))
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result);return result


FIX=module('fix_frames')
VERIFY=module('verify')
BUILD=module('build')


def fixture(encoding=0x1b):
    # One real DWARF CIE/FDE: relative PC 0xfe8 at 0x201c names code at 0x3004.
    cie=struct.pack('<II',16,0)+b'\x01zR\0\x01\x7c\x10\x01'+bytes([encoding])+bytes(3)
    fde=struct.pack('<IIII',16,24,0xfe8,16)+bytes(4)
    frames=cie+fde+bytes(4)
    strings=b'\0';symbols=bytes(16)
    for name,value in [('__module_eh_start',0x2000),('__module_eh_end',0x202c),
                       ('__module_lsda_start',0x202c),('__module_lsda_end',0x202c)]:
        index=len(strings);strings+=name.encode()+b'\0'
        symbols+=struct.pack('<IIIBBH',index,value,0,0,0,2)
    rows=[('',0,0,0,b'',0,0),('.debug_info',1,0,0,bytes(0x4000),0,0),
          ('.data.rel.ro',1,3,0x2000,frames,0,0),('.text',1,6,0x3000,bytes(32),0,0),
          ('.rela.dyn',4,2,0,b'',7,12),('.symtab',2,0,0,symbols,6,16),
          ('.strtab',3,0,0,strings,0,0),('.dynsym',11,2,0,bytes(16),8,16),
          ('.dynstr',3,2,0,b'\0',0,0)]
    names=b'\0';name_offsets=[0]
    for row in rows[1:]:name_offsets.append(len(names));names+=row[0].encode()+b'\0'
    name_offsets.append(len(names));names+=b'.shstrtab\0';rows.append(('.shstrtab',3,0,0,names,0,0))
    data=bytearray(52);sections=[]
    for index,(_,kind,flags,address,payload,link,entry) in enumerate(rows):
        data.extend(bytes((-len(data))%4));offset=len(data);data.extend(payload)
        sections.append((name_offsets[index],kind,flags,address,offset,len(payload),link,0,4,entry))
    data.extend(bytes((-len(data))%4));shoff=len(data)
    for section in sections:data.extend(struct.pack('<10I',*section))
    ident=b'\x7fELF\x01\x01\x01'+bytes(9)
    struct.pack_into('<16sHHIIIIIHHHHHH',data,0,ident,3,94,1,0x3000,0,shoff,0,52,0,0,40,len(sections),len(sections)-1)
    return data,sections,shoff


class EspdlLibraryToolsTest(unittest.TestCase):
    def sdk(self, root, sources):
        for name in ('esp-idf/espressif__esp-dl/libespressif__esp-dl.a',
                     'esp-idf/espdl_schema/libespdl_schema.a'):
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        entries = [{'file': source, 'command': 'xtensa-esp32s3-elf-g++ -c ' + source}
                   for source in sources]
        (root / 'compile_commands.json').write_text(json.dumps(entries))
        return entries

    def test_sdk_preparation_uses_private_full_backend_and_archive_targets(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'flavors').mkdir()
            original = (ROOT / 'flavors/full.toml').read_text()
            (root / 'flavors/full.toml').write_text(original)
            cached = root / '.pio/espdl-sdk/build/test-s3'
            def configure(command, **kwargs):
                if command[0] == '/sdk/cmake':
                    return types.SimpleNamespace(returncode=0)
                self.assertEqual(command[-2:], ['-t', 'envdump'])
                cached.mkdir(parents=True)
                (cached / 'CMakeCache.txt').write_text('CMAKE_COMMAND:INTERNAL=/sdk/cmake\n')
                return types.SimpleNamespace(returncode=0)
            lock = mock.Mock()
            with mock.patch.object(BUILD, 'ROOT', root), \
                 mock.patch.dict(os.environ, {'SOLAR_OS_FLAVOR': 'writerdeck',
                     'SOLAR_OS_BOARD': 'wrong-board', 'PLATFORMIO_BUILD_DIR': '/wrong-build',
                     'SOLAROS_PIO': sys.executable}), \
                 mock.patch.dict(sys.modules, {'solaros_build_lock':
                     types.SimpleNamespace(acquire_project_build_lock=lock)}), \
                 mock.patch.object(BUILD.subprocess, 'run', side_effect=configure) as run:
                self.assertEqual(BUILD.prepare_sdk('test-s3'), cached)
                config_env = run.call_args_list[0].kwargs['env']
                self.assertEqual(config_env['SOLAR_OS_FLAVOR'], 'full')
                self.assertNotIn('SOLAR_OS_BOARD', config_env)
                self.assertEqual(config_env['PLATFORMIO_BUILD_DIR'], str(cached.parent))
                self.assertEqual(run.call_args_list[1].args[0], ['/sdk/cmake', '--build',
                    str(cached), '--target', '__idf_espressif__esp-dl', '__idf_espdl_schema'])
                lock.assert_called_once_with(root, 'test-s3')
            self.assertEqual((root / 'flavors/full.toml').read_text(), original)
            self.assertIn('inference_backend = true', (root / '.pio/espdl-sdk/full.toml').read_text())

    def test_sdk_command_selection_accepts_adapter_without_fallback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            entries = self.sdk(root, ['solar_os_inference_espdl.cpp'])
            self.assertEqual(BUILD.load_sdk_commands(root), (entries, entries[0]))
            entries = self.sdk(root, ['dl_model_base.cpp'])
            self.assertEqual(BUILD.load_sdk_commands(root), (entries, entries[0]))

    def test_incomplete_or_external_only_sdk_has_actionable_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(ValueError, 'Omit --sdk-build-dir'):
                BUILD.load_sdk_commands(root)
            self.sdk(root, ['solar_os_inference_backend.c'])
            with self.assertRaisesRegex(ValueError, 'enable inference_backend'):
                BUILD.load_sdk_commands(root)

    def test_relative_frame_pc_becomes_a_mapped_absolute_relocation(self):
        data,sections,shoff=fixture()
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'library.elf';path.write_bytes(data);FIX.fix(path)
            fixed=path.read_bytes();section=struct.unpack_from('<10I',fixed,shoff+40*4)
            self.assertEqual(struct.unpack_from('<III',fixed,section[4]),(0x201c,5,0))
            self.assertEqual(fixed[sections[2][4]+16],3)
            self.assertEqual(struct.unpack_from('<I',fixed,sections[2][4]+28)[0],0x3004)
            self.assertEqual(VERIFY.verify(path,set()),[])
            FIX.fix(path)
            twice=path.read_bytes();self.assertEqual(struct.unpack_from('<I',twice,shoff+40*4+20)[0],12)

    def test_unsupported_encoding_and_truncated_elf_leave_input_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'library.elf'
            for data in (fixture(0x19)[0],bytearray(b'\x7fELF')):
                path.write_bytes(data)
                with self.assertRaises(ValueError):FIX.fix(path)
                self.assertEqual(path.read_bytes(),data)

    def test_unmapped_relocation_is_rejected_and_noops_are_removed(self):
        data,sections,shoff=fixture()
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/'library.elf';path.write_bytes(data);FIX.fix(path)
            fixed=bytearray(path.read_bytes());section=struct.unpack_from('<10I',fixed,shoff+40*4)
            struct.pack_into('<III',fixed,section[4],0xdeadbeec,5,0);path.write_bytes(fixed)
            with self.assertRaisesRegex(ValueError,'unmapped'):VERIFY.verify(path,set())
            struct.pack_into('<III',fixed,section[4],0,0,0);path.write_bytes(fixed)
            VERIFY.remove_noop_relocations(path)
            self.assertEqual(VERIFY.verify(path,set()),[])
