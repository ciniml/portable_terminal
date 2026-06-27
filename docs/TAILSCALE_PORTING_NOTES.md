# Tailscale 改良の serial_wifi_logger への back-port 用ノート

Tab5 (tab5_claude_client) で実装した Tailscale クライアント拡張を、上流の
`ciniml/serial_wifi_logger` に取り込む際に必要な知識をまとめたもの。
時系列に積み上げた 4 phase をひと続きの設計として整理し直してある。

対象コミット:
- `7cf3e71` Phase 7 step 8 — Tailscale DISCO Ping/Pong
- `11cd210` Phase 7 step 9 follow-ups — DERP-initial endpoint + multi-probe + initial-connect retry
- `b99b226` Phase 7 step 10 + 11 — HTTPS pubip + outbound CallMeMaybe

## 全体ゴール

「DERP relay は動く、direct UDP が動かない」状態の Tailscale クライアントに
**direct UDP path を成立させる** こと。具体的には:

- **DISCO** — peer の endpoint への到達性検証 (Ping/Pong)
- **DISCO CallMeMaybe** — peer に「ここに ping して来て」と促す DERP 経由のシグナル
- **STUN 代替の公開 IP 申告** (MapRequest.Endpoints) — peer が我々に CallMeMaybe / Ping を撃てるようにする
- **multi-probe + DERP-initial** — connectivity を切らさずに段階的に direct へ昇格

これにより、両方向で direct UDP が成立して WireGuard data plane が relay
経由 (32 KB/s) から direct UDP (200+ KB/s) に化ける。

---

## 1. DISCO Ping/Pong (step 8 相当)

`serial_wifi_logger` の `tailscale_disco.c` (265 行) には **4 つの致命的 bug**
がある。Tab5 では全部書き直した:

| # | 上流の現状 | 上流仕様 | 結果 |
|---|---|---|---|
| 1 | 暗号 = `wireguard_xaead_encrypt` (XChaCha20-Poly1305) | NaCl box (XSalsa20 + Poly1305) | peer 復号失敗 → 全 Ping silent drop |
| 2 | wire = `magic[6] + nonce[24] + ct` (sender disco pub 欠落) | `magic[6] + senderDiscoPub[32] + nonce[24] + ct` | 誰の Ping か特定不能 |
| 3 | 独立 UDP socket (ephemeral port) | WG と同じ UDP socket (41641 共有) | peer は ephemeral を学習 → asymmetric path → DERP 固定 |
| 4 | Pong のみ処理、Ping responder 無し | Ping → Pong (Src 欄に caller IP:port echo) | peer の bestAddr が direct に flip しない |

### 1.1 上流 wire format (verbatim)

```
[ magic[6]            "TS💬" = 54 53 F0 9F 92 AC ]
[ senderDiscoPub[32]  sender's Curve25519 disco public key ]
[ nonce[24]           random per packet ]
[ box ciphertext      NaCl box = X25519 → HSalsa20 → XSalsa20-Poly1305 ]
   └── plaintext: [type:1] [version=0:1] [body...]
```

Ping body: `TxID[12] || NodeKey[32]`
Pong body: `TxID[12] || SrcIP[16, IPv4-mapped v6] || SrcPort[2 BE]`

### 1.2 必要なモジュール

- **`tailscale_nacl.{h,c}`** (新規) — XSalsa20-Poly1305 NaCl box の seal/open。
  下回りの X25519 / HSalsa20 / Poly1305 は `components/wireguard/src/crypto/refc/`
  に既に揃っている (`wireguard_x25519`、`hsalsa20`、`poly1305_*`)。コピー不要、
  リンクするだけ。DERP の `ClientInfo` も同じ NaCl box なので `tailscale_derp.c`
  の inline `nacl_box_seal` (line ~182-255) も `ts_nacl_box_seal()` 呼び出しに置換。
- **`tailscale_disco.{h,c}`** (全面書き直し)

### 1.3 wireguardif との連携

`components/wireguard/src/wireguardif.c::wireguardif_network_rx` 冒頭に
**DISCO magic dispatch arm** を追加 (WG message type 判定の前):

