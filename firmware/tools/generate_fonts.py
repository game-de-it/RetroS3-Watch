#!/usr/bin/env python3
"""Regenerate pinned 1-bit fonts; install ../toolchains/font-tools first."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
exe = root.parent / "toolchains/font-tools/node_modules/.bin/lv_font_conv"
jobs = [
    ("msx_ascii_32", 32, "msx-cbios-pixels.ttf", "--range", "0x20-0x7e"),
    ("msx_ascii_20", 20, "msx-cbios-pixels.ttf", "--range", "0x20-0x7e"),
    ("msx_clock_64", 64, "msx-cbios-pixels.ttf", "--symbols", "0123456789:-"),
    ("msx_japanese_32", 32, "DotGothic16-Regular.ttf", "--range",
     (root / "assets/fonts/japanese-ranges.txt").read_text().strip()),
]
for name, size, font, selector, characters in jobs:
    result = subprocess.run([
        str(exe), "--size", str(size), "--bpp", "1", "--no-compress", "--no-kerning",
        "--lv-include", "lvgl.h", "--format", "lvgl", "--font", "assets/fonts/" + font,
        "--autohint-off", selector, characters, "--lv-font-name", name,
        "-o", "main/fonts/" + name + ".c",
    ], cwd=root, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(result.stderr[-2000:])
    print("Generated", name)
