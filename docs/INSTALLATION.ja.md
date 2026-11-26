# 導入マニュアル

Waveshare ESP32-S3-Touch-AMOLED-1.8 V2へ配布済みファームウェアを書き込み、
C-BIOS入りのmicroSDカードを準備する手順です。

## 必要なもの

- Waveshare ESP32-S3-Touch-AMOLED-1.8 V2
- データ通信対応USB-Cケーブル
- FAT32形式のmicroSDカード
- Python 3と`esptool`
- 任意：Kモードの8BitDo Micro

```sh
python3 -m pip install 'esptool==4.12.0'
```

## 1. microSDカードの準備

[配布ZIP](https://github.com/game-de-it/RetroS3-Watch/releases/latest)を展開し、
`sdcard/`の中身をFAT32のmicroSDカードのルートへコピーします。

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

BIOSディレクトリにはC-BIOS 0.29が含まれています。多くのカートリッジROMは
正規MSX BIOSなしで起動できますが、ディスク、テープ、BASICの完全互換性は
ありません。

利用権を持つメディアを`msx/roms`へ追加します。対応形式は`.rom`、読み取り専用の
`.dsk`、`.cas`です。更新時はmicroSDをバックアップし、ご自身で入れたBIOSは
保持したまま必要なC-BIOSファイルを追加してください。

## 2. 書き込みモードへ入る

RetroS3 Watchファームウェアが動作中なら、USBシリアルへ半角の`d`を1文字送ると
ESP32-S3 ROMダウンローダーへ再起動します。

未書き込み、または起動しない場合は、BOOTを押したまま電源を入れるかリセットし、
その後BOOTを離します。バッテリー駆動中はUSBを抜き差ししても、電源が切れない場合があります。

ポート名の例：

- macOS：`/dev/cu.usbmodem...`
- Linux：`/dev/ttyACM...`
- Windows：`COM...`

## 3. 配布済みファームウェアの書き込み

配布ZIPの展開先、またはリポジトリのルートで実行します。
`PORT`は実際のポート名へ置き換えてください。
`release/flash/`で`shasum -a 256 -c SHA256SUMS`を実行すると書き込みファイルを検証できます。

```sh
python3 -m esptool --chip esp32s3 -p PORT -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 release/flash/bootloader.bin \
  0x8000 release/flash/partition-table.bin \
  0x10000 release/flash/retros3_watch.bin
```

DIOモードを使ってください。検証済みボードでQIOはサポートしません。
最後にハッシュ検証とハードリセットが表示されれば書き込み成功です。

## 4. 初回起動

再起動後に時計画面が表示されます。本体のBOOTボタンでランチャーを開き、
ROMブラウザーに`/msx/roms`のメディアが表示されることを確認します。

既定はMSX2です。ROM一覧の`MODEL`をタップするとMSX1・MSX2・MSX2+を切り替えられます。

## 5. Wi-Fi転送の初期設定（任意）

```sh
python3 -m pip install pyserial
python3 firmware/tools/configure_wifi.py --port PORT
```

パスワードはシェル履歴に残らず、端末のNVSへ保存されます。ランチャーで
`Wi-Fi UPLOAD`を開き、表示されたLANアドレスをPCのブラウザーで開きます。
`Game ROM`または`BIOS ROM`を選び、1ファイルずつ転送します。書き込み後に
SHA-256でmicroSD上の内容が検証されます。

2.4GHz Wi-Fiに対応したステーションモードです。

## 正規BIOSについて

RetroS3 MSXは`msx/bios`の次のファイルを使用します。

| 機種 | メインBIOS | 拡張BIOS |
| --- | --- | --- |
| MSX1 | `MSX.ROM` | ― |
| MSX2 | `MSX2.ROM` | `MSX2EXT.ROM` |
| MSX2+ | `MSX2P.ROM` | `MSX2PEXT.ROM` |

カセットやBASICには、正当に入手した対応システムBIOSを使用してください。
ディスクには加えて、Sony/Philips系WD2793対応の`DISK.ROM`が必要です。
C-BIOSに戻せるよう、差し替え前のファイルをバックアップしてください。

## 復旧

書き込み後に画面が黒い場合：

1. ROM書き込みモードへ入り直します。
2. DIO、16 MB、3つのオフセットを確認します。
3. 3ファイルすべてを再書き込みします。
4. USB電源を切断し、再接続します。

この配布物に工場出荷バックアップは含まれません。パーティションやブートローダーを
変更する前に、必要に応じて自分の端末をバックアップしてください。

