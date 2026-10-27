from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from tests.test_native_foreground import native_key_pulse_includes
ROOT = Path(__file__).resolve().parents[1]
class GameboyModuleTest(unittest.TestCase):
    def test_real_core_lifecycle_audio_save_and_failures(self):
        self.run_module_test(False)

    def test_board_navigation_through_shared_foreground_pulses(self):
        self.run_module_test(True)

    def run_module_test(self, pulses):
        with tempfile.TemporaryDirectory() as tmp:
            source_root=Path(tmp,"source")
            shutil.copytree(ROOT/"include",source_root/"include")
            shutil.copytree(ROOT/"modules/gameboy/main",source_root/"modules/gameboy/main")
            shutil.copyfile(ROOT/"tests/host/gameboy_module_test.c",source_root/"gameboy_module_test.c")
            if pulses:
                native_key_pulse_includes(source_root)
            objects=[]
            sources=[("modules/gameboy/main/gameboy.c", ["-Dmain=gameboy_module_main"]),
                     ("modules/gameboy/main/apu.c", []),
                     ("modules/gameboy/main/solar_os_gameboy_rom.c", []),
                     ("modules/gameboy/main/solar_os_gameboy_video.c", []),
                     ("gameboy_module_test.c", [])]
            for index,(source,extra) in enumerate(sources):
                obj=Path(tmp,str(index)+".o");objects.append(str(obj))
                subprocess.run(["cc","-std=c11","-O2","-Wall","-Wextra","-Werror",
                                "-I",str(source_root/"include"),"-I",str(source_root/"modules/gameboy/main"),
                                "-I",str(source_root),"-I",str(ROOT/"tests/host"),
                                *(["-DTEST_NATIVE_KEY_PULSES"] if pulses else []),
                                *extra,"-c",str(source_root/source),"-o",str(obj)],check=True)
            binary=Path(tmp,"gameboy")
            subprocess.run(["cc",*objects,"-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
