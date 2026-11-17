"""Exercise the portable writer without downloading/building the compiler."""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def object_file(alignment, merged_alignment=None):
    names = b"\0.text\0.data\0.bss\0.rodata\0.symtab\0.strtab\0.shstrtab\0"
    if merged_alignment is not None: names += b".data.rel.ro\0"
    data = bytearray(256)
    data[64:68] = b"\0" * 4
    data[80:96] = b"\x12" * 16
    data[96:112] = b"\x34" * 16
    # The writer needs one defined, aligned entry symbol; this fixture is never
    # executed, so it does not need instructions or a kernel ABI import.
    data[128:160] = bytes(16) + struct.pack("<IIIBBH", 1, 0, 4, 0x12, 0, 1)
    data[160:166] = b"\0main\0"
    data[176:176+len(names)] = names
    sections = [bytes(40)]
    for name, kind, flags, offset, size, link, info, align, entry in (
        (b".text", 1, 6, 64, 4, 0, 0, 4, 0),
        (b".data", 1, 3, 80, 16, 0, 0, alignment, 0),
        (b".bss", 8, 3, 112, 32, 0, 0, alignment, 0),
        (b".rodata", 1, 2, 96, 16, 0, 0, alignment, 0),
        (b".symtab", 2, 0, 128, 32, 6, 1, 4, 16),
        (b".strtab", 3, 0, 160, 6, 0, 0, 1, 0),
        (b".shstrtab", 3, 0, 176, len(names), 0, 0, 1, 0),
    ):
        sections.append(struct.pack("<10I", names.index(name), kind, flags, 0,
                                    offset, size, link, info, align, entry))
    if merged_alignment is not None:
        sections.append(struct.pack("<10I",names.index(b".data.rel.ro"),1,3,0,
                                    112,8,0,0,merged_alignment,0))
    ident = b"\x7fELF\1\1\1" + bytes(9)
    data[:52] = struct.pack("<16sHHIIIIIHHHHHH", ident, 1, 94, 1, 0, 0, 256, 0,
                            52, 0, 0, 40, len(sections), 7)
    return data + b"".join(sections)


class TinyCCModuleWriterTest(unittest.TestCase):
    def test_preserves_requested_section_alignment_and_rejects_unsupported(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            binary = folder / "writer"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-DSOLAR_TCC_MODULE_TOOL",
                str(ROOT / "modules/experimental/tinycc/module.c"), "-o", str(binary),
            ], check=True)
            source, output = folder / "input.o", folder / "output.elf"
            for alignment in (1, 2, 4, 8, 16):
                source.write_bytes(object_file(alignment))
                subprocess.run([str(binary), str(source), str(output), "main"], check=True)
                data = output.read_bytes()
                table = struct.unpack_from("<I", data, 32)[0]
                # Canonical output indices: rodata=2, data=3, bss=4.
                for index in (2, 3, 4):
                    section = struct.unpack_from("<10I", data, table+index*40)
                    self.assertGreaterEqual(section[8], alignment)
                    self.assertEqual(section[3] % alignment, 0)
                    self.assertEqual(section[4] % alignment, 0)
            for alignment, merged_alignment in ((4,16),(16,4)):
                source.write_bytes(object_file(alignment,merged_alignment))
                subprocess.run([str(binary),str(source),str(output),"main"],check=True)
                data=output.read_bytes(); table=struct.unpack_from("<I",data,32)[0]
                section=struct.unpack_from("<10I",data,table+3*40)
                self.assertEqual(section[8],16)
            source.write_bytes(object_file(32))
            result = subprocess.run([str(binary), str(source), str(output), "main"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("unsupported section flags or alignment", result.stderr)


if __name__ == "__main__":
    unittest.main()
