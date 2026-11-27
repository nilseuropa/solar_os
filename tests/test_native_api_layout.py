import copy
import json
from pathlib import Path
import sys
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import check_native_layout as layout


class NativeApiLayoutTest(unittest.TestCase):
    def test_layout_baseline_tracks_every_public_field(self):
        _,checks,tables=layout.specification(ROOT)
        baseline=json.loads((ROOT/layout.BASELINE).read_text())
        self.assertEqual(set(baseline['values']),{name for name,_ in checks})
        self.assertEqual(baseline['tables'],tables)

    def test_only_new_types_and_table_tails_are_compatible(self):
        old={'target':'test','tables':['api'],'values':{'sizeof(api)':8,'alignof(api)':4,'offsetof(api,call)':4,'sizeof(record)':16,'offsetof(record,field)':8}}
        new=copy.deepcopy(old);new['values'].update({'sizeof(api)':12,'offsetof(api,extra)':8,'sizeof(new_record)':24})
        self.assertFalse(layout.compatibility_errors(old,new))
        for key,value in [('alignof(api)',8),('offsetof(api,call)',8),('sizeof(record)',24),('offsetof(record,field)',4)]:
            changed=copy.deepcopy(new);changed['values'][key]=value
            self.assertTrue(layout.compatibility_errors(old,changed),key)

    def test_size_and_alignment_field_names_do_not_hide_record_layout(self):
        _,checks,_=layout.specification(ROOT)
        self.assertEqual(len(checks),len({name for name,_ in checks}))
        self.assertIn(('offsetof(solar_os_native_stream_frame_v1_t,size)',
            'offsetof(solar_os_native_stream_frame_v1_t,size)'),checks)

    def test_nested_records_arrays_and_callbacks_are_included(self):
        self.assertEqual(layout.members('uint32_t a,b; void (*call)(void); struct { int x,y; } item[2]; union { int n; float f; };'),
            ['a','b','call','item','item[0].x','item[0].y','n','f'])

    @unittest.skipUnless((Path.home()/'.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc').is_file()
        and (ROOT/'.pio/tinycc/solar-tcc').is_file(),'firmware toolchain and OS C frontend required')
    def test_real_target_compilers_agree_with_released_layout(self):
        layout.main([])
