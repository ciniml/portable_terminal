# OTA (Over-the-Air) 更新設計ノート

`tab5_claude_client` (M5Stack Tab5 / ESP32-P4, ESP-IDF 6.0) の
ネットワーク経由ファームウェア更新機構の設計案。
実装コードは含まない — 方針を固めるためのドキュメント。

対象コミット時点: `62cf6b1` (tag-driven release + Web Flasher)。

## 前提のおさらい

- Partition (`partitions.csv`)
  - `ota_0` @ `0x10000`, 3 MB / `ota_1` @ `0x310000`, 3 MB
  - `otadata` @ `0xd000`, 8 KB
  - `storage` (spiffs) @ `0x610000`, 1 MB
  - Flash 16 MB — `0x710000` 以降は未使用 (~9 MB)
- App バイナリは現状 ~2.4 MB (両 OTA スロットに収まる)
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (rollback は有効)
- CI (`.github/workflows/release.yml`) が tag push で
  `firmware-<tag>.zip` を GH Release に添付。ZIP 内には
  `tab5_claude_client.bin` (アプリ本体, `0x10000`) と単一ファイル
  `firmware-<tag>.bin` (`0x0`) が含まれる。
- Pages ワークフロー (`.github/workflows/pages.yml`) は
  `_site/firmware/<tag>/firmware-<tag>.zip` を配置し、
  `versions.json` に `{tag, zip}` の順序付きリストを書き出す。
- ランタイム側は Wi-Fi (ESP32-C6 via esp_hosted 2.x) と
  Tailscale/WireGuard がすでに立ち上がる (`main/vpn.cpp`,
  `main/wifi_setup.cpp`)。

---

## 1. 選択肢の比較

Tab5 で現実的な 5 系統 + 参考 1 系統を並べる。
「フィット」= Tab5 の Wi-Fi / Tailscale 環境で自然に流れるか。
「セキュリティ」= 転送路 + 発行者検証。
「ストレージ」= flash / RAM の追加コスト。
「複雑さ」= 実装 + 運用。

### 1.1 HTTPS pull from GitHub Releases (直接)

`https://github.com/<owner>/<repo>/releases/download/<tag>/tab5_claude_client.bin`
を `esp_https_ota` で pull する。

- フィット: **極めて良い**。Wi-Fi が立ち上がった時点で GitHub は
  グローバルに reachable。Tailscale 不要。
- セキュリティ:
  - 転送路 = TLS。`esp_crt_bundle_attach` で Mozilla ルート束を
    埋め込んで検証 (追加 flash ~200 KB)。GitHub のリダイレクトは
    `objects.githubusercontent.com` へ抜けるので `esp_http_client`
    の `disable_auto_redirect=false` + 3xx 追跡が必要
    (`esp_https_ota` はデフォルトで追跡する)。
  - 発行者検証 = 現状は「TLS 経由で GH Releases に置かれた
    ものは真」と信じる (=リポジトリ所有権に依拠)。厳密には
    `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` = ECDSA 署名を
    有効化して CI が署名する形が理想 (§2.3 参照)。
- ストレージ: cert bundle + esp_https_ota のみ。追加 ~200 KB flash,
  ~8 KB RAM (`buffer_size` 既定 1024 + TLS state)。
- 複雑さ: **最低**。ESP-IDF 標準コンポーネント (`esp_https_ota`)
  だけで完結。ヘッダは `~/esp-idf/6.0/components/esp_https_ota/include/esp_https_ota.h` —
  `esp_https_ota_begin/perform/finish` の 3 段で回すか、
  一発の `esp_https_ota()` 呼び出しでも OK。