```c
static const uint8_t kDiscoMagic[6] = { 0x54, 0x53, 0xF0, 0x9F, 0x92, 0xAC };
#define DISCO_MIN_LEN  (6 + 32 + 24 + 16 + 2)
if (len >= DISCO_MIN_LEN && memcmp(data, kDiscoMagic, 6) == 0) {
    if (s_disco_input_fn) s_disco_input_fn(data, len, addr, port);
    pbuf_free(p);
    return;
}
```

これにより **UDP 41641 で受信した DISCO frame と、DERP 経由で `inject_wg_packet`
から流れ込んできた DISCO frame が同じパスを通る**。DERP-relayed DISCO もタダで処理可能。

`wireguard_esp32.h` に typedef + setter + 共通 UDP TX 関数を追加:

```c
typedef void (*wireguard_disco_input_fn)(const uint8_t *data, size_t len,
                                          const ip_addr_t *src, uint16_t src_port);
void wireguard_esp32_set_disco_input(wireguard_disco_input_fn fn);

err_t wireguard_esp32_udp_send(const ip_addr_t *dst, uint16_t dst_port,
                               const uint8_t *data, size_t len);
```

`wireguard_esp32_udp_send` の実装は `wireguardif_peer_output` と同じ
`udp_sendto_if(device->udp_pcb, p, dst, dst_port, device->underlying_netif)`
パターン (DERP pseudo IP の分岐は通さない)。**WG udp_pcb を共有することで
NAT mapping を WG とまとめる** のが要点。これがないと NAT が UDP 送信元 port
毎に別 mapping を作って direct 化が壊れる。

### 1.4 ts_disco_send_ping の API 形

```c
esp_err_t ts_disco_send_ping(uint8_t wg_peer_index,
                             const uint8_t peer_node_pub[32],
                             const uint8_t peer_disco_pub[32],
                             const ip_addr_t *cand_ip, uint16_t cand_port);
```

- 内側 plaintext: `[0x01][0x00] [TxID:12] [NodeKey:32]` (NodeKey は我々の node_pub)
- box の宛先鍵 = `peer_disco_pub` (X25519 で shared 算出 → キャッシュ推奨、後述)
- 外側 frame = `magic[6] + s_disco_pub[32] + nonce[24] + ct`
- transport = `wireguard_esp32_udp_send(cand_ip, cand_port, frame, frame_len)`
- pending テーブル: TxID → (wg_peer_index) を保持。Pong 受信時に matching

### 1.5 Pong 受信 → wireguard_esp32_update_endpoint

Pong recv 時:
```c
case DISCO_MSG_PONG:
    // TxID lookup, peer_index 復元, ログ:
    //   ts_disco: Pong recv from <ip>:<port> → peer N direct
    wireguard_esp32_update_endpoint(wg_idx, src_ip, src_port);
    s_verified[wg_idx] = true;  // 後の CMM rate skip 用
    break;
```

`wireguard_esp32_update_endpoint` は `wireguardif` に追加する関数で、`device->peers[wg_idx].ip/port` を更新する。これで WG の TX が direct UDP に flip する。

### 1.6 Ping 受信 → Pong 即時返信

Ping recv は **「UDP の src IP:port」を Pong.Src 欄に echo** して同じ src に返信:

```c
case DISCO_MSG_PING:
    // build Pong: [0x02][0x00] [TxID:12] [SrcIP6:16, v4-mapped] [SrcPort:2 BE]
    // box to sender's disco_pub
    // wireguard_esp32_udp_send(src_ip, src_port, frame, frame_len);
```

### 1.7 per-peer shared key cache

X25519 は遅い (ESP32 で数 ms)。`(peer_disco_pub) → shared[32]` を 8 entry
程度のキャッシュに入れて使い回すと、Pong/Ping 連投時の CPU 負荷が大幅に下がる。

### 1.8 ハマったポイント

- **`update_peer_addr` を MESSAGE_TRANSPORT_DATA で呼ぶか問題**:
  WG の last-source roaming は magicsock 仕様だと `bestAddr` 機構で上書きされる。
  我々は **verified Pong 経由でしか endpoint を昇格しない** 方針で正解。
- **DERP pseudo address (127.3.3.0/24)**: WG TX 側で `peer->ip` が `127.3.3.N:region`
  のとき DERP output callback に流す分岐がある。direct 昇格時に `update_endpoint`
  が呼ばれてここを抜ければ direct UDP に flip する。

---

