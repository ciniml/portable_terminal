# ESP32-C6 スレーブファーム ブート時オートアップデート

## なぜこの仕組みがあるのか

Tab5 の Wi-Fi 無線は ESP32-C6 コプロセッサに乗っており、ESP32-P4 上で動く
本ファームは SDIO 経由で `esp_hosted` 2.x プロトコルを話して C6 を駆動する。
工場出荷状態の C6 は `esp_hosted` 1.4.x の M5Stack 提供ファーム
(`ESP32C6-WiFi-SDIO-Interface-V1.4.1`) で、2.x とはワイヤープロトコルが
非互換なので、最初の 1.4.x → 2.x 移行だけは別途
[`c6_updater/`](../c6_updater/) (IDF 5.4 + esp_hosted 1.4 ホスト)
を使う必要がある。

ただし**いったん 2.x に乗ってしまえば、その後の 2.x → 2.x 更新は本ファームの
ブート時に自動で走る**。リリース ZIP には P4 アプリと一緒に最新スレーブ
ファーム (`c6_fw.bin`) が同梱され、専用の `c6_fw` パーティション
(`0x710000`, 1.5 MB) に書き込まれる。P4 起動時に `c6_fw_update.cpp` が
パーティションから埋め込み版のバージョンを読み、C6 で実行中の版と比較し、
埋め込み版の方が新しければ `rpc_ota_*` で OTA を流し込む。

## ブート時のフロー

1. `wifi_setup.cpp::wifi_sta_connect` が PI4IOE2 の `WLAN_PWR_EN` を一度
   落として再投入し、C6 を電源リセット。
2. `esp_hosted` の transport ハンドシェイクが完了するまで最大 6 秒待ち、
   `esp_hosted_get_coprocessor_fwversion` で現在 C6 で動いているバージョンを取得。
3. `c6_fw` パーティション先頭 256 B から `esp_app_desc_t` (オフセット
   `0x20`) を読んで埋め込み版のバージョン文字列をパース。
4. (embedded > current) のときだけ `rpc_ota_begin/write/end/activate` を
   叩いて 1400 B チャンクで全イメージを流し込む (`mmap` で flash cache から
   直接読む)。
5. 成功したら `esp_restart()` して P4 をリブート — 起動時に C6 が新ファームで
   立ち上がり直し、ホスト側 transport も新規ハンドシェイクで揃う。

差分がない、埋め込みブロブが無い、ダウングレード方向、のいずれかなら何も
しないで Wi-Fi 接続シーケンスに進む。

## 埋め込みスレーブファームを更新する

リポジトリのルートで `slave_c6_fw/build.sh` を一度走らせると、
`managed_components/espressif__esp_hosted/slave/build/network_adapter.bin`
が生成される。ビルド側はそのファイルを次の優先順で探す:

1. `slave_c6_fw/network_adapter.bin` (シンボリックリンクを張るのが推奨)
2. `c6_updater/main/slave_fw/network_adapter.bin` (`c6_updater` 系の慣習)

どちらにも見つからない場合は CMake の configure 時に `STATUS` メッセージで
警告し、`c6_fw` パーティションは空 (`0xFF`) のままビルドする。実機側は
「埋め込みブロブが無い」と判断して何もしない (Wi-Fi 接続そのものは正常に動く)。

ローカル開発で使うシンボリックリンクの例:

```bash
ln -s ../managed_components/espressif__esp_hosted/slave/build/network_adapter.bin \
    slave_c6_fw/network_adapter.bin
```

## 手動で `c6_fw` パーティションを書き込む

`make flash` から `c6_fw` を **意図的に除外** している (理由は次節)。
スレーブファームのブロブをパーティションへ直接書き込みたい場合は:

```bash
make flash-c6 PORT=/dev/ttyACM0
```

これは `esptool --force write-flash 0x710000 build/c6_fw.bin` を呼び出す
専用ターゲット。`--force` 無しでは P4 ホスト側 esptool が C6 イメージの
チップ ID 不一致を検出して abort する。

