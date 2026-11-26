#!/usr/bin/env python3
"""Restore the watch's pinned esp_lvgl_port 2.9.0 display extension safely."""
import hashlib
import json
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    saved = root / "patches/esp_lvgl_port-2.9.0"
    target = root / "managed_components/espressif__esp_lvgl_port"
    manifest = json.loads((saved / "manifest.json").read_text())
    pending = []
    for name, hashes in manifest.items():
        source = (saved / name).read_bytes()
        if hashlib.sha256(source).hexdigest() != hashes["patched_sha256"]:
            raise SystemExit(f"Saved patch checksum mismatch: {name}")
        destination = target / name
        if not destination.is_file():
            raise SystemExit("Resolve dependencies with idf.py reconfigure first.")
        current = hashlib.sha256(destination.read_bytes()).hexdigest()
        if current not in hashes.values():
            raise SystemExit(f"Refusing to overwrite unknown component changes: {name}")
        if current != hashes["patched_sha256"]:
            pending.append((destination, source))
    # Validate every file before changing any of them.
    for destination, source in pending:
        destination.write_bytes(source)
    print(f"Display extension verified; restored {len(pending)} file(s).")


if __name__ == "__main__":
    main()