## 2. DERP-initial + multi-probe + initial-connect retry (step 9 follow-ups)

step 8 直後の実機テストで判明した「初手で direct 化に失敗するとセッション断、
fall-back 無し」問題への対処。`11cd210` の中身。

### 2.1 候補数上限を 4 → 8 に拡張

`tailscale_netmap.c::TS_NETMAP_MAX_CANDIDATES = 8`。多くの peer は LAN/Docker IP
を 5-7 個並べたあとに本命の公開 WAN を 1 個出す。4 だと WAN が落ちる。

### 2.2 公開 WAN endpoint のスコア底上げ

netmap parser の endpoint scorer (`tailscale_netmap.c::score_endpoint`、tab5 で
言うと line 409-475 付近) で **public WAN (≠ RFC1918, ≠ LAN-local) を `score=1` の
底上げ**。`s5 192.168.x:41641` ばかりが TOP に並ぶのを防ぐ。

### 2.3 WG endpoint は **DERP pseudo で初期化**

```c
/* 旧: best direct candidate (届かない可能性大) を初期 endpoint に */
/* 新: ALWAYS DERP pseudo (127.3.3.40:home_region) を初期に */
ip_addr_t derp_pseudo;
ip4_addr_set_u32(&v4, htonl(0x7F030328 | 0));   // 127.3.3.40
ip_addr_copy_from_ip4(derp_pseudo, v4);
wireguard_esp32_update_endpoint(wg_idx, &derp_pseudo, home_region);
```

これにより **WG はまず DERP 経由で必ず接続できる**。DISCO Pong 受信したら
direct に昇格、失敗してても DERP のまま動作継続。

### 2.4 multi-probe DISCO

旧コードは `picked` 1 候補だけに Ping を打っていた。`apply_one_peer` の中で
**全候補に Ping fan-out**:

```c
if (disco_set && p->cand_count > 0) {
    for (int i = 0; i < p->cand_count; i++) {
        ts_disco_send_ping(wg_idx, p->node_pub, p->disco_pub,
                           &p->cands[i].ip, p->cands[i].port);
    }
}
```

### 2.5 first-Pong-wins idempotency

multi-probe で複数 Pong が返って来る可能性がある。**最初の Pong だけ採用**:

```c
#define DISCO_VERIFIED_MAX 16
static bool s_verified[DISCO_VERIFIED_MAX];
// Pong handler:
if (wg_idx < DISCO_VERIFIED_MAX && s_verified[wg_idx]) {
    ESP_LOGD(TAG, "Pong from %s for peer %u — already direct", ip, wg_idx);
    return;
}
if (wg_idx < DISCO_VERIFIED_MAX) s_verified[wg_idx] = true;
```

`DISCO_PENDING_MAX` (TxID → pending entry) は **16 → 64 に拡張**。multi-probe を
同時 in-flight で持てるように。

### 2.6 initial-connect retry

SSH のような上位アプリは「Wi-Fi 立ち上がる前」「Tailscale up 前」に start() しても
失敗する。`main/reconnect.{hpp,cpp}` の supervisor を **start() が初回失敗しても
動かす** よう変更。Tab5 では `app_main` の boot タスクで:

```c
bool up = c->start(d.remote_rx);
// up==false でも supervisor を起動して再試行に任せる
tab5::set_active_connection(c.get());
tab5::start_reconnect_supervisor(c.get(), d.remote_rx, d.get_pty_size);
```

これは Tailscale そのものではないが、Tailscale up タイミングが遅い問題に対応するため。

---

## 3. HTTPS pubip (step 10) — STUN の代わり

step 9 では STUN over UDP を実装したが、**MAP-E / IPv4-over-IPv6 (v6プラス, BBIX
フルマップ, transix, OCN バーチャルコネクト等) の家庭環境で STUN BindingResponse
が返ってこない** 問題が判明。

### 3.1 観測された症状

- BindingRequest は出ている (`make monitor` で確認)
- 30 秒おきに 5 回送って 5 回とも応答ゼロ
- `wireguardif_network_rx` 冒頭にデバッグログを置いて確認 → **STUN magic を持つ
  packet が一切 lwIP に届いていない**
- 別経路 (HTTPS) は問題なく通る

### 3.2 仮説

