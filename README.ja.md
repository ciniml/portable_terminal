# portable_terminal

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
`partition-table.bin` / `ota_data_initial.bin` / `portable_terminal.bin`
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

## HTTP 設定サービス (softAP + QR オンボーディング)

`CONFIG_TAB5_HTTP_CONFIG_ENABLED=y` (デフォルト) の場合、STA 接続と並行して
WPA2 の softAP `Tab5-XXXXXX` (サフィックスは AP MAC) を立ち上げ、
`http://192.168.4.1/` に設定ページ、`GET /api/info` に JSON ステータス API、
さらに設定書き込み API を提供します。

| ルート | ボディ (JSON) | 効果 |
|---|---|---|
| `POST /api/wifi` | `{"ssid","psk","open"?}` | STA 資格情報を保存 (NVS) |
| `POST /api/profile` | `{"proto","host","port"?,"user"?,"password"?}` | 接続プロファイル 0 を保存 |
| `POST /api/tailscale` | `{"auth_key"?,"hostname"?}` | Tailscale 設定を保存 (NVS `tailscale`) |
| `POST /api/reboot` | — | 応答の約 500 ms 後に再起動 |

書き込みは NVS への保存のみで、再起動後に反映されます (応答には
`"reboot_required": true` が付き、設定ページの再起動ボタンが強調表示されます)。
ボディは 1 KB まで。`/api/info` はシークレットについて `*_set` の真偽値のみを
返し、シークレット自体は決して返しません。

**空フィールドの扱い** (設定ページのフォームも同じ規約):

- `/api/wifi` — `ssid` と `psk` は毎回両方必須です (「保存済み psk を維持」の
  ショートカットはありません — 保存済み psk が間違っているケースこそこの
  エンドポイントの出番のため)。空の `psk` は明示的な `"open": true`
  (オープンネットワーク) と併用した場合のみ受け付けます。
- `/api/profile` — `password` が空/未指定の場合、プロファイル 0 の保存済み
  パスワード*と*認証方式を維持します (ホストだけの編集で公開鍵設定を
  壊さないため。鍵自体はファームウェア埋め込みのまま)。非空のパスワードは
  認証方式をパスワードに切り替えます。
- `/api/tailscale` — 空/未指定のフィールドは保存済みの値を維持します。
  非空の `auth_key` は `tskey-` で始まる必要があります。

**プロビジョニングの流れ** — 未設定のデバイス (STA 資格情報なし) では、
起動パスが `wifi_hw_init()` (C6 電源投入 + esp_hosted + `esp_wifi_init`、
STA 接続なし) を呼んで設定 AP を直接起動するため、ネットワーク未設定でも
プロビジョニングできます:

1. 未設定のデバイスを起動 → STA 資格情報なし → 設定 AP `Tab5-XXXXXX` が
   起動し、LCD に QR オンボーディング画面を表示。
2. スマートフォンで Wi-Fi QR を読んで AP に参加し、URL QR を読む —
   またはキャプティブポータルが `http://192.168.4.1/` を自動的に開きます。
3. Wi-Fi フォーム (必要ならプロファイル / Tailscale も) を入力 → 保存。
4. 強調表示された再起動ボタンをタップ (または電源再投入)。
5. 再起動後は保存された資格情報で通常どおり STA 接続します。設定ページは
   引き続き AP (および LAN) から利用できます。

- **機体ごとの AP パスワード** — 初回起動時に 10 文字 (紛らわしい
  `0/o/1/l` を除いた小文字+数字) を生成して NVS (ネームスペース
  `httpcfg`、キー `ap_psk`) に保存するため、再起動しても変わりません。
  `CONFIG_TAB5_HTTP_CONFIG_AP_PSK` を非空にするとそちらが優先されます
  (開発用)。パスワードを忘れた場合はターミナルの起動ログ
  (`Config AP "Tab5-XXXXXX" pass "..."`) を確認するか、
  `idf.py erase-flash` で再生成されます。