- 典型的な失敗:
  - GH の maintenance / rate limit → `esp_https_ota_perform`
    が `ESP_ERR_HTTP_EAGAIN` を返すのでリトライ可能。
  - Wi-Fi flap → 途中で `recv` エラー → セッション破棄。
    `partial_http_download=true` + `max_http_request_size` を
    使うと HTTP Range で分割 GET になり、フラップに強くなる
    (ただし各 chunk で TLS handshake 再発)。フル resume は
    ネイティブには効かない (ota partition の書き込みは
    accumulate されるので、`esp_ota_write` の途中状態は残るが
    次回起動時にやり直し)。
  - `objects.githubusercontent.com` の cert が Mozilla bundle に
    含まれていない場合 → 起きない (DigiCert / Sectigo なので束に
    ある)。ただし GitHub が CA を切り替えた際に古い bundle だと
    落ちるので、bundle を版に含めておく (今回は IDF 同梱で OK)。

### 1.2 HTTPS pull from GitHub Pages

`https://<owner>.github.io/<repo>/firmware/<tag>/firmware-<tag>.zip`
に ZIP がすでに staging されている。ただし ZIP を on-device で
展開するのは高コスト (miniz を link する必要あり + RAM 使用)。
`.bin` を単体で置くように pages ワークフローを 1 行追加すれば
1.1 と等価な pull が可能:

```
_site/firmware/<tag>/tab5_claude_client.bin  (アプリのみ、OTA 用)
_site/firmware/<tag>/firmware-<tag>.zip       (フル一式、Web Flasher 用)
_site/latest.json                             (最新 tag / URL / sha256)
```

- フィット: 1.1 と同じ。Pages の CDN (Fastly) が入るので体感は
  むしろ早い。CORS は関係ない (デバイス直で `esp_http_client`)。
- セキュリティ: 1.1 と同じ。Pages ドメインの証明書検証。
- 複雑さ: **低**。Pages ワークフローに `.bin` 1 本のコピーと
  `latest.json` 生成を足すだけ。
- 失敗モード: Pages のデプロイ遅延 (release publish 後 1〜数分)
  で latest.json が古いことがある — poll 間隔を分単位に。

### 1.3 Tailscale-served HTTPS

tailnet 内のピア (例 `prometheus`) が `python -m http.server` 相当
で `.bin` を serve、Tab5 が Tailscale 直 UDP で pull。

- フィット: **良いが用途限定**。GH がブロックされている環境
  (社内 / 検閲 / エアギャップ tailnet) の fallback として有効。
  Direct UDP が立てば 200 KB/s 出る (Tailscale ノート §Phase 7)、
  DERP relay 経由だと 32 KB/s なので更新に時間はかかる。
- セキュリティ: WireGuard による回線暗号化 + peer 認証があるので
  TLS すら不要。ただし Tailscale ACL / tailnet-lock を運用しないと
  「同じ tailnet の悪意ある peer」から任意バイナリを渡され得る。
- ストレージ: Tailscale スタックはすでに常駐。追加コストは
  `esp_http_client` の HTTP (not HTTPS) パスのみ。
- 複雑さ: **中**。運用側にサーバを立てる必要があり、URL 設定を
  Kconfig / NVS に持たせるので UI の口も増える。
- 失敗モード: peer down、DERP→direct 昇格前のフェーズ、
  Tailscale 未認証 (Awaiting Auth) 時に blocking しない設計が必要。

### 1.4 BLE OTA

`feature/defaults-and-ble` に `components/ble_config_service/` が
sketched されている前提。BLE GATT の write-without-response 特性に
チャンクを流し込む古典パターン (M5Stack stackchan 系と同型)。

- フィット: **今の main では不成立**。BLE ブランチは main 未マージ、
  かつ ESP32-P4 は BLE を内蔵しない — BLE は on-board C6 経由で
  やる必要があり、esp_hosted 2.x で BLE を通す構成は非自明
  (`esp_hosted` は Wi-Fi + Bluetooth HCI transparent 化を謳うが、
  スレーブ firmware 側で BT stack が有効かは build による)。
- セキュリティ: pairing/bonding 必須。カスタム CRC + resume は
  自前実装。