MAP-E 環境特有の挙動:
- 公開 IPv4 を 16 世帯で共有、各世帯は限られた port 範囲のみ使える
- WG の UDP keepalive が常時 port を消費していて、新規 outbound (STUN) の
  conntrack が安定して張れない
- もしくは router の独自フィルタが port 3478 inbound を block
- いずれにせよ **TCP 443 (HTTPS) はこれらの制約から外れて動く**

### 3.3 解決 — HTTPS で公開 IP を取る

`tailscale_pubip.{h,c}` を新規追加。STUN モジュールは丸ごと削除して置き換え:

```c
typedef void (*ts_pubip_result_fn)(uint32_t public_ip_be, uint16_t public_port);

esp_err_t ts_pubip_init(ts_pubip_result_fn cb);
void      ts_pubip_deinit(void);
void      ts_pubip_kick(void);
```

実装:
- **`esp_http_client` + `esp_crt_bundle_attach`** で TLS GET
- primary: `https://api.ipify.org/` (plain text body, 7-15 byte IPv4)
- fallback: `https://checkip.amazonaws.com/`
- 自前 FreeRTOS task (stack 6KB, prio 4) で fetch。tcpip thread を絡めない
- 5 分周期の `xTimer` (auto-reload) + bin semaphore 起動
- port は `CONFIG_TAILSCALE_LISTEN_PORT` を仮定 (port preservation 前提)
- `(ip, port)` で dedup、変化時のみ cb 発火

### 3.4 MapRequest.Endpoints の publish

`tailscale_control.c` に endpoint 管理を足す:

```c
#define CTRL_MAX_ENDPOINTS 4
#define CTRL_MAX_EP_LEN    48
static char   s_endpoints[CTRL_MAX_ENDPOINTS][CTRL_MAX_EP_LEN];
static int    s_endpoint_count = 0;
static volatile bool s_endpoints_dirty = false;

void ts_ctrl_set_endpoints(const char *const *eps, int n);
void ts_ctrl_signal_endpoints_dirty(void);
int  ts_ctrl_get_endpoints(char out[][CTRL_MAX_EP_LEN], int max);
```

- `ts_ctrl_map_request()` の cJSON 構築時に `"Endpoints":["a.b.c.d:port"]` を追加
- `ts_ctrl_poll_loop()` の `ts_netmap_apply` 直後で `s_endpoints_dirty` チェック、
  立ってたら `ts_ctrl_map_request(ctx)` を再発行
- pubip 結果コールバックから `set_endpoints` + `signal_endpoints_dirty` を呼ぶ

### 3.5 期待されるログシーケンス

```
ts_pubip: discovered 49.239.69.1:41641
tailscale: Public endpoint: 49.239.69.1:41641
ts_ctrl: H2: MapRequest sent on sid=... (Endpoints field 込み)
```

### 3.6 注意 — 公開 IP が PPPoE 側か MAP-E 側かは契約による

実機テストで判明:
- フラッシュ 1 回目: `218.183.105.162` (PPPoE 側) を取得
- フラッシュ 2 回目: `49.239.69.1` (MAP-E 側) を取得

これは **api.ipify.org への TLS connect が IPv4 PPPoE か IPv6 IPoE+MAP-E
どちらを通るか**でブレる。**実際の WG UDP は概ね MAP-E 側を通る**ので、
本当は `49.x` を申告したい。安定化には:
- DNS 解決を IPv4 強制にする
- 複数 service で投げて多数決
- もしくは netmap の peer 側 candidate との交差検証

ただし不安定でも **peer 側の WG roaming が勝手に正しい IP を学習する** ことが
多いので、初期実装では best-effort で OK。

---

## 4. 外向き CallMeMaybe (step 11)

step 10 で `MapRequest.Endpoints` 申告は動くようになったが、**peer 側が我々を
direct と認識するタイミングが遅い** (peer の netmap re-fetch 待ち = 分単位)。
それを能動的に短縮するのが CallMeMaybe sender。

### 4.1 CallMeMaybe wire format

DISCO type `0x03`。外側 frame は Ping/Pong と同じ:
```
[magic[6]][senderDiscoPub[32]][nonce[24]][NaCl box ciphertext]
```