- **QR オンボーディング** — STA 接続に *失敗* した場合 (資格情報の誤り
  など)、LCD にフルスクリーンで 2 つの QR コードを表示します。Wi-Fi QR
  (`WIFI:T:WPA;...` — 標準カメラアプリで読むだけで AP に接続) と
  `http://192.168.4.1/` の URL QR、および平文の SSID / パスワードです。
  キー入力またはタップで閉じます。
- **キャプティブポータル** — UDP/53 の小さな DNS レスポンダが全クエリに
  `192.168.4.1` を返し、HTTP サーバは OS の接続性プローブ
  (`/generate_204`、`/hotspot-detect.html`、`/connecttest.txt` など) と
  未知のパスをすべて設定ページへ 302 リダイレクトするため、AP に参加した
  スマートフォンでは設定ページが自動的に開きます。

## 画面ロック (無操作ロック + PIN)

共有デスク向けのプライバシーロック (任意機能)。設定した時間だけ
**ユーザー入力** (タッチ、ソフトキーボード、クリップオン / USB キーボード、
UART / USB-JTAG のバイト入力 — SSH のストリーミング出力は**カウントしない**)
が無いとバックライトが消灯します。次の入力で画面が点灯し、ターミナル内容
ではなくフルスクリーンの PIN パッドが表示されます。PIN はタッチのテンキー
または物理キーボード (数字 + Backspace + Enter) で入力できます。5 回間違えると
段階的ロックアウト (30 秒から倍々、最大 240 秒) がかかり、カウントダウンを
画面に表示します。試行カウンタは RAM のみで、解除または再起動でリセットされます。

- 設定は設定ポータルの「画面ロック」セクションから: 有効トグル、
  無操作タイムアウト (分)、PIN (4〜8 桁)。保存は即時反映で再起動不要。
  PIN は SHA-256 ハッシュとして NVS (ネームスペース `scrlock`) に保存されます。
- ☰ メニューのフッター `[Lock]` ボタンで即時ロックできます
  (PIN 未設定の間はグレー表示)。
- **PIN を忘れた場合**: 設定ポータル (softAP パスワードは QR 画面 /
  起動ログに表示) を開いて新しい PIN を設定してください — 旧 PIN の入力は
  不要です。これは意図的な仕様で、この画面ロックは覗き見防止のカジュアルな
  保護であり、暗号学的な保護ではありません (フラッシュを読める人には
  データは保護されません)。

## OTA アップデート

GitHub Pages にホストされた `latest.json` を 6 時間おき (±15 分のジッタ、
STA MAC 由来のシードで機体分散) にポーリングし、新しいバージョンがあれば
`esp_https_ota` で自動更新します。

1. `<base>/latest.json` を GET (デフォルト URL は Kconfig、NVS `ota.base_url`
   で上書き可)
2. `esp_app_desc.version` と `latest.json:tag` を SemVer 比較。
   ダウングレードは `allow_downgrade: true` が manifest にある場合のみ許可。
3. アプリバイナリを 256 KB の HTTP Range 分割でダウンロード
   (Wi-Fi 瞬断でも全体再送にならない)。
4. 遅延リブート — アクティブな `IConnection` が無くなるまで最大 30 分待機。
5. 新イメージ起動後 120 秒間正常動作したら
   `esp_ota_mark_app_valid_cancel_rollback` を発行。ハング / クラッシュ
   した場合は bootloader が次回起動で自動ロールバック。

ランタイム設定は NVS 名前空間 `"ota"`:

| キー | 型 | 既定値 | 意味 |
|---|---|---|---|
| `auto` | u8 | `1` (`CONFIG_TAB5_OTA_ENABLED` に追従) | 自動更新の ON/OFF |
| `base_url` | str | (空 = Kconfig 値) | ポーリング先の基底 URL |
| `pinned_tag` | str | (空) | 指定 tag のみ受け入れる |
| `rollback_cnt` | u32 | 0 | 累積ロールバック回数 |

詳細な設計 / 脅威モデルは
[docs/OTA_UPDATE_DESIGN.md](docs/OTA_UPDATE_DESIGN.md) を参照。

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