- ストレージ: BLE stack (Bluedroid / NimBLE) + GATT サービスで
  数十〜100 KB。
- 複雑さ: **高**。GATT chunker + 進捗 + retry + on-device UI
  (ペアリング承認)。
- 失敗モード: 距離 / 干渉での flap は BLE の宿命。数分〜数十分の
  更新中に落ちるのは覚悟する必要あり。
- 用途: Wi-Fi のない環境向けの緊急パス。**当面は out of scope**。

### 1.5 HTTP OTA on softAP

Tab5 が softAP を立て、内蔵 HTTP サーバに phone / laptop から
`.bin` を POST。

- フィット: **中**。初期セットアップ / Wi-Fi 未設定機の救済に有効。
  ただし `esp_wifi_remote` (esp_hosted 経由) で AP+STA 同時運用が
  安定して動くかは要検証 — C6 の firmware が AP mode を許可する
  構成である必要がある。
- セキュリティ: SoftAP の WPA2 + on-device で表示するランダム PSK。
  HTTP は平文だが、SoftAP に入れる人 = 物理的に近い人なので
  用途としては許容。
- ストレージ: `esp_http_server` を追加 (~15 KB)。
- 複雑さ: **中**。HTML アップロード UI (端末では multipart 受けが
  必要) と進捗表示。
- 失敗モード: STA と AP を同時にやると通信品質が落ちる、
  接続元の OS が captive portal 検出で切断、など運用臭が強い。
- 用途: 初期セットアップ / Wi-Fi 変更のブリッジとして 1.1/1.2 とは
  独立に持てる補助線。**Phase 2 の候補**。

### 1.6 Delta / diff OTA

- IDF 6.0 時点で公式 delta OTA は無い (`esp_delta_ota` は
  `esp-idf-extra-component` にある「参考実装」で API 安定性 unclear、
  bsdiff patch を on-device で apply)。
- Tab5 のアプリは 2.4 MB / 差分は多くの場合 100〜500 KB に落ちるが、
  patch 適用中に PSRAM を数 MB 消費する。
- **今回は out of scope**。将来アプリが 5 MB 級になったら再検討。

### 1.7 ランク付け

1. **HTTPS from GitHub Pages (`.bin` + `latest.json`)** — 主線推奨
2. **HTTPS from GitHub Releases 直** — Pages が落ちた時の代替 URL
3. **Tailscale HTTP** — GH 到達不能環境の運用用 fallback
4. **SoftAP HTTP** — 初期セットアップ用 (Phase 2)
5. **BLE OTA** — 主 branch にマージされてから再検討
6. **Delta OTA** — 保留

---

## 2. 主線: GitHub Pages HTTPS pull + rollback

### 2.1 全体フロー

```
[boot] → Wi-Fi up → OTA task 起動
        └─ 初回 100 s 後に latest.json を GET (jitter 付き)
        └─ 以降 6 h 毎に GET (±10 % jitter)

[poll] latest.json → { tag, bin_url, sha256, min_prev_tag? }
        └─ 現行 esp_app_desc::version と compare
        └─ 更新あり かつ NVS の enable フラグが true → 更新開始

[update]
        1. status_bar に "Updating x%" を出す
        2. esp_https_ota_begin(bin_url, cert_bundle)
        3. esp_https_ota_get_img_desc() → chip_id / project_name チェック
        4. loop: esp_https_ota_perform() + progress 発火
        5. esp_https_ota_finish() → next-boot flag が pending_verify に
        6. 遅延リブート: アクティブな remote 接続が idle 状態
           (send/recv 30 s 無し) を待って esp_restart()
           — SSH セッション途中の切断を避けるため

[post-boot]
        - 起動後 120 s、UI / VPN / SSH が全部立ち、かつ status_bar が
          "healthy" (Wi-Fi connected, VPN up ならその状態) を保った時点で
          esp_ota_mark_app_valid_cancel_rollback()
        - それまでに reboot / hang すれば bootloader が rollback
```

