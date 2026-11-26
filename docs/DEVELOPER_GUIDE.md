# Developer guide

This guide covers source layout, reproducible builds, tests, device validation
and preparation of a public RetroS3 Watch release.

[日本語版](DEVELOPER_GUIDE.ja.md)

See [RetroS3 MSX](INDEPENDENT_MSX.md) for the emulator architecture, host
build and current compatibility.

## License boundary

Original project code, including the RetroS3 MSX core, and documentation
are MIT-licensed. C-BIOS and other dependencies retain their respective
licenses. See the [third-party notices](../THIRD_PARTY_LICENSES.md).

The product name is RetroS3 Watch. Use MSX/MSX2 only to describe compatibility,
do not use the MSX logo, and retain the trademark and non-affiliation notice in
the README.

## Supported hardware and toolchain

- Waveshare ESP32-S3-Touch-AMOLED-1.8 V2
- ESP32-S3 with 16 MiB flash and 8 MiB octal PSRAM
- ESP-IDF 5.5.5
- Python 3 and a C compiler for host tests
- DIO flash mode; QIO is not supported by the validated board configuration

Other boards and ESP-IDF releases may work, but are outside the validated
release path.

## Repository layout

```text
components/                 local board and USB-recovery components
docs/                       English and Japanese manuals
firmware/                   ESP-IDF application
  assets/fonts/             font sources and licenses
  components/retros3_msx/   MSX core and headless diagnostic tool
  main/                     launcher, emulator bridge, display, input and Wi-Fi
  patches/                  managed-component patches
  tests/                    host-side unit tests
  tools/                    build, upload, Wi-Fi and diagnostic helpers
licenses/                   third-party notices
release/flash/              prebuilt flash images and checksums
sdcard/msx/bios/            redistributable C-BIOS files
```

Games, proprietary BIOS files, disk/cassette images, save states, credentials,
captures and local build trees do not belong in the repository.

## Build from source

Install ESP-IDF 5.5.5 using Espressif's instructions and activate it in the
current shell. From the repository root:

```sh
cd firmware
idf.py reconfigure
python3 tools/restore_display_patch.py
idf.py build
```

`idf.py reconfigure` resolves managed components. The patch helper then applies
the required LCD-panel change to the resolved component. Run the helper again
after deleting or refreshing `managed_components/`.

The generated `firmware/dependencies.lock` can contain an absolute path when a
local component override is resolved. It is intentionally ignored and must not
be published. Review any dependency updates before release.

### Flash a development build

Replace `PORT` with the device serial port:

```sh
idf.py -p PORT flash monitor
```

The release layout flashes at these offsets:

```text
0x0000  bootloader.bin
0x8000  partition-table.bin
0x10000 application binary
```

Keep DIO mode when producing or writing a release image.

## Host tests

Run the source-level tests from `firmware/`:

```sh
python3 -m unittest discover -s tests -v
```

These tests cover render accounting, dirty-region logic, scaling, filename
handling and controller behavior. Run the separate [MSX core tests](INDEPENDENT_MSX.md#host-build)
for CPU, VDP, media and state persistence.

## Font generation

The checked-in generated fonts make Node.js unnecessary for ordinary builds.
To regenerate them, install Node.js and `lv_font_conv` 1.5.3 at:

```text
toolchains/font-tools/node_modules/
```

Then run:

```sh
python3 tools/generate_fonts.py
```

Review generated source and confirm menu/keyboard glyphs on the AMOLED before
committing changes.

## Runtime architecture

- `app_main.c` owns the watch launcher and high-level application state.
- `main/msx_runner.cpp` connects the watch to `components/retros3_msx/`.
- MSX1 uses 64 KiB of main RAM. MSX2 and MSX2+ use 256 KiB. The core allocates
  128 KiB of VRAM.
- `msx_video_diff.c` finds changed regions; the LCD path sends aligned,
  even-sized chunks and presents at full rate by default.
- The menu and software keyboard use partial redraws so controller input is not
  delayed by unnecessary full-screen transfers.
- `controller.c` handles the validated 8BitDo Micro K-mode path.
- `wifi_uploader.c` provides the local HTTP upload interface after credentials
  have been provisioned over USB.
- Save states are stored on microSD as `.rss` files. Load them with the
  same model, BIOS and media. v0.1.0 used a different format.

The source default is 100% render scheduling. Do not lower it in a release
without documenting the changed performance target.

## C-BIOS packaging

The included C-BIOS 0.29 files are renamed to match the firmware's expected
paths:

| Firmware path | C-BIOS upstream file |
| --- | --- |
| `sdcard/msx/bios/MSX.ROM` | `cbios_main_msx1.rom` |
| `sdcard/msx/bios/MSX2.ROM` | `cbios_main_msx2.rom` |
| `sdcard/msx/bios/MSX2EXT.ROM` | `cbios_sub.rom` |
| `sdcard/msx/bios/CBIOS2.ROM` | `cbios_logo_msx2.rom` |
| `sdcard/msx/bios/MSX2P.ROM` | `cbios_main_msx2+.rom` |
| `sdcard/msx/bios/MSX2PEXT.ROM` | `cbios_sub.rom` |
| `sdcard/msx/bios/CBIOS2P.ROM` | `cbios_logo_msx2+.rom` |

C-BIOS enables a redistributable cartridge-oriented startup path. It does not
provide full disk, cassette or BASIC compatibility. Never add proprietary BIOS
or DiskROM files to a release.

## Physical-device acceptance

A successful compile or visible frame is not release acceptance. Test these
routes separately on the supported board:

1. Cold boot and touch launcher.
2. Cartridge launch with bundled C-BIOS.
3. MSX1, MSX2 and MSX2+ display motion at the 100% render setting.
4. Audio continuity during motion.
5. Controller connection and reconnection.
6. SELECT emulator menu, START software keyboard and L2 Shift layer.
7. Save and load using a newly created state slot.
8. Disk boot and disk swapping with user-supplied legal media and BIOS.
9. Cassette loading with user-supplied legal media and a compatible BIOS.
10. USB Wi-Fi provisioning and HTTP file upload.
11. Exit from emulation back to the launcher and reboot recovery.

Record the exact build, device conditions and unresolved routes. Do not infer
untested routes from a successful one.

## Public release procedure

1. Start from the public distribution tree.
2. Run host tests and a clean ESP-IDF build.
3. Complete the physical-device acceptance matrix.
4. Copy the three flash images into `release/flash/` and generate
   `SHA256SUMS` from inside that directory.
5. Verify the seven C-BIOS files against `sdcard/msx/bios/SHA256SUMS`.
6. Review `LICENSE`, `THIRD_PARTY_LICENSES.md` and all upstream notices.
7. Scan the files about to be committed for private paths, usernames, network
   credentials, media and generated dependency locks.
8. Package the flash images, `sdcard/` files, guides, Wi-Fi setup tool and
   license notices. Verify the ZIP and publish it on the new release tag.

Example pre-commit checks from the repository root:

```sh
git diff --check
git ls-files | rg -i '\.(rom|dsk|fdi|cas|sta|state|sav|rss)$'
git grep -nF "$HOME"
git grep -nE 'password|ssid'
git log --format=fuller
```

The media-file list must contain only the seven intentional C-BIOS ROMs. Inspect
every credential match manually; generic field names are expected, actual
values are not. Review author names and email addresses before pushing.

Do not publish `firmware/build/`, `firmware/managed_components/`, `sdkconfig`,
`dependencies.lock`, serial logs, screenshots, states or development ROM
collections. Release archives should be produced from tracked files only.

