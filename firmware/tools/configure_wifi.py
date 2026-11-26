#!/usr/bin/env python3
import argparse
import getpass
import sys
import time

import serial


def wait_for_result(port: serial.Serial, timeout: float = 10.0) -> str:
    deadline = time.monotonic() + timeout
    transcript = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            transcript.extend(chunk)
            if b"WIFI CONFIG OK" in transcript or b"WIFI CONFIG ERROR" in transcript:
                return transcript.decode(errors="replace")
    raise TimeoutError(f"timed out waiting for the watch; received {transcript[-500:]!r}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Store Wi-Fi credentials on RetroS3 Watch over USB without logging the password"
    )
    parser.add_argument("--port", required=True, help="Serial port, for example /dev/cu.usbmodemXXXX or COM3")
    args = parser.parse_args()

    ssid = input("Wi-Fi SSID: ")
    password = getpass.getpass("Wi-Fi password (leave blank for an open network): ")
    ssid_bytes = ssid.encode("utf-8")
    password_bytes = password.encode("utf-8")
    if not 1 <= len(ssid_bytes) <= 32:
        raise SystemExit("SSID must be 1 to 32 bytes in UTF-8")
    if len(password_bytes) > 64 or (password_bytes and len(password_bytes) < 8):
        raise SystemExit("Password must be empty or 8 to 64 bytes in UTF-8")

    password_token = password_bytes.hex() or "-"
    command = f"wifi-config {ssid_bytes.hex()} {password_token}\n".encode()
    with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=5) as port:
        port.reset_input_buffer()
        port.write(command)
        transcript = wait_for_result(port)
    if "WIFI CONFIG OK" not in transcript:
        sys.stderr.write(transcript)
        return 1
    print("Wi-Fi credentials stored on RetroS3 Watch.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