### 2.2 トリガ

- **手動**: 既存 modal menu (`main/menu.cpp`) に "Firmware Update"
  項目を追加。押下で latest.json を即 fetch → 更新確認ダイアログ。
- **自動 (opt-in)**: NVS 上の `auto` フラグ (default: **off** を
  推奨。ユーザ環境が不安定な可能性があるため保守的に)。
  ON なら 6 h 毎 + boot 100 s 後の一発。
- **自動の失敗モード対策**:
  - **rollback ループ**: `esp_ota_get_state_partition` で
    現在 slot の状態が `ESP_OTA_IMG_INVALID` の間は自動更新を
    抑止 (前回の失敗 image が残っているため)。
  - **network flap 中の更新開始**: RSSI / 直近 5 分の
    切断回数を見て、不安定なら skip。
  - **brownout 中の flash 書き込み**: `M5.Power.getBatteryLevel()`
    が 20 % 未満 かつ充電中でない場合は skip。
    (`main/status_bar.cpp` がすでにこの値を読んでいる。)
  - **更新ラッシュ**: `min_prev_tag` が JSON にあれば
    「この tag 以降からしか自動更新しない」ガードにできる
    (壊れた履歴 tag からの直接ジャンプを防止)。

### 2.3 バージョン検出 + 発行者検証

- 現行版: `esp_app_desc_t::version` を `esp_ota_get_app_description()`
  で取得。CI が `version.txt` を書いてから build するので tag が
  そのまま入る (`release.yml:78-88`)。
- 更新判定: `strcmp(current, latest.tag) != 0` かつ semver で
  latest ≥ current (semver パーサ簡易実装 or 純文字列比較で妥協)。
- **cert 検証**: `esp_crt_bundle_attach`。GH Pages は `github.io`
  ワイルドカード + Fastly の証明書。IDF 同梱束で足りる。
- **image 検証**: `esp_https_ota` は download 中に
  `esp_app_desc` を先頭 chunk からパースして chip_id (P4) と
  `project_name` == "tab5_claude_client" を照合する
  (`CONFIG_ESP_HTTPS_OTA_DECRYPT_CB` オフ, 標準経路)。
- **署名**: 現状は「TLS + repo 所有者信頼」で運用可能だが、
  漏洩耐性を上げたい場合は
  `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT=y` + ECDSA 鍵で
  CI が sign_data を追加 (`espsecure.py sign_data --version 2`)。
  Secure Boot は要らないので鍵はソフトウェア署名のみ。
  - Trade-off: 鍵管理を運用側に持つ必要が出るので、初手では
    入れない。Phase 2 で GH Actions secret に private key を
    置いて自動署名する形が現実的。

