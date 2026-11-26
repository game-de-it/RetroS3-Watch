# 開発者向けマニュアル

本書は、RetroS3 Watchのソース構成、再現ビルド、テスト、実機検証、公開リリース
作成を扱います。

[English version](DEVELOPER_GUIDE.md)

エミュレーターの構成、ホストビルド、互換性については
[RetroS3 MSX](INDEPENDENT_MSX.ja.md) を参照してください。

## ライセンス境界

RetroS3 MSXを含む本プロジェクト独自のコードと文書はMITライセンスです。
C-BIOSなどの依存コンポーネントには各ライセンスが適用されます。
詳しくは [サードパーティライセンス](../THIRD_PARTY_LICENSES.md) を参照してください。

製品名はRetroS3 Watchです。MSX/MSX2は互換性の説明にだけ用い、MSXロゴは
使用せず、READMEの商標・非提携表示を残してください。

## 対応ハードウェアとツールチェーン

- Waveshare ESP32-S3-Touch-AMOLED-1.8 V2
- 16 MiBフラッシュ、8 MiB Octal PSRAM搭載ESP32-S3
- ESP-IDF 5.5.5
- ホストテスト用Python 3およびCコンパイラ
- DIOフラッシュモード。検証済みボード設定ではQIOを使用しないこと

他のボードやESP-IDF版は動作する可能性がありますが、検証済みリリース経路の
対象外です。

## リポジトリ構成

```text
components/                 ボードおよびUSB復旧用ローカルコンポーネント
docs/                       英語・日本語マニュアル
firmware/                   ESP-IDFアプリケーション
  assets/fonts/             フォント原本とライセンス
  components/retros3_msx/   MSXコアとホスト用検証ツール
  main/                     ランチャー、エミュレータ連携、表示、入力、Wi-Fi
  patches/                  管理コンポーネント用パッチ
  tests/                    ホスト側単体テスト
  tools/                    ビルド、転送、Wi-Fi、診断補助ツール
licenses/                   サードパーティ通知
release/flash/              書き込み済みバイナリとチェックサム
sdcard/msx/bios/            再配布可能なC-BIOS
```

ゲーム、正規BIOS、ディスク・テープイメージ、ステート、認証情報、キャプチャ、
ローカルビルドツリーをリポジトリへ入れないでください。

## ソースからのビルド

Espressifの手順でESP-IDF 5.5.5を導入し、現在のシェルで有効化します。
リポジトリのルートから実行します。

```sh
cd firmware
idf.py reconfigure
python3 tools/restore_display_patch.py
idf.py build
```

`idf.py reconfigure`で管理コンポーネントを解決した後、パッチ補助ツールがLCD
パネルに必要な変更を適用します。`managed_components/`を削除・更新した場合は、
補助ツールを再実行してください。

生成される`firmware/dependencies.lock`には、ローカルコンポーネントの解決時に
絶対パスが記録される場合があります。そのため意図的にGit対象外としており、
公開してはいけません。依存関係を更新した場合は、リリース前に内容を確認します。

### 開発ビルドの書き込み

`PORT`を実機のシリアルポートに置き換えます。

```sh
idf.py -p PORT flash monitor
```

配布バイナリの書き込み位置は次の通りです。

```text
0x0000  bootloader.bin
0x8000  partition-table.bin
0x10000 アプリケーションバイナリ
```

リリースイメージの生成・書き込みではDIOモードを維持してください。

## ホストテスト

`firmware/`から実行します。

```sh
python3 -m unittest discover -s tests -v
```