内側 plaintext:
```
[ type=0x03 : 1 ]
[ ver=0    : 1 ]
[ AddrPort records, 18 bytes each:
    IPv6[16]   ← v4 は v4-mapped ::ffff:a.b.c.d
    Port[2 BE]
]
```

セマンティクス: **「sender (= 我々) はこれらの addr に居る。あなた (= recipient
peer) は今すぐ DISCO Ping を撃って」**。

### 4.2 送り方

**DERP 経由** で送る (Ping/Pong の UDP path とは違う)。理由: CallMeMaybe を撃つ
ということは **まだ direct UDP が無い peer に対して**だから。

```c
esp_err_t ts_disco_send_call_me_maybe(uint8_t wg_peer_index,
                                       const uint8_t peer_node_pub[32],
                                       const uint8_t peer_disco_pub[32],
                                       uint8_t peer_derp_region);
```

実装:
1. `ts_ctrl_get_endpoints()` で自分の published endpoint をスナップショット
2. 18-byte AddrPort records に pack (v4-mapped v6)
3. 内側 plaintext `[0x03][0x00][AddrPort×N]` を組み立てる
4. NaCl box で encrypt (宛先鍵 = `peer_disco_pub`)
5. 外側 frame を組む
6. `ts_derp_send(peer_derp_region, peer_node_pub, frame, frame_len)`

### 4.3 抑止フロー

3 段の skip:
1. **verified-skip**: `s_verified[wg_idx]` が true (= 既に direct 化済み) なら送らない
2. **rate-limit**: peer ごと 30 秒に 1 回まで (`s_last_call_me_maybe_ts[16]`、
   `esp_timer_get_time()` で測る)
3. **no-endpoints**: published endpoints が空なら諦める

### 4.4 トリガ場所

`tailscale_netmap.c::apply_one_peer` の **既存 DISCO Ping fan-out の直後**:

```c
if (disco_set && p->derp_region > 0) {
    ts_disco_send_call_me_maybe(wg_idx, p->node_pub, p->disco_pub,
                                 (uint8_t)p->derp_region);
}
```

これを **新規 peer 追加パス** と **既存 peer 更新パス** の両方に入れる。
理由: 起動時の最初の netmap 適用時点ではまだ pubip が完了しておらず
endpoint がゼロなので、後で endpoint が確定したら **次の netmap (= delta) 適用時に
ようやく送れる**。新規 peer 出現と endpoint 確定のタイミングが必ずしも一致しないため。

### 4.5 受信側 (= 我々が peer から CallMeMaybe を受け取ったとき)

step 11 の前にも入れておくべき。step 9 の DISCO 書き直し時に
**type=0x03 の case** を `ts_disco_rx` の switch に追加:

```c
case DISCO_MSG_CALL_ME_MAYBE: {
    const uint8_t *body = pt + 2;
    size_t body_len = pt_len - 2;
    if (body_len % 18 != 0) { /* drop */ return; }

    uint8_t wg_idx;
    uint8_t peer_node_pub[32];
    if (!ts_netmap_lookup_by_disco(sender_pub, &wg_idx, peer_node_pub)) return;

    for (int i = 0; i < body_len/18; i++) {
        const uint8_t *rec = body + i*18;
        // v4-mapped 判定
        bool v4mapped = (memchr(rec, 0, 10) - rec == 0)  /* 10 zero bytes */
                       && rec[10]==0xFF && rec[11]==0xFF;
        if (!v4mapped) continue;
        uint32_t addr_be; memcpy(&addr_be, rec+12, 4);
        uint16_t port = (rec[16]<<8) | rec[17];
        // skip 0.0.0.0, 127/8, 224/4, port=0
        ip_addr_t ip; ip4_addr_set_u32_via_helper(&ip, addr_be);
        // 即時 DISCO Ping → 両側の NAT pinhole 同期穴あけ
        ts_disco_send_ping(wg_idx, peer_node_pub, sender_pub, &ip, port);
    }
    break;
}
```

`ts_netmap_lookup_by_disco(disco_pub, ...)` は `s_peers[]` を走査して
`memcmp(disco_pub, ...) == 0` の slot を返す helper。`tailscale_netmap.{h,c}` に追加。

---

## 5. 環境依存の知見 (= 想定読者の家のネットワーク特性)

実機テストで明らかになった事項。CONFIG とアルゴリズム選択に直結する。

