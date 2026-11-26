# RetroS3 MSX independent core

[日本語](INDEPENDENT_MSX.ja.md)

RetroS3 MSX is an original, MIT-licensed implementation of the Z80, machine
bus, VDP, PSG, and media devices using the hardware references linked in the
Japanese guide. RetroS3 Watch includes this component in the standard
ESP32-S3 build. C-BIOS and other dependencies retain their own licenses.

## Host build

Run from the repository root.

```sh
cmake -S firmware/components/retros3_msx -B firmware/build-msx-host -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/build-msx-host
ctest --test-dir firmware/build-msx-host --output-on-failure
firmware/build-msx-host/retros3-msx \
  --model 3 --bios sdcard/msx/bios/MSX2P.ROM --sub sdcard/msx/bios/MSX2PEXT.ROM \
  --rom game.rom --mapper linear --frames 300 --ppm frame.ppm --save game.rss
```

Models 1, 2, and 3 select MSX1, MSX2, and MSX2+. Choose the cartridge
mapper with `--mapper linear|ascii8|ascii16|konami|scc`. Use
`--load game.rss` with the same machine and mounted images to resume. This
is a headless diagnostic tool with RGB screenshot output. Run `--help` for
the input, tape, and disk options.

## Watch build

Activate ESP-IDF 5.5.5, then run the following.

```sh
cd firmware
idf.py reconfigure
python3 tools/restore_display_patch.py
idf.py build
```

Copy the contents of `sdcard/` to the microSD root. Choose the machine with
the Model button in the media browser, then open an image under `msx/roms`.
SELECT opens the menu, START opens the keyboard, and L2 toggles Shift.
The menu provides three state slots, drive A/B image changes, and tape
rewind. Load an `.rss` state with the same model, BIOS and media mounted.
Save states from v0.1.0 use an incompatible format.

| Machine | Main BIOS | Sub-ROM | Optional C-BIOS logo |
| --- | --- | --- | --- |
| MSX1 | `MSX.ROM` | — | `CBIOS1.ROM` |
| MSX2 | `MSX2.ROM` | `MSX2EXT.ROM` | `CBIOS2.ROM` |
| MSX2+ | `MSX2P.ROM` | `MSX2PEXT.ROM` | `CBIOS2P.ROM` |

- **Cartridge**. The supplied C-BIOS can boot cartridges. If the watch
  guesses an incorrect mapper, place `game.rom.mapper` next to the ROM and
  put the mapper name in the file.
- **Cassette**. CAS playback requires a suitable system BIOS with tape
  support. Type the program's BASIC load command. The signal is played at
  1200 baud, so large tapes can take several minutes.
- **Disk**. Standard 512-byte sector DSK images are read-only. Supply a system
  BIOS and a Sony/Philips WD2793-compatible `DISK.ROM`.

Host verification includes C-BIOS for all three models, hardware BIOS boot
on MSX2 and MSX2+, Ys II boot and data disk swapping, and the OBAKE HALLOWEEN
cassette boot. The watch launched USAS, saved and restored a state on
microSD, and reached the Ys II copyright screen from its boot disk.
Data disk switching and complete CAS loading were verified on the host.
With rendering set to 100%, the USAS demo averaged about 59.4 fps over a
steady 10-second interval on the watch. Longer frame intervals occurred
during startup and scene changes. The watch menu shows the full drive
labels and scrolls the selected long disk name.
FDI, disk and tape writing, FM sound, interlacing, and cycle-exact VDP
timing are outside the current implementation.
Game images and proprietary firmware are user-supplied.