描画計測、差分領域、拡大縮小、ファイル名処理、コントローラー動作を検査します。
CPU、VDP、メディア、ステート保存の検査には
[MSXコアのテスト](INDEPENDENT_MSX.ja.md#ホストで実行) を実行します。

## フォント生成

生成済みフォントを同梱しているため、通常のビルドにはNode.jsは不要です。
再生成時はNode.jsと`lv_font_conv` 1.5.3を次の場所へ導入します。

```text
toolchains/font-tools/node_modules/
```

続いて実行します。

```sh
python3 tools/generate_fonts.py
```

生成コードの差分を確認し、メニューとキーボードの文字をAMOLED実機で確認してから
コミットしてください。

## 実行時構成

- `app_main.c`が時計ランチャーと上位アプリ状態を管理します。
- `main/msx_runner.cpp`が時計と`components/retros3_msx/`のMSXコアを接続します。
- メインRAMはMSX1で64 KiB、MSX2とMSX2+で256 KiBです。
  コアはVRAMを128 KiB確保します。
- `msx_video_diff.c`が更新領域を検出し、LCD経路は整列済みの偶数サイズ単位で
  転送します。既定は100%描画です。
- メニューとソフトウェアキーボードは部分再描画を使い、不要な全画面転送による
  入力遅延を避けます。
- `controller.c`が検証済みの8BitDo Micro Kモード経路を扱います。
- `wifi_uploader.c`が、USBでWi-Fi情報を設定した後のローカルHTTP転送を提供します。
- ステートはmicroSDに`.rss`形式で保存されます。読み込み時は保存時と同じ機種、
  BIOS、メディアを使います。v0.1.0のステートは別形式です。

ソースの既定値は100%描画です。性能目標の変更を明記せずに、リリース版で値を
下げないでください。

## C-BIOSの同梱

C-BIOS 0.29のファイルを、ファームウェアが期待する名前へ変更しています。

| 配置先 | C-BIOS上流ファイル |
| --- | --- |
| `sdcard/msx/bios/MSX.ROM` | `cbios_main_msx1.rom` |
| `sdcard/msx/bios/MSX2.ROM` | `cbios_main_msx2.rom` |
| `sdcard/msx/bios/MSX2EXT.ROM` | `cbios_sub.rom` |
| `sdcard/msx/bios/CBIOS2.ROM` | `cbios_logo_msx2.rom` |
| `sdcard/msx/bios/MSX2P.ROM` | `cbios_main_msx2+.rom` |
| `sdcard/msx/bios/MSX2PEXT.ROM` | `cbios_sub.rom` |
| `sdcard/msx/bios/CBIOS2P.ROM` | `cbios_logo_msx2+.rom` |

C-BIOSにより、再配布可能なカートリッジ中心の起動経路を提供できます。ディスク、
テープ、BASICの完全な互換性はありません。正規BIOSやDiskROMをリリースへ追加して
はいけません。

## 実機受入確認

ビルド成功や1画面の表示だけではリリース合格ではありません。対応ボードで次の
経路を個別に確認します。

1. コールドブートとタッチランチャー
2. 同梱C-BIOSによるカートリッジ起動
3. 100%描画設定でのMSX1・MSX2・MSX2+の動画面
4. 動画面中の音声継続
5. コントローラーの接続と再接続
6. SELECTのエミュレータメニュー、STARTのソフトウェアキーボード、L2のShift切り替え
7. 新規スロットでのステート保存・読込
8. 利用者が正当に用意した媒体・BIOSによるディスク起動と入れ替え
9. 利用者が正当に用意した媒体・互換BIOSによるテープ読込
10. USBによるWi-Fi設定とHTTPファイル転送
11. エミュレーション終了からランチャーへの復帰、および再起動復旧

正確なビルド、実機条件、未解決経路を記録します。1つの成功結果から未検証経路の
成功を推定しないでください。

## 公開リリース手順

1. 公開用の配布ツリーから開始します。
2. ホストテストとESP-IDFのクリーンビルドを実行します。
3. 実機受入確認表を完了します。
4. 3つの書き込みイメージを`release/flash/`へ置き、そのディレクトリ内で
   `SHA256SUMS`を生成します。
5. 7つのC-BIOSを`sdcard/msx/bios/SHA256SUMS`で検証します。
6. `LICENSE`、`THIRD_PARTY_LICENSES.md`、すべての上流通知を確認します。
7. コミット予定ファイルから、個人パス、ユーザー名、認証情報、媒体、生成された
   依存ロックを検索します。
8. 書き込みイメージ、`sdcard/`、マニュアル、Wi-Fi設定ツール、ライセンス通知を
   ZIPにまとめます。展開後の内容を確認し、新しいリリースタグで公開します。

リポジトリのルートで行うコミット前確認例です。

```sh
git diff --check
git ls-files | rg -i '\.(rom|dsk|fdi|cas|sta|state|sav|rss)$'
git grep -nF "$HOME"
git grep -nE 'password|ssid'
git log --format=fuller
```

媒体一覧に現れてよいのは、意図的に含めた7つのC-BIOS ROMだけです。認証情報の
一致はすべて目視確認します。一般的なフィールド名は存在しますが、実値は存在して
はいけません。push前に作者名とメールアドレスも確認してください。

`firmware/build/`、`firmware/managed_components/`、`sdkconfig`、
`dependencies.lock`、シリアルログ、スクリーンショット、ステート、開発用ROM集を
公開しないでください。リリースアーカイブはGit追跡ファイルだけから作成します。