### 5.1 BBIX フルマップ MAP-E / v6プラス系

- **UDP STUN は応答が返ってこない** (理由は仮説段階だが事実として観測済み)
- **TCP HTTPS は普通に通る**
- → **STUN モジュールを置く意味はゼロ**。pubip 一択
- 上流 serial_wifi_logger に新たに STUN を実装するのは非推奨。直接 pubip でいい

### 5.2 2 段 NAT (XG-100NE + IX2215 のような)

- IX2215 など下流ルータの port forward 設定なしでも:
  - 上流 NAT の **port preservation** が効いていれば peer の roaming が勝手に
    direct を学習する
  - **下流 NAT に port forward 入れる必要は実は無かった** (peer ↔ Tab5 で direct 確認済み)
- 重要なのは「published endpoint と outbound 実体の IP が **概ね一致** すること」

### 5.3 hairpin

- 同一 NAT 配下の peer (例: 同じ家のホスト) から自分の WAN endpoint に Ping すると、
  ルータが hairpin NAT で内側に戻してくれる場合がある
- これが効くと **LAN-local candidate に DISCO Ping が通らなくても WAN endpoint
  経由で direct が成立** することがある
- 期待しすぎは禁物 (router 依存) だが、effective fallback の 1 つ

### 5.4 dual-WAN

- `218.183.105.162` (PPPoE) と `49.239.69.1` (MAP-E) のような **同一家ネットワーク内で
  経路依存に公開 IP が変わる** ケースが日常的に存在する
- pubip 結果はそのとき HTTPS が通った経路の IP しか分からない
- WG UDP の実 outbound IP と申告 IP がズレることを想定して、**peer 側 WG roaming
  に頼る部分も残しておく** べき

---

## 6. ファイル別差分サマリ

`serial_wifi_logger` に対する変更点を、ファイル別に。

### components/tailscale/CMakeLists.txt

- `SRCS` に `tailscale_pubip.c`、`tailscale_nacl.c` を追加 (`tailscale_stun.c` は **作らない**)
- `PRIV_REQUIRES` に `esp_http_client`、`esp_timer`、`esp_event` を追加
- `target_include_directories` に上記コンポーネントの include path を explicit に追加
  (MINIMAL_BUILD で transitive include が伝わらないケース対策)

### components/tailscale/src/tailscale_nacl.{h,c} (新規)

- `ts_nacl_box_seal(shared[32], nonce[24], pt, ptlen, out_ct)` (= XSalsa20-Poly1305)
- `ts_nacl_box_open(shared[32], nonce[24], ct, ctlen, out_pt)` (検証 + 復号)
- 下回り (X25519 / HSalsa20 / Salsa20 / Poly1305) は `components/wireguard/src/crypto/refc/` を流用

### components/tailscale/src/tailscale_disco.{h,c} (全面書き直し)

- 4 bug 修正 (上 1.1)
- 共有鍵キャッシュ (per-peer 8 entry)
- `ts_disco_send_ping()` + Ping/Pong recv (with first-Pong-wins idempotency)
- `ts_disco_send_call_me_maybe()` (rate-limit + verified-skip + no-endpoints skip)
- DISCO type 0x03 (CallMeMaybe) **receiver** = 上流 endpoint へ即時 Ping fan-out
- `DISCO_PENDING_MAX = 64`, `DISCO_VERIFIED_MAX = 16`, `DISCO_PEERS_MAX = 16`

### components/tailscale/src/tailscale_netmap.{h,c}

- `TS_NETMAP_MAX_CANDIDATES = 8`
- `score_endpoint` で public WAN を底上げ (+1)
- `apply_one_peer` で:
  - WG endpoint は DERP pseudo で初期化
  - 全候補に DISCO Ping fan-out
  - CallMeMaybe fan-out (新規 + 更新両方の path)
- `ts_netmap_lookup_by_disco(disco_pub, &wg_idx, node_pub_out)` helper 追加
- `netmap_peer_t` に `derp_region` フィールド追加

### components/tailscale/src/tailscale_control.{h,c}

- `CTRL_MAX_ENDPOINTS = 4`, `CTRL_MAX_EP_LEN = 48`
- `ts_ctrl_set_endpoints()` / `ts_ctrl_signal_endpoints_dirty()` / `ts_ctrl_get_endpoints()`
- `ts_ctrl_map_request()` の cJSON 構築で `"Endpoints":[...]` を追加
- `ts_ctrl_poll_loop()` で `s_endpoints_dirty` の check + 再 MapRequest

