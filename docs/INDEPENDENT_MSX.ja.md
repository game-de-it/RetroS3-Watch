# RetroS3 MSX 独自コア

[English](INDEPENDENT_MSX.md)

RetroS3 MSXは、Z80、メモリ・スロット、VDP、PSG、入出力とメディア処理を
ハードウェア資料から実装したMITライセンスのコアです。ESP32-S3向けの
RetroS3 Watch標準ビルドに組み込まれます。
C-BIOSなど、同梱する第三者のファイルには各ライセンスが適用されます。

## ホストで実行

リポジトリのルートから実行します。

```sh
cmake -S firmware/components/retros3_msx -B firmware/build-msx-host -DCMAKE_BUILD_TYPE=Release
cmake --build firmware/build-msx-host
ctest --test-dir firmware/build-msx-host --output-on-failure
firmware/build-msx-host/retros3-msx \
  --model 3 --bios sdcard/msx/bios/MSX2P.ROM --sub sdcard/msx/bios/MSX2PEXT.ROM \
  --rom game.rom --mapper linear --frames 300 --ppm frame.ppm --save game.rss
```

`--model 1|2|3` はMSX1、MSX2、MSX2+に対応します。カートリッジのマッパーには
`linear`、`ascii8`、`ascii16`、`konami`、`scc`を指定できます。同じBIOSとメディアを
指定して `--load game.rss` で再開できます。ホスト実行ツールは最終フレームをPPMに
出力できる検証用コマンドです。使える引数は `--help` で確認できます。

## 時計向けビルド

ESP-IDF 5.5.5を有効にしてから実行します。

```sh
cd firmware
idf.py reconfigure
python3 tools/restore_display_patch.py
idf.py build
```

microSDには `sdcard/` の内容をコピーします。
MSXメディア一覧の「MODEL」で機種を選択し、`msx/roms` 内のメディアを開きます。
SELECTでメニュー、STARTでソフトウェアキーボード、L2でシフトを切り替えます。
メニューから3つのステートスロット、ドライブA/Bのディスク交換、カセットの巻き戻しを
利用できます。`.rss` ステートを読み込むときは保存時と同じ機種、BIOS、メディアを
使います。v0.1.0のステートとは互換性がありません。

## BIOSとメディア

| 機種 | メインBIOS | サブBIOS | 任意のC-BIOSロゴ |
| --- | --- | --- | --- |
| MSX1 | `MSX.ROM` | ― | `CBIOS1.ROM` |
| MSX2 | `MSX2.ROM` | `MSX2EXT.ROM` | `CBIOS2.ROM` |
| MSX2+ | `MSX2P.ROM` | `MSX2PEXT.ROM` | `CBIOS2P.ROM` |

- **カートリッジ**。同梱のC-BIOSで起動できます。時計の自動マッパー判定で
  起動しない場合は、ROMと同じ場所に `game.rom.mapper` を置き、マッパー名を1語で指定します。
- **カセット**。CASの再生に対応します。対応する実機BIOSを `msx/bios` に用意し、
  ソフトに応じたBASICコマンドを入力します。1200 baud相当で読み込むため、
  大きなテープでは読み込みに数分かかります。
- **ディスク**。標準的な512バイトセクターのDSKを読み取り専用で扱います。
  対応する実機BIOSとSony/Philips系WD2793インターフェース用の `DISK.ROM` が必要です。

ホストではC-BIOSの3機種、実機BIOSのMSX2・MSX2+、イースIIの起動とデータディスク
交換、OBAKE HALLOWEENのCAS起動を確認しました。
時計では、USASの起動とmicroSDへのステートの保存・復元、イースIIの起動ディスクから
著作権表示までを確認しました。データディスク交換とCASの全ロードはホストでの確認です。
描画率100%のUSASデモでは、時計の安定した10秒間で平均約59.4 fpsを計測しました。
起動時とシーン切り替え時には、表示間隔が長くなる箇所があります。
時計のメニューではドライブ名を省略せず、選択中の長いディスク名をスクロール表示します。
現段階の対象外は、FDI、ディスク・テープへの書き込み、FM音源、インターレースと
サイクル単位のVDPタイミングです。ゲームと実機BIOSは同梱しません。

## 実装資料

- [Zilog Z80 CPU User Manual](https://www.zilog.com/docs/z80/um0080.pdf)
- [ASCII MSX2 Technical Handbook](https://github.com/Konamiman/MSX2-Technical-Handbook)
- [Yamaha V9938 Technical Data Book](https://ftp.whtech.com/YAHOO%20group%20backups/GENEVE/FILES/v9938%20Application%20Manual.htm)
- [Yamaha V9958 Technical Data Book](https://www.vgamuseum.info/images/doc/yamaha/v9958.pdf)
- [Sony HB-F500 Service Manual](https://hansotten.file-hunter.com/uploads/files/sonyhbf500sm2.pdf)
- [General Instrument AY-3-8910 Data Manual](https://map.grauw.nl/resources/sound/generalinstrument_ay-3-8910.pdf)
