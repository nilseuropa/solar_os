"""Virtual flash persistence and build-and-run behavior for the QEMU targets."""

import json
import os
from pathlib import Path
import runpy
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import solaros_qemu as qemu


class FlashTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)
        self.boot = self.build / "bootloader.bin"
        self.app = self.build / "firmware.bin"
        self.boot.write_bytes(b"boot")
        self.app.write_bytes(b"application")
        self.images = [(0, self.boot), (0x10000, self.app)]

    def test_flash_layout_and_erased_padding(self):
        image = qemu.prepare_flash(self.build, self.images)
        self.assertEqual(image.stat().st_size, 16 * 1024 * 1024)
        with image.open("rb") as stream:
            self.assertEqual(stream.read(8), b"boot\xff\xff\xff\xff")
            stream.seek(0x10000)
            self.assertEqual(stream.read(12), b"application\xff")
            stream.seek(-4, 2)
            self.assertEqual(stream.read(), b"\xff" * 4)

    def test_restart_keeps_guest_written_data(self):
        image = qemu.prepare_flash(self.build, self.images)
        with image.open("r+b") as stream:
            stream.seek(0x9000)
            stream.write(b"saved settings")
        before = image.stat().st_mtime_ns
        self.assertEqual(qemu.prepare_flash(self.build, self.images), image)
        self.assertEqual(image.stat().st_mtime_ns, before)
        with image.open("rb") as stream:
            stream.seek(0x9000)
            self.assertEqual(stream.read(14), b"saved settings")

    def test_rebuild_replaces_firmware_and_erases_old_tail(self):
        image = qemu.prepare_flash(self.build, self.images)
        self.app.write_bytes(b"new")
        qemu.prepare_flash(self.build, self.images)
        with image.open("rb") as stream:
            stream.seek(0x10000)
            self.assertEqual(stream.read(12), b"new" + b"\xff" * 9)

    def test_bad_layout_does_not_replace_existing_flash(self):
        image = qemu.prepare_flash(self.build, self.images)
        before = image.stat().st_mtime_ns
        for offset in (2, -1, qemu.FLASH_BYTES):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                qemu.prepare_flash(self.build, [(0, self.boot), (offset, self.app)])
        self.assertEqual(image.stat().st_mtime_ns, before)


class InstallerTests(unittest.TestCase):
    def test_download_once_then_use_cached_tool(self):
        with tempfile.TemporaryDirectory() as directory:
            package = Path(directory)
            framework = package / "framework"
            manifest = {"tools": [{"name": "qemu-xtensa", "versions": [
                {"name": "esp_test", "status": "recommended"}]}]}
            (package / "tools.json").write_text(json.dumps(manifest))
            executable = package / "cache/tools/qemu-xtensa/esp_test/qemu/bin" / (
                "qemu-system-xtensa.exe" if os.name == "nt" else "qemu-system-xtensa")

            def install(command, **kwargs):
                self.assertEqual(command[1], str(framework / "tools/idf_tools.py"))
                self.assertEqual(command[-2:], ["install", "qemu-xtensa"])
                self.assertEqual(kwargs["env"]["IDF_TOOLS_PATH"], str(package / "cache"))
                self.assertTrue(kwargs["check"])
                executable.parent.mkdir(parents=True)
                executable.touch()

            with patch.object(qemu.subprocess, "run", side_effect=install) as run:
                self.assertEqual(qemu.ensure_qemu(package, framework), executable)
                self.assertEqual(qemu.ensure_qemu(package, framework), executable)
                run.assert_called_once()