### components/tailscale/src/tailscale_pubip.{h,c} (新規)

- HTTPS GET to api.ipify.org / checkip.amazonaws.com
- 自前 task + 5min auto-reload timer + bin semaphore
- IPv4 dotted-quad パース (whitespace 寛容)
- dedup-by-endpoint
- `ts_pubip_init/_deinit/_kick` API

### components/tailscale/src/tailscale_esp32.c

- `pubip_result_cb` (uint32_t ip_be, uint16_t port) → `snprintf("a.b.c.d:port")` →
  `ts_ctrl_set_endpoints` + `signal_endpoints_dirty`
- `ctrl_task` の 2 回目 MapRequest 後 で `ts_pubip_init(pubip_result_cb)` (idempotent guarded)
- 後続 ctrl_task ループでは `ts_pubip_kick()`

### components/tailscale/src/tailscale_derp.h

- `lwip/ip_addr.h` 追加 include (一部 helper signature 用)

### components/wireguard/include/wireguard_esp32.h

- `wireguard_disco_input_fn` typedef + `wireguard_esp32_set_disco_input()` 追加
- `wireguard_esp32_udp_send(dst, port, data, len)` 追加
- `wireguard_esp32_update_endpoint(wg_idx, addr, port)` 追加

### components/wireguard/src/wireguardif.c

- `wireguardif_network_rx` 冒頭に DISCO magic dispatch arm 追加
- DISCO callback static + setter 追加
- `wireguard_esp32_udp_send()` 実装 (udp_sendto_if pattern)
- `wireguard_esp32_update_endpoint()` 実装 (`device->peers[i].ip/port` 更新)
- DERP pseudo IP (127.3.3.0/24) の output 分岐は既存

---

## 7. 取り込み順 (推奨)

1 commit ずつ実機検証しながら積む順序。各段階で動くものができる。

### Stage A — DISCO 基盤
- `tailscale_nacl.{h,c}` 追加 + `tailscale_derp.c` の inline nacl_box_seal 置換
- `tailscale_disco.{h,c}` 全書き直し (Ping/Pong のみ、CallMeMaybe はまだ)
- wireguardif の DISCO dispatch arm + `wireguard_esp32_udp_send` + `wireguard_esp32_update_endpoint`
- 検証: LAN 内 peer に direct で繋がる (DISCO Pong recv → "peer N direct" ログ)

### Stage B — multi-probe + DERP-initial
- `TS_NETMAP_MAX_CANDIDATES = 8`、scorer 修正
- WG endpoint を DERP pseudo で初期化
- 全候補 Ping fan-out + `s_verified[]` first-Pong-wins
- `DISCO_PENDING_MAX = 64`
- 検証: 候補が複数ある peer で direct 化が早くなる、失敗しても DERP fall-back で生存

### Stage C — MapRequest.Endpoints
- `tailscale_control.{h,c}` の endpoint API + cJSON 追加 + poll_loop の dirty 再発行
- まだ自動 source なしで、テストは手動 `ts_ctrl_set_endpoints({"a.b.c.d:port"}, 1)` の追加で
- 検証: `tailscale status` (他 peer 側) で Tab5 endpoint が見える

### Stage D — pubip
- `tailscale_pubip.{h,c}` 追加
- `tailscale_esp32.c` の wiring (pubip_result_cb → set_endpoints + signal_dirty)
- CMakeLists の include / requires 追加
- 検証: `ts_pubip: discovered ...:port` ログ、MapRequest body に Endpoints が乗る

### Stage E — CallMeMaybe (受信)
- `tailscale_netmap.{h,c}` に `ts_netmap_lookup_by_disco` 追加
- `tailscale_disco.c::ts_disco_rx` の switch に case `0x03` 追加
- 検証: 他 peer (公式 client) が CallMeMaybe を撃ってきたとき、Tab5 がそれに応じて
  DISCO Ping fan-out するログが出る