### 2.4 Rollback との連携

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` により、更新後の
  最初の起動 partition は `ESP_OTA_IMG_PENDING_VERIFY` 状態。
  この状態で `esp_ota_mark_app_valid_cancel_rollback()` を
  呼ばずに reboot すると bootloader が自動で前スロットに戻す。
- 呼び出しタイミング (提案):
  - Wi-Fi 接続成功 + (VPN 有効なら VPN up) + 起動から **120 秒**
    経過 + 直近 30 秒 crash 無し。
  - 実装場所は OTA task 内の post-boot チェック。あるいは
    `main/app_main.cpp` の boot_task 末尾に 1 回だけタイマー。
- **手動 rollback**: menu に "Rollback firmware" 項目。内部で
  `esp_ota_mark_app_invalid_rollback_and_reboot()`。

### 2.5 UI

`main/status_bar.cpp` に 1 行追加:

- 通常時: 既存の battery / Wi-Fi / SSH / uptime。
- 更新チェック中: 右下に `OTA: checking...`。
- ダウンロード中: `OTA: 42% (1.0/2.4 MB)` + 細い progress bar。
  `esp_https_ota_get_image_len_read()` / `_get_image_size()` で
  分子分母が取れる。5 s の refresh に加え、OTA task が
  `status_render()` を明示 kick して 1 s 毎に更新。
- 完了: `OTA: rebooting in Ns...` — カウントダウン。
- 失敗: `OTA: failed (retry in Nh)` を 30 s 表示。

Modal menu 側 (`main/menu.cpp`) にも "Firmware" タブを追加して:
- 現行 version / 最新 version / 更新チャネル (Pages / Releases /
  Tailscale) の設定 + 手動 "Update now" / "Rollback" ボタン。

### 2.6 NVS gate

新規 namespace `"ota"`:

| key | 型 | 意味 | default |
|---|---|---|---|
| `auto` | u8 | 自動更新 ON/OFF | 0 |
| `channel` | u8 | 0=pages, 1=releases, 2=tailscale | 0 |
| `url_base` | str(128) | tailscale チャネル時のみ | "" |
| `pubkey` | blob | (将来) 署名検証用 ECDSA pubkey | 空 |
| `last_check_ts` | u64 | 最終 poll 時刻 (unix) | 0 |
| `last_fail_count` | u8 | 連続失敗回数 (backoff 用) | 0 |
| `pinned_tag` | str(32) | 手動で pin する tag | "" |

`main/menu.cpp` の設定 UI から編集可能に。

---

## 3. 統合計画

### 3.1 新規ファイル

- `main/ota_task.hpp` / `main/ota_task.cpp`
  - API (sketch — 実装は書かない):
    - `void start_ota_task();` — boot_task の末尾から起動
    - `bool ota_check_now(bool user_initiated);` — menu から手動
    - `bool ota_rollback_now();` — menu の "Rollback" から
    - `enum class OtaState { Idle, Checking, Downloading, Rebooting,
      Failed };` と `struct OtaStatus { OtaState st; int pct;
      char msg[64]; };` を expose し、`status_bar.cpp` が読む
    - `void ota_notify_healthy();` — boot_task 末尾で呼ぶ
      (mark_app_valid_cancel_rollback のトリガ)
- `main/ota_channel.hpp` / `.cpp`
  - `latest.json` fetch + parse (cJSON はすでに managed_components
    に依存として引かれている)。channel 分岐の実装。
- (任意) `main/ota_signing.hpp` / `.cpp`
  - Phase 2 で ECDSA 検証を入れる場合の口。今回は空でよい。

### 3.2 既存ファイルの変更点

- `main/app_main.cpp`
  - `do_boot_sequence()` の末尾 (Done ステージ後) に
    `start_ota_task()` を追加。
  - boot_task が正常終了する経路で `ota_notify_healthy()` を
    呼ぶ (mark_app_valid_cancel_rollback のトリガ)。
- `main/status_bar.cpp`
  - `render()` 内で `ota_get_status()` を読み、
    Downloading 状態のときは既存の battery/Wi-Fi 行の下に
    OTA 行 + progress bar を描画。
- `main/menu.cpp`
  - "Firmware" タブ追加 (今の profiles / Wi-Fi / Tailscale 並び)。
  - Update now / Rollback / auto toggle / channel select /
    現行 version 表示。
- `main/Kconfig.projbuild`
  - `TAB5_OTA_ENABLED` (default y)
  - `TAB5_OTA_DEFAULT_CHANNEL` (choice: pages/releases/tailscale)
  - `TAB5_OTA_DEFAULT_URL_BASE` (string, blank → GH derived)
  - `TAB5_OTA_POLL_HOURS` (int, default 6)
  - `TAB5_OTA_HEALTHY_DELAY_S` (int, default 120)
- `main/CMakeLists.txt`
  - `ota_task.cpp` / `ota_channel.cpp` 追加。
  - `REQUIRES esp_https_ota esp_http_client app_update
    esp-tls` (bundle は `esp_crt_bundle` 追加)。
- `sdkconfig.defaults`
  - `CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=n`
  - `CONFIG_ESP_TLS_USING_MBEDTLS=y` (すでにそう)
  - `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y`
  - `CONFIG_ESP_HTTPS_OTA_DECRYPT_CB=n`

### 3.3 CI / Pages ワークフロー変更

`release.yml` (**別 PR で** — 本設計は「変更が要る」を書くだけ):

1. Stage 時に `_site/firmware/<tag>/tab5_claude_client.bin` として
   アプリバイナリを平置きコピー。
   - 具体的には `release` step で
     `cp build/tab5_claude_client.bin` を追加。
2. `latest.json` の生成:

   ```json
   {
     "tag": "v0.3.0",
     "bin_url": "https://<owner>.github.io/<repo>/firmware/v0.3.0/tab5_claude_client.bin",
     "sha256": "…",
     "size": 2412544,
     "project_name": "tab5_claude_client",
     "chip_id": "esp32p4",
     "min_prev_tag": "v0.2.0"
   }
   ```

3. `pages.yml` の後段で最新 release の `latest.json` を
   `_site/latest.json` として書き出し。手動 pin 用に
   per-tag ディレクトリにも同じ JSON を置いておく。
4. **canary**: `latest.json` は tag push で自動更新しない
   運用にする選択肢もある — 別ワークフロー
   `promote-latest.yml` を workflow_dispatch (input=tag) で
   持って、人間が「fleet に流していい」と判断してから叩く。
   小規模なら不要、fleet が育ってきたら導入。

### 3.4 ドキュメント

- `README.md` / `README.ja.md`:
  - 「OTA 更新」節を追加。手順 (menu → Firmware → Update now)、
    auto の有効化方法、rollback 手順、channel 切替。
- `docs/index.html` (Web Flasher):
  - 既存の Web Flasher は「クリーンな初期化」用として残す。
    OTA が動かなくなった時の fallback として周知。
- `docs/OTA_UPDATE_DESIGN.md` (本ドキュメント) を README から
  crosslink。

---

## 4. Threat model + open questions

### 4.1 `.bin` DL は完了したが flash が失敗

- ケース: mbedTLS OK, HTTP 200 で受信完了、`esp_ota_write` の
  途中で verify error / partition erase 失敗。
- 対応: `esp_https_ota_finish` が失敗を返す。partition は
  半端に書かれているが、boot_data は切り替わっていないので
  現行 slot がそのまま起動する。副次: 次回 OTA 時に
  `esp_ota_begin` で該当 partition を頭から erase するので
  影響なし。→ NVS の `last_fail_count++`、backoff。

### 4.2 SSH / Tailscale 通信中の再起動

- ケース: SSH セッション中に OTA が終わり `esp_restart` すると
  操作中の shell が死ぬ — 実用上は嫌がられる。
- 対応:
  - `IConnection` に is_idle 判定を用意 (直近 30 s 送受 0 byte)。
  - `active_connection()` があれば idle を条件に reboot 遅延。
  - 最大 30 分待って強制 reboot (更新済み image が延々放置される
    のは危険なので上限は必要)。
  - ユーザ手動更新の場合は「今すぐ再起動 / 5 分後」を確認モーダル。

### 4.3 同時 DL のうち片方だけ flash 失敗

- ケース: 複数台が同じ `.bin` を pull。1 台は成功、1 台は途中で
  brownout / TLS reset。
- 対応: 失敗機は 4.1 と同じ経路で rollback 対象にならず、
  ただ backoff で次回リトライ。**backoff は指数**
  (1 h → 2 h → 4 h → cap 24 h) + jitter ±20 %。fleet で同時 hit を
  避けるため poll 時刻に既定で jitter を入れる。

### 4.4 発行元 (GH / Pages / tailscale peer) ダウン

- `latest.json` fetch 失敗 → `last_check_ts` を更新しない
  (次回 poll 時刻から逆算されるので早めに再試行)。
- `.bin` fetch 失敗 (途中) → `esp_https_ota_abort` で partition
  cleanup、backoff。
- pages と releases が両方落ちるのは実質 GH 全落ち。
  Tailscale channel を有効にしている機だけ生き残る。

### 4.5 全機ブリック (bad release)

- 個別機は bootloader rollback で救われる。
- ただし「pending_verify のまま healthy 判定に到達し、
  120 s 経過後に mark_app_valid が走ってから hang するバグ」の
  ようなケースは rollback 対象外 (もう valid になっている)。
  → healthy 判定を厳しめにする + `TAB5_OTA_HEALTHY_DELAY_S` を
  短くしすぎない (120 s は下限)。
- Fleet 対策: canary rollout。`latest.json` を人間承認で bump
  (§3.3 の promote-latest ワークフロー)。数台が数日「新版で
  healthy」を維持したのを見てから general availability に上げる。
- キル/ピン機構: NVS の `pinned_tag` を UI から入れると
  auto は無効化 (= 動作中の版を維持)。

### 4.6 `esp_https_ota` の network flap 挙動 (要検証)

- IDF ソース (`~/esp-idf/6.0/components/esp_https_ota/src/esp_https_ota.c`)
  を読むと、`esp_https_ota_perform` は 1 read = 1 chunk で
  `esp_ota_write` に流す。`recv` が失敗すると
  `ESP_ERR_HTTP_EAGAIN` (data 未着) or 致命エラーを返す。
  EAGAIN は同関数を再呼び出しでリトライ可能 (poll loop で回す
  想定)。**しかし TCP セッションが切れた場合はリトライ不可** —
  ハンドルを abort → begin し直しになる。
- `partial_http_download=true` + `max_http_request_size=64K` に
  すると、内部で HTTP Range で分割 GET する。各 GET を独立して
  やり直せるので、一度切れても次の chunk からリトライ可能。
  ただし TLS handshake が chunk 毎に発生 (数百 ms オーダ)。
- 提案: `partial_http_download=true` + `max_http_request_size =
  262144` (256 KB) — flap 耐性と handshake overhead のバランス。

### 4.7 未解決 / 保留

- **署名鍵の管理**: GH Actions secret に private key を置く運用
  で足りるか、HSM 相当 (KMS) が要るか。fleet 規模で決める。
- **時刻**: TLS 検証には妥当な wall clock が必要。boot 時点で
  SNTP が走る保証が無い場合、latest.json fetch 前に SNTP sync
  を強制するかどうか (Tailscale が既にやっている経路もある)。
- **A/B partition 満杯化**: アプリが 3 MB を超えたら partition
  レイアウト変更 (ota_0 / ota_1 を 4 MB に、storage を 512 KB に)
  が必要。partitions.csv 変更は OTA 経由での適用が難しい (partition
  table 自体は OTA 対象外) → 次回大幅増加時は事前に partitions.csv
  を先行リリースする段階的移行が要る。
- **`versions.json` の後方互換**: Web Flasher が使っている
  `{tag, zip}` フォーマットを壊さない (追加のみ)。OTA 用は
  別ファイル `latest.json` にして責務分離。

---

## 5. スコープ外 (このドキュメントで扱わない)

- 実装コード (`main/ota_task.cpp` などは書かない)。
- production ファイル (`main/`, `components/`) の変更。
- CI ワークフロー (`release.yml`, `pages.yml`) の変更。
- BLE OTA と delta OTA (§1.4, §1.6 参照 — 保留)。

以上は別 PR で段階的に実装する前提。まずはこの設計に対する
レビュー / 合意を経てから、`ota_task` の骨組みだけを入れる
小さな最初の PR を作るのが自然な段取り。

---

## 6. Implementation notes (2026-07)

このリビジョンで `main/ota_task.{hpp,cpp}` を実装した。実装上、
設計案から意図的にずらした / 保留した部分:

- **API 名は `tab5::ota::start/kick/snapshot`** に統一。`Trigger::Auto`
  / `Trigger::Manual` を Enum で切り分ける形にし、`OtaStatus` 相当の
  `Status` 構造体を `snapshot()` から返す。
- **NVS 名前空間 = `"ota"`**、キーは `auto` / `base_url` / `pinned_tag` /
  `rollback_cnt`。§2.6 の表にあった `channel` / `url_base` / `pubkey` /
  `last_check_ts` / `last_fail_count` は未実装 (channel = GitHub Pages
  固定 + backoff/streak はプロセス内 atomic で保持 — 電源断で失敗回数が
  0 に戻る挙動を許容している)。
- **channel 分岐 (§3.1 の `ota_channel.hpp/cpp`) は未実装**。設計時点
  では pages / releases / tailscale の切替を持たせる想定だったが、URL
  1 本 + `base_url` overrides で足りるので簡素化。
- **§2.3 の「brownout / 電池 20% ガード」「network flap ガード」は未実装**
  (先送り)。Wi-Fi が落ちた状態で `esp_https_ota_begin` が失敗すると
  streak が上がり指数バックオフに入るのでソフトには守れている。
- **`is_complete_data_received` を `esp_https_ota_finish` の前に呼び、
  途中打ち切りを検出**。設計案では明示していなかった。
- **署名検証は入れていない** (§2.3 のとおり Phase 2)。TLS + repo 所有者
  信頼で運用。`sha256` は `latest.json` から読み込みは出来ているが、
  ダウンロード側でオンザフライ検証は入れていない (esp_https_ota が
  chip_id / project_name / secure_version の検証を回している)。
- **§2.4 の rollback 呼び出し条件を単純化**: 現在は「起動から 120 秒
  経過」だけを条件に `esp_ota_mark_app_valid_cancel_rollback` を呼ぶ。
  「Wi-Fi 接続 + VPN 有効」を条件に含めると、更新後に Wi-Fi 圏外に
  出た端末がロールバックで戻ってしまい "更新済みだが reachable でない"
  状態が固定化しかねないため、意図的に緩めた。
- **§2.4 の手動 rollback (`esp_ota_mark_app_invalid_rollback_and_reboot`)
  は未実装** (menu 未対応と対)。
- **Deferred reboot の判断は `active_connection()` の有無で行う**。
  §4.2 の「直近 30 秒 send/recv 0 byte」相当の idle 判定は `IConnection`
  に API が無いので次のリファクタで足す (`RebootReadyFn` を差し替える口
  だけ用意した)。
- **menu 統合 (§3.2 の "Firmware" タブ) は未実装**。`main/menu.cpp` の
  タブは変更していない。手動 `kick(Manual)` / rollback / channel select
  等の UI は BLE 設定サービス側に寄せるか、menu の後続 PR で入れる。
- **CI (§3.3)**:
  - `release.yml` は `release/tab5_claude_client.bin` + `release/latest.json`
    をリリースアセットとして直接添付する形にした (ZIP は既存 Web Flasher
    互換のためそのまま残す)。
  - `pages.yml` は各 tag の raw アプリバイナリと `latest.json` を
    `_site/firmware/<tag>/` に staging し、非 prerelease で最新の tag の
    `latest.json` を `_site/latest.json` にコピーする。canary /
    promote-latest ワークフローは未実装 (§3.3 step 4)。
- **`sdkconfig.defaults`** に `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` /
  `CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD=y` を追加。
- **Kconfig** は `TAB5_OTA_ENABLED` / `TAB5_OTA_LATEST_URL` /
  `TAB5_OTA_POLL_INTERVAL_H` の 3 項目のみ (§3.2 の
  `TAB5_OTA_HEALTHY_DELAY_S` などは定数化した)。
