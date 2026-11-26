#!/usr/bin/env python3
import argparse
import hashlib
import pathlib
import sys
import time
import unicodedata

import serial


def read_until(port: serial.Serial, marker: str, timeout: float = 10.0) -> str:
    deadline = time.monotonic() + timeout
    transcript = bytearray()
    encoded = marker.encode()
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            transcript.extend(chunk)
            marker_offset = transcript.find(encoded)
            if marker_offset >= 0 and b"\n" in transcript[marker_offset:]:
                return transcript.decode(errors="replace")
    raise TimeoutError(f"timed out waiting for {marker!r}; received {transcript[-500:]!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload one MSX media file or BIOS to RetroS3 Watch microSD")
    parser.add_argument("rom", type=pathlib.Path)
    parser.add_argument("--port", required=True, help="Serial port, for example /dev/cu.usbmodemXXXX or COM3")
    parser.add_argument("--chunk", type=int, default=512)
    parser.add_argument("--kind", choices=("rom", "bios"), default="rom")
    parser.add_argument("--name", help="destination filename (for example MSX.ROM)")
    args = parser.parse_args()

    payload = args.rom.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if not (1 <= args.chunk <= 512):
        parser.error("--chunk must be between 1 and 512")
    if args.rom.suffix.lower() not in (".rom", ".dsk", ".cas"):
        parser.error("input must have a .rom, .dsk, or .cas extension")

    with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=5) as port:
        port.reset_input_buffer()
        destination_name = unicodedata.normalize("NFC", args.name or args.rom.name)
        if len(destination_name.encode("utf-8")) > 128:
            parser.error("destination filename must fit in 128 UTF-8 bytes")
        command = f"upload-{args.kind}-utf8 {destination_name.encode('utf-8').hex()} {len(payload)} {digest}\n"
        port.write(command.encode())
        transcript = read_until(port, "UPLOAD READY")
        sys.stdout.write(transcript)
        for offset in range(0, len(payload), args.chunk):
            block = payload[offset : offset + args.chunk]
            port.write(b"D" + block.hex().encode() + b"\n")
            expected = f"UPLOAD ACK {offset + len(block)}"
            transcript = read_until(port, expected)
            if "UPLOAD ERROR" in transcript:
                raise RuntimeError(transcript)
            if offset == 0 or offset + len(block) == len(payload) or offset % (16 * args.chunk) == 0:
                print(f"{offset + len(block)}/{len(payload)} bytes")
        port.write(b"END\n")
        transcript = read_until(port, "UPLOAD OK", timeout=15.0)
        sys.stdout.write(transcript)
    print(f"verified SHA-256 {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