### Stage F — CallMeMaybe (送信)
- `ts_disco_send_call_me_maybe()` 実装 (rate-limit + verified-skip + no-eps skip)
- `tailscale_netmap.c::apply_one_peer` の 2 箇所 (新規/更新 path) で CMM 発射
- `s_last_call_me_maybe_ts[16]` state
- `derp_region` フィールド `netmap_peer_t` に追加
- 検証: 起動後 ~2 分以内に peer 側で `direct` になる

---

## 8. 既知の制限 (back-port 後も残る)

- **IPv6 ピア未対応**: CallMeMaybe receiver で v4-mapped でない address は捨てている。
  本来は IPv6 ネイティブで peer に届けば NAT 越え一切不要だが、本実装は IPv4 only
- **NAT 種別自動判定なし**: 公式 client は STUN を複数回打って Symmetric NAT 検出と
  DERP fallback の自動判定をしている。本実装はそこまで踏み込まない
- **MapRequest 再発行の latency**: dirty flag を `ts_netmap_apply` 直後でしか見ないので、
  最悪 KeepAlive (~1 分) 待ち。リアルタイム化は task notify 化が必要
- **pubip の経路安定性**: dual-WAN 環境で取得 IP が起動毎に変わりうる (上 5.4)

---

## 9. 動作確認用のログ patterns

実機検証時に grep するパターン:

```
ts_pubip: discovered <IPv4>:<port>          # pubip 成功
tailscale: Public endpoint: ...              # ctrl 側に登録
ts_disco: Ping sent peer=N to ...            # outbound DISCO Ping
ts_disco: Pong recv from ... → peer N direct # direct 確立
ts_disco: Ping recv from ...                 # peer からの DISCO Ping 受信
ts_disco: Pong sent to ...                   # 我々が Pong を返した
ts_disco: CallMeMaybe from peer N with M addrs   # CMM 受信
ts_disco: CallMeMaybe sent peer=N to derp=R with M eps  # CMM 送信
wireguard: HANDSHAKE_RESPONSE: <hex>:<port>  # WG handshake (hex は little-endian IPv4)
```

`HANDSHAKE_RESPONSE: 2803037f:N` (= `127.3.3.40:region`) は DERP 経由。
非 `127.3.3.x` の IP が出てくれば direct 確立成功。

bench 用に `ssh: perf rx=... B/s` も合わせて見ると、direct 昇格前後の throughput
変化が定量化できる。LAN baseline ≒ 395 KB/s, DERP ≒ 32 KB/s, WG direct ≒ 200+ KB/s。

---

## 10. ファイル間相互依存マップ

```
                tailscale_pubip
                      ↓ pubip_result_cb
                tailscale_esp32 (stun_result_cb → set_endpoints + signal_dirty)
                      ↓
                tailscale_control (Endpoints, MapRequest 再発行)
                      ↓ get_endpoints (read access)
                tailscale_disco (send_call_me_maybe pack)
                      ↓ ts_derp_send
                tailscale_derp (frame transport)

  netmap apply ──→ disco.send_ping (UDP via WG socket)
              ──→ disco.send_call_me_maybe (DERP)

  WG udp_pcb RX ──→ wireguardif_network_rx
                    ├── magic == "TS💬" → s_disco_input_fn = disco.ts_disco_rx
                    │                       ├── PING  → Pong 即時返信
                    │                       ├── PONG  → update_endpoint + s_verified
                    │                       └── CMM   → 全 addr に send_ping fan-out
                    └── default → WG handshake / transport data

  WG TX ──→ wireguardif_peer_output
            ├── peer->ip が 127.3.3.x → DERP output callback
            └── else → udp_sendto_if (direct UDP)
```

---

## まとめ

- **STUN を作らずに pubip (HTTPS) で行く** — MAP-E 環境で確実
- **DISCO は wire format 厳守** — NaCl box + sender disco pub embed + WG socket 共有
- **CallMeMaybe receiver と sender はセット** — receiver だけでも片側 direct には効く
- **DERP-initial endpoint は必須** — connectivity を切らさない保険
- **multi-probe + first-Pong-wins** — 候補が多い peer に強い
- **MapRequest.Endpoints の dirty 再発行** — pubip 確定後の peer 側 netmap 更新を促す

実機テストで段階的に上げていけば、最終的に外部ネットワーク (LTE テザリング等)
からの SSH bench が DERP-only 32 KB/s から direct 200+ KB/s に化けるところまで
持っていける。
