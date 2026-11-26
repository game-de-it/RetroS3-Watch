"""Host checks for the C validator shared by USB and Wi-Fi uploads."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import unicodedata
import unittest


class RomFilenameTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(__file__).resolve().parents[1]
        library = Path(cls.temp.name) / "filename.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                        str(root / "main/rom_filename.c"), "-o", str(library)], check=True)
        cls.lib = ctypes.CDLL(str(library))
        cls.lib.rom_filename_valid.argtypes = [ctypes.c_char_p]
        cls.lib.rom_filename_valid.restype = ctypes.c_bool
        cls.lib.rom_filename_decode_hex.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.rom_filename_decode_hex.restype = ctypes.c_bool
        cls.lib.rom_filename_display.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_normal_and_unicode_names(self):
        for name in ["usas.rom", "MSX2EXT.ROM", "game name.rom", "A+B.rom",
                     "おばけハロウィン.rom", "日本語のゲーム.ROM", "星の冒険 2.rom"]:
            with self.subTest(name=name):
                self.assertTrue(self.lib.rom_filename_valid(name.encode()))

    def test_traversal_and_invalid_utf8(self):
        for name in [b"../x.rom", b".hidden.rom", b"dir/x.rom", b"a\\x.rom", b"a?.rom",
                     b"a*.rom", b"a:.rom", b"a\n.rom", b"a.rom ", b"noext", b"", b"old.fdi",
                     b"\xc0\xaf.rom", b"\xed\xa0\x80.rom", b"\xf4\x90\x80\x80.rom",
                     b"\xe3\x81.rom", b"\x80.rom", b"x" * 129 + b".rom"]:
            with self.subTest(name=name):
                self.assertFalse(self.lib.rom_filename_valid(name))

    def test_hex_transport(self):
        name = "おばけ ゲーム.rom".encode()
        output = ctypes.create_string_buffer(129)
        self.assertTrue(self.lib.rom_filename_decode_hex(name.hex().encode(), output, 129))
        self.assertEqual(output.value, name)
        for encoded in [b"00", b"gg", b"123", b"612e726f6d0062", b"2e2e2f782e726f6d"]:
            self.assertFalse(self.lib.rom_filename_decode_hex(encoded, output, 129))

    def test_mac_kana_display_and_character_boundaries(self):
        original = unicodedata.normalize("NFD", "おばけハロウィン.rom").encode()
        output = ctypes.create_string_buffer(256)
        self.lib.rom_filename_display(original, output, 256)
        self.assertEqual(output.value.decode(), "おばけハロウィン.ROM")
        for capacity in range(1, 32):
            output = ctypes.create_string_buffer(capacity)
            self.lib.rom_filename_display(original, output, capacity)
            output.value.decode("utf-8")  # a cutoff must never leave half a character
            self.assertLess(len(output.value), capacity)


if __name__ == "__main__":
    unittest.main()
