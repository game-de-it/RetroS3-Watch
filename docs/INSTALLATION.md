# Installation guide

This guide installs the prebuilt RetroS3 Watch firmware on a Waveshare
ESP32-S3-Touch-AMOLED-1.8 V2 and prepares a microSD card with C-BIOS.

## What you need

- Waveshare ESP32-S3-Touch-AMOLED-1.8 V2
- USB-C data cable
- FAT32-formatted microSD card
- Python 3 and `esptool`
- Optional: 8BitDo Micro controller in K mode

Install the flashing tool:

```sh
python3 -m pip install 'esptool==4.12.0'
```

## 1. Prepare the microSD card

Extract the [release ZIP](https://github.com/game-de-it/RetroS3-Watch/releases/latest).
Copy the contents of `sdcard/` to the root of a FAT32 microSD card:

```text
msx/
  bios/
    MSX.ROM
    MSX2.ROM
    MSX2EXT.ROM
    CBIOS2.ROM
    MSX2P.ROM
    MSX2PEXT.ROM
    CBIOS2P.ROM
  roms/
  states/
```

These BIOS files are C-BIOS 0.29. They let many cartridge ROM programs start
without a proprietary MSX BIOS. C-BIOS does not provide complete disk,
cassette or BASIC support.

Add only game media you are legally entitled to use to `msx/roms`. Supported
media extensions are `.rom`, read-only `.dsk`, and `.cas`.
If upgrading, back up the card first. Keep any BIOS files you supplied and
add the new C-BIOS files you need.

Insert the card before launching MSX software.

## 2. Enter download mode

If RetroS3 Watch firmware is already running, connect its USB serial port and send
the single character `d`; the watch restarts into the ESP32-S3 ROM downloader.

For an unprogrammed or non-running board, hold BOOT while powering it on or
resetting it, then release BOOT. If the device is battery-powered, unplugging
USB alone may leave it running.

Find the serial port:

- macOS: `/dev/cu.usbmodem...`
- Linux: `/dev/ttyACM...`
- Windows: `COM...`

## 3. Flash the prebuilt release

From the extracted release root or the repository root, replace `PORT` with
the actual serial port. The SHA-256 file alongside the binaries can be
checked with `shasum -a 256 -c SHA256SUMS` in `release/flash/`.

```sh
python3 -m esptool --chip esp32s3 -p PORT -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 release/flash/bootloader.bin \
  0x8000 release/flash/partition-table.bin \
  0x10000 release/flash/retros3_watch.bin
```

Use DIO mode. QIO is not supported by the validated board configuration.
Successful output ends with a verified hash and a hard reset.

## 4. First boot

After reset, the clock appears. Press the physical BOOT button to open the
launcher. Confirm that the ROM browser opens and lists media from
`/msx/roms`.

The default machine is MSX2. Tap `MODEL` in the media list to cycle through
MSX1, MSX2, and MSX2+.

## 5. Optional Wi-Fi upload setup

Install `pyserial`:

```sh
python3 -m pip install pyserial
```

Store the Wi-Fi credentials over USB without putting the password in shell
history:

```sh
python3 firmware/tools/configure_wifi.py --port PORT
```

Open `Wi-Fi UPLOAD` in the launcher. The watch displays its LAN address. Open
that HTTP address in a browser, select `Game ROM` or `BIOS ROM`, and upload one
file at a time. The watch writes to microSD and verifies the result with
SHA-256 before installing it.

Wi-Fi is station-only and requires a reachable 2.4 GHz network.

## Proprietary BIOS files

RetroS3 MSX looks for these filenames under `msx/bios`:

| Model | Main BIOS | Extension |
| --- | --- | --- |
| MSX1 | `MSX.ROM` | — |
| MSX2 | `MSX2.ROM` | `MSX2EXT.ROM` |
| MSX2+ | `MSX2P.ROM` | `MSX2PEXT.ROM` |

Use compatible, legally obtained system ROMs for cassette and BASIC
software. Disk images also require a compatible Sony/Philips WD2793
`DISK.ROM`. A backup lets you restore your C-BIOS setup later.

## Recovery

If the display stays black after flashing:

1. Re-enter ROM download mode.
2. Confirm that the command used DIO, 16 MB and the three documented offsets.
3. Flash all three files again.
4. Disconnect and reconnect USB power.

The repository does not contain a factory backup. Preserve your own device
backup before experimenting with partition layouts or bootloaders.