class LauncherTests(unittest.TestCase):
    def test_audio_backends_are_explicit_output_devices(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            for backend in ("sdl", "none"):
                self.assertEqual(qemu.prepare_audio_backend(project, backend), backend)
                command = qemu.qemu_command(Path("qemu"), "qemu_esp32s3", project / "flash.bin", audio=backend)
                self.assertIn("solar-os-audio.audiodev=solaros-audio", command)
                self.assertTrue(command[command.index("-audiodev") + 1].startswith(backend + ","))
            capture = qemu.prepare_audio_backend(project, "wav:captures/with,comma.wav")
            self.assertEqual(capture, "wav:" + str(project / "captures/with,comma.wav"))
            command = qemu.qemu_command(Path("qemu"), "qemu_esp32s3", project / "flash.bin", audio=capture)
            self.assertIn("path=" + str(project / "captures/with,,comma.wav"), command[command.index("-audiodev") + 1])
            self.assertNotIn("-audiodev", qemu.qemu_command(Path("qemu"), "qemu_esp32s3", project / "flash.bin", audio="off"))
            for invalid in ("", "wav:", "unknown"):
                with self.assertRaises(ValueError): qemu.prepare_audio_backend(project, invalid)

    def test_host_storage_is_live_directory_outside_the_build_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            shared = qemu.prepare_host_storage(project, "qemu-storage")
            self.assertEqual(shared, project / "qemu-storage")
            (shared / "keep").write_text("saved")
            self.assertEqual(qemu.prepare_host_storage(project, "qemu-storage"), shared)
            self.assertEqual((shared / "keep").read_text(), "saved")
            for value in ("none", "off", ""):
                self.assertIsNone(qemu.prepare_host_storage(project, value))
            command = qemu.qemu_command(Path("qemu"), "qemu_esp32s3", project / "flash.bin", storage=project / "path with,comma")
            self.assertIn("solar-os-hostfs.root=" + str(project / "path with,,comma"), command)

    def test_headless_has_console_and_no_display_or_network(self):
        command = qemu.qemu_command(Path("qemu"), "qemu_esp32s3", Path("build/flash.bin"))
        self.assertEqual(command[command.index("-display") + 1], "none")
        self.assertEqual(command[command.index("-nic") + 1], "none")
        if qemu.DEFAULT_UART0_MODE == "pty":
            self.assertEqual(command[command.index("-monitor") + 1], "stdio")
        else:
            self.assertIn("chardev=console,mode=readline", command)
        self.assertIn("driver=ssi_psram,property=is_octal,value=true", command)

    def test_display_uses_only_the_console_uart_and_paths_are_single_arguments(self):
        binary = Path("tools with spaces/qemu")
        image = Path("build with spaces,and comma/flash.bin")
        command = qemu.qemu_command(binary, "qemu_esp32s3_rgb", image, uart0_mode="stdio")
        self.assertEqual(command[0], str(binary))
        self.assertIn("file=build with spaces,,and comma/flash.bin,if=mtd,format=raw", command)
        self.assertEqual(command.count("-serial"), 1)
        self.assertEqual(command[command.index("-serial") + 1], "chardev:console")
        self.assertIn("user,model=open_eth", command)
        self.assertEqual(command[command.index("-display") + 1], "sdl")
        self.assertIn("solar-os-input.absolute=on", command)

    @unittest.skipIf(sys.platform == "win32", "QEMU PTYs require a POSIX host")
    def test_virtual_uart_is_separate_from_the_qemu_monitor(self):
        image = Path("build with spaces,and comma/flash.bin")
        command = qemu.qemu_command(Path("qemu"), "qemu_esp32s3_rgb", image, uart0_mode="pty")
        self.assertIn("pty,id=uart0,path=build with spaces,,and comma/uart0,logfile=build with spaces,,and comma/uart0.log", command)
        self.assertEqual(command.count("-serial"), 1)
        self.assertEqual(command[command.index("-serial") + 1], "chardev:uart0")
        self.assertEqual(command[command.index("-monitor") + 1], "stdio")
        self.assertFalse(any("mux=on" in arg for arg in command))

    def test_invalid_uart_mode_is_rejected(self):
        with self.assertRaises(ValueError):
            qemu.qemu_command(Path("qemu"), "qemu_esp32s3", Path("flash.bin"), uart0_mode="unknown")

    @unittest.skipIf(sys.platform == "win32", "symlink handling on POSIX hosts")
    def test_uart_path_cleanup_preserves_existing_devices_and_files(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            port = build / "uart0"
            port.symlink_to(build / "missing")
            qemu.prepare_uart0(build, "pty")
            self.assertFalse(port.is_symlink())
            target = build / "existing"
            target.write_text("keep")
            port.symlink_to(target)
            with self.assertRaises(RuntimeError): qemu.prepare_uart0(build, "pty")
            self.assertTrue(port.is_symlink())
            port.unlink()
            port.write_text("keep")
            with self.assertRaises(RuntimeError): qemu.prepare_uart0(build, "pty")
            self.assertEqual(port.read_text(), "keep")

    def test_relative_mouse_mode(self):
        command = qemu.qemu_command(Path("qemu"), "qemu_esp32s3_rgb", Path("flash.bin"), "mouse")
        self.assertIn("solar-os-input.absolute=off", command)
        with self.assertRaises(ValueError):
            qemu.qemu_command(Path("qemu"), "qemu_esp32s3_rgb", Path("flash.bin"), "invalid")

    def test_physical_boards_are_rejected(self):
        with self.assertRaises(ValueError):
            qemu.qemu_command(Path("qemu"), "solar_term", Path("flash.bin"))

    def test_target_builds_binary_before_launching(self):
        class Environment:
            def subst(self, value):
                return str(ROOT) if value == "$PROJECT_DIR" else value

            def AddCustomTarget(self, **kwargs):
                self.target = kwargs

        env = Environment()
        runpy.run_path(str(ROOT / "scripts/platformio_qemu.py"),
                       init_globals={"env": env, "Import": lambda name: None})
        self.assertEqual(env.target["name"], "qemu")
        self.assertEqual(env.target["dependencies"], ["$BUILD_DIR/${PROGNAME}.bin"])
        self.assertEqual(len(env.target["actions"]), 1)


if __name__ == "__main__":
    unittest.main()
