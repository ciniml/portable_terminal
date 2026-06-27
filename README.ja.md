# tab5_claude_client

**M5Stack Tab5** (ESP32-P4, 1280×720 MIPI-DSI LCD) 向けターミナルエミュレータ。
Wi-Fi 経由で SSH / Telnet 接続、さらに WireGuard / Tailscale VPN で NAT 越しの
ホストにも到達可能。常時稼働の中継機を借りずに、家庭/ラボのサーバへ Tailscale
direct UDP で繋がる携帯ハードウェア SSH ターミナルとして使うことを想定。

英語版は [README.md](README.md) を参照。

---

## 機能

- **80×30 VT100 ターミナル** を Tab5 LCD に [M5GFX](https://github.com/m5stack/M5GFX) /
  [M5Unified](https://github.com/m5stack/M5Unified) で描画
- **リモート接続**
  - SSH (libssh2 + mbedTLS、パスワードまたは埋め込み鍵による認証、TOFU ホスト鍵検証)
  - Telnet (RFC 854 + NAWS)
  - 指数バックオフによる自動再接続 + TCP/SSH keepalive
- **Tailscale クライアント** (control plane, DERP, DISCO, NAT 越え) — プロトコル
  の詳細は [docs/TAILSCALE_PORTING_NOTES.md](docs/TAILSCALE_PORTING_NOTES.md) 参照
  - DISCO Ping/Pong による direct path 検証
  - DISCO CallMeMaybe (送受信両方)
  - HTTPS による公開 IP 取得 (UDP STUN の応答が返ってこない MAP-E /
    IPv4-over-IPv6 環境向けの代替)
  - WireGuard data plane (managed mode + DERP fallback)
- **入力**
  - タッチスクリーン (60 Hz polling)
  - オンスクリーンソフトキーボード (ターミナルリサイズ連動)
  - 付属クリップオン **Tab5 Keyboard** (I2C HID、ソフトウェアタイプマティック / オートリピート対応)
- **ステータスパネル** — バッテリ %、充電状態、Wi-Fi IP、SSH 状態、稼働時間
- **ブザー** (BEL → M5.Speaker)
- **ホスト側ユニットテスト** — ハードウェア非依存な VT100 コアを GoogleTest で 52 件カバー

## ハードウェア

- **M5Stack Tab5** (ESP32-P4 + Wi-Fi 用に SDIO 接続された ESP32-C6)
- 任意: **Tab5 Keyboard** (クリップオン QWERTY、ExtPort1 上の I2C 0x6D)

## リポジトリ構成

```
main/                          ターゲット側エントリ: app_main、M5GFX を IDisplay でラップ
components/term_core/          HW 非依存 VT100 コア (IDF + ホスト両対応)
components/M5GFX/              git submodule — vendored、IDF 6.0 パッチ適用
components/M5Unified/          git submodule — vendored、IDF 6.0 パッチ適用
components/libssh2/            libssh2 1.11.1 submodule + 自作 CMake ラッパ (mbedTLS)
components/wireguard/          serial_wifi_logger からベンダ
components/tailscale/          serial_wifi_logger からベンダ、大幅拡張済み
components/usb_host_ftdi_sio/  serial_wifi_logger からベンダ (Phase 2+ で使用)
host_test/                     スタンドアロン CMake プロジェクト、gtest
sdkconfig.defaults*            プロジェクト/ターゲットごとの sdkconfig デフォルト
partitions.csv                 OTA 対応パーティションレイアウト (16 MB flash)
c6_updater/                    C6 ファームを SDIO 経由で更新するスタンドアロン IDF 5.4 プロジェクト
slave_c6_fw/                   C6 スレーブファーム (esp_hosted 2.x)
docs/                          ドキュメント
M5_IDF6_PATCHES.md             M5GFX / M5Unified の IDF 6.0 パッチ一覧
```

## クイックスタート — リリース済みファームを書き込む

一番手軽な方法は **Web Flasher** です:

<https://ciniml.github.io/portable_terminal/>

Tab5 を USB で接続し、リリースを選んで Flash を押すだけ。Chrome / Edge など
WebSerial 対応ブラウザが必要です。C6 ファームが対応バージョンに更新されている必要が
あります (後述の「Wi-Fi C6 ファーム」参照)。Wi-Fi を使わずローカル UART だけで
動作確認する用途であれば、本アプリのみ書き込んでそのまま使えます。

リリース成果物は [GitHub Releases](https://github.com/ciniml/portable_terminal/releases)
からもダウンロードできます。各リリース ZIP には単一ファイル形式の
`firmware-vX.Y.Z.bin` と、esptool 用に分割した `bootloader.bin` /
`partition-table.bin` / `ota_data_initial.bin` / `tab5_claude_client.bin`
が同梱されています。

## 必要環境 (ソースからビルドする場合)

- **ESP-IDF 6.0** を `~/esp-idf/6.0` に展開
  ```bash
  source ~/esp-idf/6.0/export.sh
  ```
- Tab5 上の C6 に対応スレーブファーム (後述「Wi-Fi C6 ファーム」参照)
- ホストテスト用に CMake ≥ 3.22、GCC ≥ 13 または Clang ≥ 17 (C++23)

## ソースからのビルドとフラッシュ

```bash
git clone --recursive git@github.com:ciniml/portable_terminal.git
cd portable_terminal

make set-target          # 初回のみ: idf.py set-target esp32p4
make build               # 実機用ファームウェアをビルド
make flash monitor       # Tab5 にフラッシュしてログを追う
```

サブモジュールが取れていない場合:

```bash
git submodule update --init --recursive
```

`components/M5GFX` と `components/M5Unified` には ESP-IDF 6.0 用のローカル
パッチが当たっています — [M5_IDF6_PATCHES.md](M5_IDF6_PATCHES.md) 参照。
`git status` で submodule が dirty に見えるのは仕様です。上流を再同期した場合は
パッチを当て直してください。

## 設定

設定は `sdkconfig.defaults` を中心に、Kconfig の `Tab5 terminal` 配下から
変更します。サンプルをコピーして編集するのが楽です:

```bash
cp sdkconfig.defaults.local.example sdkconfig.defaults.local
$EDITOR sdkconfig.defaults.local
make build               # sdkconfig.defaults.local を読み込んでビルド
```

主要オプション:

| オプション | 役割 |
|---|---|
| `CONFIG_TAB5_WIFI_ENABLED` | C6 経由で Wi-Fi を起動するか |
| `CONFIG_TAB5_WIFI_SSID` / `_PASSWORD` | Wi-Fi 接続情報 |
| `CONFIG_TAB5_SSH_ENABLED` | 起動時に SSH 接続するか |
| `CONFIG_TAB5_SSH_HOST` / `_PORT` / `_USER` / `_PASSWORD` | SSH 接続先 |
| `CONFIG_TAB5_SSH_PUBKEY_AUTH` | パスワードではなく `main/keys/id_rsa` (PEM) を使う |
| `CONFIG_TAB5_TELNET_ENABLED` | SSH 無効/失敗時に Telnet にフォールバック |
| `CONFIG_TAILSCALE_ENABLE` | Tailscale スタックをコンパイル |
| `CONFIG_TAILSCALE_AUTH_KEY` | Tailscale 認証鍵 (`tskey-auth-…`) |
| `CONFIG_TAILSCALE_HOSTNAME` | tailnet に申告するホスト名 |
| `CONFIG_TAILSCALE_LISTEN_PORT` | WireGuard UDP listen ポート (デフォルト 41641) |
| `CONFIG_TAB5_BLE_CONFIG_ENABLED` | BLE プロビジョニング (デフォルト ON; 後述) |

BLE 経由のプロビジョニングはデフォルト有効です。
<https://ciniml.github.io/portable_terminal/settings.html>
の Web Bluetooth UI (Chrome / Edge 133+) から実行時に
Wi-Fi / SSH / Tailscale を設定できます。
C6 スレーブ FW を BT コントローラー込みでビルドする必要がありますが、
上流 esp_hosted のスレーブサンプルが ESP32-C6 ターゲット向けに
`BT_ENABLED=y + BT_CONTROLLER_ONLY=y` を既定で持っているので、
`slave_c6_fw/build.sh` で出来上がる blob にそのまま含まれます。
一度書き込めば、起動時 C6 自動更新
([`docs/C6_FW_UPDATE.md`](docs/C6_FW_UPDATE.md))
が後続のリリースを追従させます。
BLE が不要なら `CONFIG_TAB5_BLE_CONFIG_ENABLED=n` で約 100 KB 削減。
プロトコルや脅威モデルは
[`docs/BLE_CONFIG_DESIGN.md`](docs/BLE_CONFIG_DESIGN.md) を参照。

SSH 公開鍵認証を使う場合:

```bash
ssh-keygen -m PEM -t rsa -b 2048 -f main/keys/id_rsa
ssh-copy-id -i main/keys/id_rsa.pub user@host
```

`main/keys/` は `.gitignore` 済み。**秘密鍵を commit しないでください**。

## Wi-Fi C6 ファーム

Tab5 出荷時の C6 ファームは `esp_hosted` 1.4.x 系で、本プロジェクト
(`esp_hosted` 2.x 系) とは非互換です。更新方法は 2 通り。

**推奨 — IDF 5.4 ベースの SDIO 経由 updater** (カバーを開けずに済む):

```bash
./c6_updater/updater.sh /dev/ttyACM0   # ビルド・フラッシュ・モニタを一度に実行
```

`Slave update completed` を確認したら、本リポジトリのメインアプリを
`make flash monitor` で再フラッシュしてください。

**フォールバック — ESP-Prog を使った物理 UART フラッシュ**:

Tab5 のカバーを開けて C6 プログラミングパッドに ESP-Prog を結線
([slave_c6_fw/README.md](slave_c6_fw/README.md) 参照) してから:

```bash
./slave_c6_fw/build.sh
./slave_c6_fw/flash.sh /dev/ttyUSB0
```

**2.x 以降の更新はブート時に自動で走る**。一度 2.x に乗ってしまえば、
本リポジトリの main アプリに同梱した `c6_fw` パーティション (埋め込み版の
スレーブファーム) を起動時に読み、C6 の現バージョンより新しければ
`rpc_ota_*` で OTA 更新する。詳細とログメッセージは
[docs/C6_FW_UPDATE.md](docs/C6_FW_UPDATE.md) を参照。

## 使用方法

フラッシュ後:
1. Tab5 が起動し、Wi-Fi を立ち上げます (状況は右側ステータスパネルに表示)。
2. `CONFIG_TAILSCALE_ENABLE` が ON の場合、Tailscale クライアントが起動。
   初回起動時は認証 URL が必要で、ログとステータスパネルに表示されます。
3. `CONFIG_TAB5_SSH_ENABLED` が ON の場合、設定済みのホストへ SSH 接続が始まります。
4. 切断/再接続は supervisor タスクが指数バックオフ (1 秒 → 30 秒) で処理。

タッチスクリーンからソフトキーボードで入力できます。オプションのクリップオン
Tab5 Keyboard を装着すれば物理 QWERTY が使えます。どちらの入力も同じターミナル
sink に流れ込みます。

## ホスト側テスト

`term_core` ライブラリはハードウェア非依存で、ホスト側 C++ コンパイラで
ビルドできます。GoogleTest は CMake `FetchContent` で初回ビルド時に取得されます。

```bash
make build-host
make test-host
make clean-host
```

期待結果: 52 / 52 テストが成功。

## Tailscale NAT 越え

プロトコル詳細は [docs/TAILSCALE_PORTING_NOTES.md](docs/TAILSCALE_PORTING_NOTES.md)
に集約しています。主要トピック:

- DISCO ワイヤフォーマット (Ping / Pong / CallMeMaybe)、NaCl box の構造、
  sender disco-pub 埋め込み
- WireGuard udp_pcb を DISCO と共有して NAT mapping を統一する設計
- DERP-initial endpoint + multi-probe DISCO + 先着 Pong 採用 (first-Pong-wins)
- UDP STUN ではなく HTTPS で公開 IP を取得 (日本の MAP-E / IPv4-over-IPv6
  契約環境で UDP STUN 応答が返ってこない問題への対応)
- 外向き CallMeMaybe (rate-limit + verified-skip 付き)

これらの knowledge は、本リポジトリと同じ `wireguard` / `tailscale` コンポーネントを
含む他の ESP-IDF プロジェクトにそのまま横展開できます。

## ライセンス

本リポジトリのために新規作成されたソースコードは **Boost Software License 1.0**
の下で公開されています — [LICENSE](LICENSE) を参照。

以下のサードパーティコンポーネントは **vendored / submodule** で取り込まれて
おり、それぞれ元のライセンスが適用されます:

- `components/M5GFX/`、`components/M5Unified/` — git submodule。上流 M5Stack
  ライセンス (MIT)。ESP-IDF 6.0 用ローカルパッチが当たっています
  ([M5_IDF6_PATCHES.md](M5_IDF6_PATCHES.md) 参照)。
- `components/libssh2/libssh2/` — git submodule、BSD-3-Clause。
- `components/wireguard/`、`components/tailscale/`、`components/usb_host_ftdi_sio/`
  — [ciniml/serial_wifi_logger](https://github.com/ciniml/serial_wifi_logger)
  からベンダリング、BSD-3-Clause (上流 SPDX ヘッダ準拠)。
- ESP-IDF managed components (`espressif/esp_hosted`、`espressif/esp_wifi_remote`、
  `espressif/usb` 等) — Espressif レジストリから取得、各ライセンスに従う。

各コンポーネントのライセンスヘッダ / `LICENSE` ファイルも併せてご確認ください。

## 謝辞

- **M5Stack** — Tab5 ハードウェアおよび M5GFX / M5Unified ライブラリ
- **Tailscale / WireGuard** — 本プロジェクトはこれらプロトコルのサブセットを実装
- [ciniml/serial_wifi_logger](https://github.com/ciniml/serial_wifi_logger) —
  オリジナルの ESP32 向け Tailscale / WireGuard 実装。本プロジェクトはここから
  大幅に拡張しています
- **Espressif** — ESP-IDF、esp_hosted、C6 / P4 シリコン