普段は **手動書き込みは初回セットアップだけ**で十分。それ以降は本ファームの
ブート時 OTA が後続バージョンを自動的に流し込む。

### なぜ `c6_fw.bin` を `flash_args` から外すのか

`esptool write-flash` は各入力ファイルの IDF イメージヘッダを検査し、
チップ ID がホスト側 (`esp32p4`) と一致しないと

```
A fatal error occurred: 'c6_fw.bin' is not an ESP32-P4 image.
Use the force argument to flash anyway.
```

で abort する。`c6_fw.bin` はそもそも ESP32-C6 のアプリイメージなので、
`flash_args` (= 主 `flash` ターゲットの入力) に含めると **必ず失敗**する。
代わりに:

- **手動セットアップ**: `make flash-c6` (`--force` 付き) を独立ターゲットで提供
- **リリース ZIP の単一ファイル `firmware-vX.Y.Z.bin`**: `script/pack_firmware.py`
  に `--extra 0x710000:build/c6_fw.bin` を渡して `c6_fw` も含めて結合
  (`esptool` を経由しないので validation 制約は無関係)
- **Web Flasher (`docs/index.html`)**: `FLASH_PARTS` の optional 5 番目として
  `c6_fw.bin` を持つ。ZIP に同梱されていれば書き込み、無ければ skip

## 失敗モードとログメッセージ

| ログ (TAG=`c6_fw`) | 意味 | 対処 |
|---|---|---|
| `no c6_fw partition in table — auto-update disabled` | パーティションテーブルに `c6_fw` が無い (古いビルド) | 本ファームをビルドし直す |
| `c6_fw partition is blank — no embedded slave fw` | パーティションは確保されているが中身が `0xFF` (ブロブ未組み込み) | `slave_c6_fw/build.sh` を実行してから main アプリを再ビルド |
| `c6_fw blob has wrong magic 0x...` | パーティションの中身が ESP-IDF アプリイメージではない | リリース ZIP の `c6_fw.bin` が壊れている可能性 — ZIP を取り直す |
| `embedded c6 fw version 'XYZ' — can't parse X.Y.Z` | `esp_app_desc::version` が `vX.Y.Z` 形式でない (e.g. `git describe` の `-g1234`) | CI ビルド (`version.txt`) を使うか、`slave_c6_fw` 側にも `version.txt` を置く |
| `fwver query failed (...) — esp_hosted SDIO link is not up.` ＋ `C6 likely still on the factory 1.4.x firmware.` | C6 が `esp_hosted` 1.4.x のままで、2.x ホストと話せない | 一度だけ `./c6_updater/updater.sh` で 1.4 → 2.x 移行を実施 |
| `C6 firmware up to date (cur=A.B.C, embedded=A.B.C)` | 差分なし | 正常動作。何もしない |
| `C6 update available: A.B.C → X.Y.Z; starting OTA` ＋ `OTA progress N / M (...%)` | 更新中 | 完了するまで電源を切らない (約 10–20 秒) |
| `C6 firmware updated successfully` ＋ `C6 firmware updated — restarting P4 to renegotiate link.` | 完了 | P4 が `esp_restart()` で自動再起動する |
| `rpc_ota_write at N/M failed: ...` | OTA 途中で失敗 | C6 は前のファームのままなので動作上の影響はない (次回ブートで再試行) |

## 関連ファイル

- [`partitions.csv`](../partitions.csv) — `c6_fw` パーティション定義
- [`CMakeLists.txt`](../CMakeLists.txt) — ブロブ探索とフラッシュ登録
- [`main/c6_fw_update.{hpp,cpp}`](../main/c6_fw_update.cpp) — バージョン比較と OTA ストリーミング
- [`main/wifi_setup.cpp`](../main/wifi_setup.cpp) — 起動シーケンスへの統合
- [`slave_c6_fw/`](../slave_c6_fw/) — スレーブファームのビルドスクリプト
- [`c6_updater/`](../c6_updater/) — 1.4.x → 2.x 初回移行用スタンドアロンアップデータ
