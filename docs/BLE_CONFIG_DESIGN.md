# BLE 設定サービス — 設計メモ / Design notes

`components/ble_config_service/` を参照。`CONFIG_TAB5_BLE_CONFIG_ENABLED=y`
で組み込まれる、Web Bluetooth ベースのプロビジョニング GATT サーバー。

## 1. なぜ BLE か / Why BLE

Tab5 のリリース zip / Web Flasher を「焼くだけ」で済ませるために、Wi-Fi /
SSH / Tailscale の各種シークレットを後からデバイス側で設定する手段が要る。
候補は 3 つ:

- **Wi-Fi softAP + HTTP** — 設定中だけソフト AP を立てる方式。利点は
  ブラウザサポートが広いこと。欠点は Wi-Fi 同時利用の切り替えが煩雑
  (sta から ap モードへ落とすため STA 接続が一旦切れる)、Tab5 を
  既存ネットワークに接続している環境ではユーザーが Wi-Fi を AP に
  繋ぎ直す必要がある。
- **シリアル CLI** — 出荷後の利用者にケーブル要求は厳しい。
- **BLE GATT + Web Bluetooth** — ペアリング不要、ブラウザから直接、
  既存 Wi-Fi の動作と独立。Chrome / Edge 133+ が必要だが、対象ユーザー
  (Tab5 を買うエンドユーザー) はこの制約をだいたい満たす。

stackchan-idf で運用実績があり、暗号セッション層 (X25519 + HKDF +
AES-256-GCM) と関連ヘルパーをほぼそのまま流用できるので、最終的に
BLE を選択した。

## 2. ハードウェア制約 / Hardware constraints

ESP32-P4 は **オンチップ Bluetooth ラジオを持たない**。Tab5 では同基板の
ESP32-C6 を `esp_hosted` 2.x ファームウェアでスレーブにし、SDIO で
HCI を中継する (Wi-Fi と同じ経路)。ホスト側は NimBLE を VHCI
(Virtual HCI) トランスポート上で走らせる:

```
ESP32-P4 (host)         ESP32-C6 (slave)
+--------------+ SDIO  +---------------+
|  NimBLE host | <---> | esp_hosted    |
|  (this code) |       |  BT controller|
+--------------+       +---------------+
```

リファレンス実装は managed_components 内の
`espressif__esp_hosted/examples/host_nimble_bleprph_host_only_vhci`。本
コンポーネントは Kconfig で `BT_ENABLED` / `BT_NIMBLE_ENABLED` /
`BT_CONTROLLER_DISABLED` / `ESP_HOSTED_ENABLE_BT_NIMBLE` /
`ESP_HOSTED_NIMBLE_HCI_VHCI` を有効化する。

**C6 スレーブファームウェア要件**: BT コントローラー込みでビルドされて
いる必要がある。`c6_updater/` で配布している esp_hosted 2.x スレーブは
現時点では Wi-Fi のみ。`CONFIG_TAB5_BLE_CONFIG_ENABLED=y` でビルドした
ファームウェアは BLE を試みるが、C6 側が BT 非対応の場合は
`nimble_port_init()` が失敗し、`Error::NimbleInit` を返して
プロビジョニングは無効のまま起動を継続する (致命的にはならない)。

## 3. プロトコル / Protocol

セッション暗号は stackchan のものをそのまま採用。`info` 文字列だけ
`tab5-ble-config-v1` に変更。

### Handshake

```
Central (browser)                Device (Tab5)
  --- READ KeyExchange  ---->     ensure_device_keypair()
  <-- 32 B device_pub  -----      = X25519 epubkey
  generate central_kp
  shared = X25519(central_priv, device_pub)
  --- WRITE KeyExchange ---->     complete_handshake(central_pub)
      (32 B central_pub)          shared = X25519(device_priv, central_pub)
  aes_key = HKDF-SHA256(           aes_key = HKDF-SHA256(
    ikm=shared,                      ikm=shared,
    salt=<see below>,                salt=<NVS auth_salt or empty>,
    info="tab5-ble-config-v1",       info="tab5-ble-config-v1",
    L=32)                            L=32)
```

ハンドシェイク後の全てのキャラクタリスティック R/W は
`[12 B nonce][ciphertext][16 B GCM tag]` 形式 (AES-256-GCM、AAD 無し)。

### Password gate (`AuthPassword`)

`AuthPassword` キャラクタリスティックに非空文字列を書き込むと、
`SHA-256(password)` を NVS の `ble_cfg/auth_salt` に保存する。次回以降の
接続では `Session::set_hkdf_salt(salt)` が呼ばれ、デバイス側は
`HKDF(ikm, salt=SHA256(pwd), info=...)` で AES 鍵を作る。

クライアント側 (settings.html) は salt なしのハンドシェイクで一度
試行し、最初の暗号化された READ が失敗した時点でパスワードを問い、
`SHA-256(pwd)` を salt にして再ハンドシェイクする。鍵が一致しない
場合は GCM tag mismatch で必ず READ が失敗するので、識別可能。

空文字列で WRITE するとデバイス側で salt をクリア → 認証なし状態に戻る。

## 4. GATT キャラクタリスティック / Characteristics

Service UUID: `b5a50000-9c1b-4f3d-8a2e-1f6c4e9b7d0a`
ベース UUID の 3 バイト目 (`xx` の位置: `b5a5xxxx-9c1b-4f3d-8a2e-1f6c4e9b7d0a`)
を差し替えて 12 個の特性を派生:

| Suffix | Name             | Plaintext? | R/W   | Notes |
|--------|------------------|------------|-------|-------|
| `0001` | KeyExchange      | yes        | R + W | 32 B X25519 pubkey |
| `0002` | WifiSsid         | encrypted  | R + W | max 32 B |
| `0003` | WifiPsk          | encrypted  | W     | max 63 B |
| `0004` | TailscaleAuthKey | encrypted  | W     | tskey-auth-... |
| `0005` | TailscaleHostname| encrypted  | R + W | max 32 B |
| `0006` | SshHost          | encrypted  | R + W | max 64 B |
| `0007` | SshPort          | encrypted  | R + W | uint16 LE |
| `0008` | SshUser          | encrypted  | R + W | max 32 B |
| `0009` | SshPassword      | encrypted  | W     | max 64 B |
| `000a` | AuthPassword     | encrypted  | W     | empty body clears gate |
| `000b` | Status           | plaintext  | R + N | 3 B = [wifi, ts, ssh] |
| `000c` | Apply            | encrypted  | W     | non-empty body → esp_restart |

- READ-only キャラクタリスティック (`Psk` / `TailscaleAuthKey` / `SshPassword` /
  `AuthPassword` / `Apply`) は READ で 0 バイトを返す (機密を漏らさない)。
- ハンドシェイク完了前の暗号化キャラクタリスティック READ は 0 バイトを返す
  (`session.is_established()` が false の間)。
- Status は NOTIFY も配信する。`notify_status(Status&)` を呼ぶと subscribe して
  いる central に 3 バイト struct がプッシュされる。

書き込みは **即座に NVS へ反映** される (Apply 待ちではない)。Apply は
あくまで「設定を反映するために再起動する」トリガ。

## 5. NVS namespaces

新しい namespace は最小限。書き込み先は既存のモジュールに合わせる:

| 領域 | namespace | キー | 経路 |
|------|-----------|------|------|
| Wi-Fi SSID/PSK | (`main/wifi_config` 内部) | (`main/wifi_config` 内部) | `tab5::wifi_config::set` |
| SSH host/port/user/password | `"conn"` (profiles) | 既存 (`slot_0` blob) | `tab5::profiles.update(0, p)` (空の場合は `add` + `select`) |
| Tailscale auth_key / hostname | `"tailscale"` | `auth_key` / `hostname` | 直接 `nvs_set_str` |
| BLE 認証 salt | `"ble_cfg"` | `auth_salt` (32 B blob) | 直接 `nvs_set_blob` |

SSH 認証種別 (`SshAuth`) は本 MVP では Password 固定。Pubkey 配布は対象外。

## 6. 脅威モデル / Threat model

- **セッション鍵は ephemeral**。接続ごとに新しい X25519 keypair を生成し、
  切断時に `Session::reset()` で全て zeroize する。LTK / bonding は使わない。
- **MITM 防御は無い**。central が初回接続するときに peer pubkey の真正性を
  確認する手段がない (TOFU すらしない)。攻撃者が同名の偽 GATT サーバーを
  立て、central と Tab5 の間で proxy すれば成立する。これは stackchan と
  同じ割り切り — BLE GATT 設定は「物理的に隣にいる人だけが操作する想定」
  で、リモート攻撃のリスクは小さい。
- **`AuthPassword` ゲート** は「家庭内 LAN 程度のうっかり侵入防止」の用途。
  パスワードは平文 hash で NVS に保存される (フラッシュダンプ攻撃には
  耐えない)。

将来的に「固定の長期鍵を flash に焼いておきペアリング相当の信頼を持たせる」
仕組みを足す余地はあるが、現状の MVP ではスコープ外。

## 7. オープン課題 / Open issues

- **C6 スレーブ BT 対応**。`slave_c6_fw/` / `c6_updater/` で配布している
  esp_hosted 2.x スレーブに BT コントローラーを足す必要がある。これが
  完了するまで本サービスは「ビルドは通るが起動時に Error::NimbleInit を
  返して無効」のままとなる。
- **ブラウザ対応**。Chrome / Edge 133+ で動作。iOS Safari、Firefox は
  Web Bluetooth 非対応なので使えない。
- **設定の一部しかカバーしていない**。VPN 種別 (WireGuard vs Tailscale)、
  Tailscale control server、WireGuard 鍵などは現時点 BLE 経由では設定
  できない。必要に応じて特性を追加する。
- **将来課題: ペアリング相当の信頼**。固定 long-term identity を flash に
  焼き込んで、Web Bluetooth UI 側でも初回接続時にその pubkey を保存し、
  次回以降の MITM をブロックする仕組み。

## 8. ファイル一覧 / Files

```
components/ble_config_service/
├── CMakeLists.txt            REQUIRES + MINIMAL_BUILD 互換の include 明示
├── Kconfig.projbuild         CONFIG_TAB5_BLE_CONFIG_ENABLED + 関連 select
├── include/ble_config/
│   ├── ble_config.hpp        public API (start / notify_status / is_connected)
│   └── crypto.hpp            Session (X25519 + HKDF + AES-256-GCM)
└── src/
    ├── crypto.cpp            stackchan からのほぼ移植 (namespace / info 変更)
    ├── gatt_service.hpp      ble_config.cpp ↔ gatt_service.cpp 間の private API
    ├── gatt_service.cpp      GATT サーバー本体 + 各特性アクセスハンドラ
    └── ble_config.cpp        NimBLE bring-up + GAP イベント処理

docs/
├── settings.html             Web Bluetooth 設定 UI (単一ファイル)
└── BLE_CONFIG_DESIGN.md      この文書
```
