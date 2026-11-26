# Third-party licenses / サードパーティライセンス

The root MIT license covers the original RetroS3 Watch code and documentation,
including the RetroS3 MSX emulator. Each third-party component retains its
own license.

ルートのMITライセンスは、RetroS3 MSXを含むRetroS3 Watch独自のコードと文書に
適用されます。サードパーティ製の構成要素には、それぞれのライセンスが適用されます。

## C-BIOS 0.29

- Included files: `sdcard/msx/bios/`.
- License: BSD-style 2-clause license permitting source and binary
  redistribution when the copyright notice, conditions and disclaimer are
  retained.
- Full notice: `licenses/C-BIOS-0.29.txt`.
- Upstream: <https://cbios.sourceforge.net/>

C-BIOS is included so cartridge software can start without proprietary MSX
BIOS files. C-BIOS does not provide full disk, cassette or BASIC
compatibility.

## C-BIOS pixel font

- Copyright: Eric Boon.
- License: BSD-style terms in `licenses/C-BIOS-font.txt` and
  `firmware/assets/fonts/msx-cbios-pixels-LICENSE.txt`.

## DotGothic16

- Copyright: The DotGothic16 Project Authors.
- License: SIL Open Font License 1.1.
- Full notice: `licenses/DotGothic16-OFL.txt` and
  `firmware/assets/fonts/DotGothic16-OFL.txt`.

## ESP-IDF, Espressif components, LVGL and board support

- ESP-IDF and most Espressif components: Apache License 2.0.
- LVGL: MIT License. Runtime notices include the bundled Montserrat font
  (SIL Open Font License 1.1), the TLSF allocator and formatting code.
- Waveshare ESP32-S3-Touch-AMOLED board support included under
  `components/esp32_s3_touch_amoled_1_8/`: Apache License 2.0; the complete
  license is retained in that directory.
- Firmware dependency notices for this build are in `licenses/runtime/`.
  The resolved SDK and component sources carry further per-file notices.

## Not included / 同梱しないもの

- Commercial game ROMs and other copyrighted MSX software
- Proprietary MSX, MSX2, MSX2+ and disk BIOS ROMs
- User save states, Wi-Fi credentials, screenshots and diagnostic captures

Users must supply only media and BIOS files they are legally entitled to use.

