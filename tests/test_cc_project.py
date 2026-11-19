from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

from test_native_sdk import pack, fixture

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / 'modules/experimental/tinycc'


class CcProjectTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.folder = Path(cls.temp.name)
        cls.binary = cls.folder / 'project'
        subprocess.run(['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
            '-I'+str(ROOT/'include'), '-I'+str(HERE), '-I'+str(HERE/'include'),
            str(ROOT/'tests/host/cc_project_test.c'), str(HERE/'project.c'), str(HERE/'sdk_reader.c'),
            '-o', str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_tool(self, *args):
        return subprocess.check_output([str(self.binary), *map(str,args)], text=True, timeout=10).splitlines()

    def parse(self, text):
        path = self.folder/'input.toml'
        path.write_bytes(text.encode() if isinstance(text,str) else text)
        return self.run_tool('parse', path)

    def test_supported_toml_matches_host_parser(self):
        samples = [
            'sources=["main.c"]',
            "# project\r\nsources = [\r\n 'src/main.c', # entry\r\n \"lib/other.c\",\r\n]\r\n"
            "include_dirs=['include', 'headers with spaces']\r\noutput='build/app.elf'\r\n"
            "dependencies=[{version='1.12.0', id='jet'},]\r\n",
            'sources=["space \\" quote.c", \'literal\\backslash.c\']\ninclude_dirs=[]\ndependencies=[]\n',
            "sources=['café.c']\noutput='/modules/apps/demo.elf'\n",
        ]
        for text in samples:
            with self.subTest(text=text):
                expected = tomllib.loads(text)
                rows = self.parse(text)
                self.assertEqual(rows[0], '0', rows)
                self.assertEqual(rows[1], 'output\t'+expected.get('output','a.elf'))
                self.assertEqual([r[7:] for r in rows if r.startswith('source\t')], expected['sources'])
                self.assertEqual([r[8:] for r in rows if r.startswith('include\t')], expected.get('include_dirs',[]))
                self.assertEqual([r.split('\t')[1:] for r in rows if r.startswith('dependency\t')],
                    [[d['id'],d['version']] for d in expected.get('dependencies',[])])

    def test_rejects_typos_duplicates_and_incomplete_values(self):
        invalid = [
            '', '# empty', "sources=[]", "source=['main.c']", "sources='main.c'",
            "sources=['main.c']\nsources=['other.c']", "sources=['']", "sources=[main.c]",
            "sources=['main.c'", "sources=['main.c',,]", "sources=['main.c'] garbage",
            "sources=['main.c'] output='a.elf'", "[build]\nsources=['main.c']",
            "sources=['main.c']\noutput=42", "sources=['main.c']\noutput='a\nb'",
            "sources=['main.c']\routput='x'", 'sources=["\\u0061.c"]',
            "sources=['''main.c''']", "sources=['main.c']\ndependencies={}",
            "sources=['main.c']\ndependencies=[{id='jet'}]",
            "sources=['main.c']\ndependencies=[{id='jet',id='jet',version='1'}]",
            "sources=['main.c']\ndependencies=[{id='jet',version='1',optional='yes'}]",
            "sources=['main.c']\ndependencies=[{id='jet',version='1'},{id='jet',version='2'}]",
            "sources=['main.c']\ndependencies=[{id='../jet',version='1'}]",
            "sources=['main.c']\ndependencies=[{id='jet',version='>=1'}]",
            "sources=['main.c']\ndependencies=[{id='jet',\nversion='1'}]",
            "sources=['main.c']\ndependencies=[{id='jet',version='1',}]",
            b"sources=['main.c']\x00", b"sources=['bad\x01name.c']",
        ]
        for text in invalid:
            with self.subTest(text=text):
                rows = self.parse(text)
                self.assertNotEqual(rows[0],'0', rows)
                self.assertTrue(rows[1],rows)
        self.assertIn('line 2:',self.parse("sources=['main.c']\ninclude_dir=[]")[1])

    def test_resource_bounds(self):
        self.assertEqual(self.parse('sources=['+','.join(f'"s{i}.c"' for i in range(8))+']')[0],'0')
        for text in (
            'sources=['+','.join(f'"s{i}.c"' for i in range(9))+']',
            "sources=['x']\ninclude_dirs=["+','.join(f'"d{i}"' for i in range(9))+']',
            "sources=['x']\ndependencies=["+','.join(f'{{id="lib{i}",version="1"}}' for i in range(9))+']',
            "sources=['"+'s'*160+"']",
            "sources=['x']\ndependencies=[{id='"+'x'*48+"',version='1'}]",
            "sources=['x']\ndependencies=[{id='x',version='"+'1'*32+"'}]",
            "sources=['x']\n#"+' '*8192,
        ):
            with self.subTest(text=text[:80]):self.assertNotEqual(self.parse(text)[0],'0')
        text = "sources=['main.c']\n#"
        self.assertEqual(self.parse(text+' '*(8192-len(text)))[0],'0')

    def load(self, root, manifest='/work/cc.toml', mode='normal'):
        return self.run_tool('load',root,manifest,mode)

    def test_manifest_relative_paths_and_output_protection(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);work=root/'work';work.mkdir();manifest=work/'cc.toml'
            manifest.write_text("sources=['src/../main.c','src/helper.c']\ninclude_dirs=['include']\noutput='build/app.elf'\n")
            rows=self.load(root)
            self.assertEqual(rows,['0','output\t/work/build/app.elf','source\t/work/main.c','source\t/work/src/helper.c','include\t/work/include'])
            manifest.write_text("sources=['/shared/main.c']")
            self.assertEqual(self.load(root)[1:3],['output\t/work/a.elf','source\t/shared/main.c'])
            for text, message in (
                ("sources=['main.c']\noutput='./CC.TOML'",'project file'),
                ("sources=['src/../Main.c']\noutput='main.c'",'every source'),
                ("sources=['main.c','./MAIN.C']",'duplicate source'),
                ("sources=['main.c']\ninclude_dirs=['include','./INCLUDE']",'duplicate include'),
            ):
                manifest.write_text(text);rows=self.load(root)
                self.assertNotEqual(rows[0],'0');self.assertIn(message,rows[1])
            manifest.write_text("sources=['main.c']")
            for mode,code in (('oom',-4),('short',-10),('read-fail',-1),('resolve-fail',-10),('cancel',-7)):
                with self.subTest(mode=mode):self.assertEqual(self.load(root,mode=mode)[0],str(code))
            self.assertEqual(self.load(root,'missing.toml')[0],'-5')
            self.assertFalse((work/'build').exists())

    def test_exact_dependency_versions_and_sdk_failures(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'work').mkdir();lib=root/'modules/libraries';lib.mkdir(parents=True)
            (root/'work/cc.toml').write_text("sources=['main.c']\ndependencies=[{id='testlib',version='1.2.3'}]")
            rows=self.load(root);self.assertEqual(rows[0],'-5');self.assertIn('pkg install testlib','\n'.join(rows))
            module=root/'module.toml';(root/'api.h').write_text('#define ANSWER 42\n')
            module.write_text('type="library"\nid="testlib"\nversion="1.2.3"\n[sdk.headers]\n"api.h"="api.h"\n')
            good=pack.pack_elf(fixture(),module);artifact=lib/'testlib.elf';artifact.write_bytes(good)
            self.assertEqual(self.load(root)[0],'0')
            self.assertEqual(self.load(root,mode='clock-cancel')[0],'-7')
            cancelled=self.load(root,mode='cancel-sdk')
            self.assertEqual(cancelled[0],'-7')
            self.assertIn('cancelled',cancelled[1])
            (root/'work/cc.toml').write_text("sources=['main.c']\ndependencies=[{id='testlib',version='2.0'}]")
            rows=self.load(root);self.assertEqual(rows[0],'-6');self.assertIn('installed 1.2.3','\n'.join(rows))
            for data in (fixture(),good[:-1],bytes(256),good[:256]+b'broken'+good[262:]):
                artifact.write_bytes(data);rows=self.load(root)
                self.assertNotEqual(rows[0],'0');self.assertIn('no valid SDK metadata','\n'.join(rows))

    def test_bounded_parser_mutations_and_truncation(self):
        self.assertIn('12000',self.run_tool('fuzz')[0])


if __name__ == '__main__':
    unittest.main()
