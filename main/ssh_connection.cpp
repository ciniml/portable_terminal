#include "ssh_connection.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "mbedtls/sha256.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "libssh2.h"

#include "tofu.hpp"

#if CONFIG_TAILSCALE_ENABLE
#include "tailscale_esp32.h"
#endif

#if CONFIG_TAB5_SSH_PUBKEY_AUTH
// main/keys/id_rsa, baked in via EMBED_FILES in main/CMakeLists.txt.
extern "C" const uint8_t _binary_id_rsa_start[];
extern "C" const uint8_t _binary_id_rsa_end[];
#endif

namespace tab5 {

namespace {

constexpr const char* kTag       = "ssh";
constexpr size_t kTxStreamBytes  = 4096;
constexpr size_t kRxChunk        = 1024;

// libssh2 session state owned by ssh_task once start() succeeds.
struct Session {
    LIBSSH2_SESSION* sess = nullptr;
    LIBSSH2_CHANNEL* chan = nullptr;
    int sock = -1;
    StreamBufferHandle_t tx = nullptr;
    ByteSink rx_apply;
    std::atomic<bool> running{false};
    // True from ssh_task entry until ssh_task has fully torn down its
    // resources. start() refuses to re-enter while this is true so that
    // a reconnect attempt from the supervisor can't race with the old
    // task's shutdown_session().
    std::atomic<bool> task_alive{false};
    char banner[96] = {0};
    // Pending pty resize. Producer: any thread via SshConnection::resize.
    // Consumer: ssh_task on its next poll. cols==0 means "no pending".
    std::atomic<uint16_t> pending_cols{0};
    std::atomic<uint16_t> pending_rows{0};
};

Session g_sess;

void close_sock_locked() {
    if (g_sess.sock >= 0) {
        close(g_sess.sock);
        g_sess.sock = -1;
    }
}

void shutdown_session() {
    g_sess.running = false;
    if (g_sess.chan) {
        libssh2_channel_free(g_sess.chan);
        g_sess.chan = nullptr;
    }
    if (g_sess.sess) {
        libssh2_session_disconnect(g_sess.sess, "shutdown");
        libssh2_session_free(g_sess.sess);
        g_sess.sess = nullptr;
    }
    close_sock_locked();
    if (g_sess.tx) {
        vStreamBufferDelete(g_sess.tx);
        g_sess.tx = nullptr;
    }
}

bool dial(const char* host, uint16_t port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    // If the user typed a Tailscale hostname (full or short form) and
    // the Tailscale netmap has that peer, use the peer's 100.x.y.z so
    // we don't need a DNS resolver capable of MagicDNS.
    const char* dial_host = host;
    char ts_ip[20] = {0};
#if CONFIG_TAILSCALE_ENABLE
    if (tailscale_esp32_resolve(host, ts_ip, sizeof(ts_ip)) == ESP_OK) {
        ESP_LOGI(kTag, "tailscale: %s -> %s", host, ts_ip);
        dial_host = ts_ip;
    }
#endif

    struct addrinfo hints = {};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(dial_host, port_str, &hints, &res);
    if (rc != 0 || !res) {
        ESP_LOGE(kTag, "getaddrinfo(%s): %d", dial_host, rc);
        return false;
    }

    int s = -1;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) continue;
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(s);
        s = -1;
    }
    freeaddrinfo(res);

    if (s < 0) {
        ESP_LOGE(kTag, "connect(%s:%u) failed: errno=%d", host, port, errno);
        return false;
    }

    // TCP_NODELAY for low-latency keystrokes; SO_KEEPALIVE + tight
    // intervals so a dead remote / Wi-Fi outage is detected in ~60 s
    // (30 s idle + 3*10 s of unacked probes) and the supervisor can
    // reconnect.
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = 30;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE,  &idle, sizeof(idle));
    int intv = 10;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intv, sizeof(intv));
    int cnt = 3;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT,   &cnt, sizeof(cnt));

    g_sess.sock = s;
    return true;
}

// SHA-256 over the host key bytes. libssh2_session_hostkey() returns the
// raw key blob — what we hash for TOFU.
bool hostkey_fingerprint_sha256(LIBSSH2_SESSION* s, uint8_t out[32]) {
    size_t len = 0;
    int type = 0;
    const char* key = libssh2_session_hostkey(s, &len, &type);
    if (!key || !len) return false;
    int rc = mbedtls_sha256(reinterpret_cast<const uint8_t*>(key), len, out, 0);
    return rc == 0;
}

void hex32(const uint8_t in[32], char out[65]) {
    static const char* kHex = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2]     = kHex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[in[i] & 0xF];
    }
    out[64] = '\0';
}

// TOFU storage lives in main/tofu.{hpp,cpp}; this function is just a
// thin alias kept for diff readability against earlier history.
int tofu_check_or_record(const char* host, uint16_t port,
                         const uint8_t fp[32], bool* first_use_out) {
    return tab5::tofu::check_or_record(host, port, fp, first_use_out);
}

void ssh_task(void* /*arg*/) {
    g_sess.task_alive.store(true);
    uint8_t rxbuf[kRxChunk];
    uint8_t txbuf[kRxChunk];
    int64_t next_ka_us = esp_timer_get_time();

    while (g_sess.running) {
        // libssh2 doesn't fire keepalives on its own — poke it once a
        // second and let it decide when to actually send a packet.
        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_ka_us) {
            int seconds_to_next = 0;
            int kr = libssh2_keepalive_send(g_sess.sess, &seconds_to_next);
            if (kr != 0 && kr != LIBSSH2_ERROR_EAGAIN) {
                ESP_LOGW(kTag, "keepalive_send -> %d; treating as dropped", kr);
                g_sess.running = false;
                break;
            }
            next_ka_us = now_us +
                (seconds_to_next > 0 ? seconds_to_next : 1) * 1000000LL;
        }
        // Honour any pending pty resize before the read loop. exchange()
        // hands us a single resize request and clears the slot.
        uint16_t pc = g_sess.pending_cols.exchange(0);
        if (pc != 0) {
            uint16_t pr = g_sess.pending_rows.load();
            int rc = libssh2_channel_request_pty_size(g_sess.chan, pc, pr);
            if (rc != 0) {
                ESP_LOGW(kTag, "pty_size(%u,%u) -> %d", pc, pr, rc);
            }
        }
        // Drain RX until EAGAIN / EOF.
        for (;;) {
            ssize_t n = libssh2_channel_read(g_sess.chan,
                                             reinterpret_cast<char*>(rxbuf),
                                             sizeof(rxbuf));
            if (n > 0) {
                if (g_sess.rx_apply) {
                    g_sess.rx_apply(std::span<const uint8_t>(rxbuf,
                                                             static_cast<size_t>(n)));
                }
                continue;
            }
            if (n == LIBSSH2_ERROR_EAGAIN) break;
            if (n == 0 || libssh2_channel_eof(g_sess.chan)) {
                ESP_LOGW(kTag, "remote closed channel");
                g_sess.running = false;
                break;
            }
            ESP_LOGE(kTag, "channel_read -> %d", static_cast<int>(n));
            g_sess.running = false;
            break;
        }
        if (!g_sess.running) break;

        // Drain TX stream buffer.
        size_t got = xStreamBufferReceive(g_sess.tx, txbuf, sizeof(txbuf), 0);
        size_t off = 0;
        while (got > 0) {
            ssize_t w = libssh2_channel_write(g_sess.chan,
                                              reinterpret_cast<const char*>(txbuf + off),
                                              got);
            if (w == LIBSSH2_ERROR_EAGAIN) {
                // Push the unsent bytes back to the front for the next pass.
                // Simpler: just loop with a tiny yield.
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            if (w < 0) {
                ESP_LOGE(kTag, "channel_write -> %d", static_cast<int>(w));
                g_sess.running = false;
                break;
            }
            got -= static_cast<size_t>(w);
            off += static_cast<size_t>(w);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(kTag, "ssh_task exiting");
    shutdown_session();
    g_sess.task_alive.store(false);
    vTaskDelete(nullptr);
}

}  // namespace

SshConnection::SshConnection(const SshConfig& cfg) : cfg_(cfg) {
    snprintf(label_, sizeof(label_), "%s@%s",
             cfg.user ? cfg.user : "?",
             cfg.host ? cfg.host : "?");
}

SshConnection::~SshConnection() { stop(); }

bool SshConnection::start(ByteSink rx_sink) {
    const SshConfig& cfg = cfg_;
    if (g_sess.task_alive.load()) {
        ESP_LOGD(kTag, "previous task still tearing down — caller should retry");
        return false;
    }
    if (g_sess.running) {
        ESP_LOGW(kTag, "already running");
        return false;
    }

    if (libssh2_init(0) != 0) {
        ESP_LOGE(kTag, "libssh2_init failed");
        return false;
    }

    if (!dial(cfg.host, cfg.port)) return false;

    g_sess.sess = libssh2_session_init();
    if (!g_sess.sess) {
        ESP_LOGE(kTag, "session_init failed");
        close_sock_locked();
        return false;
    }

    // Blocking mode for handshake / auth — simpler and the user is already
    // waiting on this call.
    libssh2_session_set_blocking(g_sess.sess, 1);

    int rc = libssh2_session_handshake(g_sess.sess, g_sess.sock);
    if (rc != 0) {
        ESP_LOGE(kTag, "handshake -> %d", rc);
        shutdown_session();
        return false;
    }

    // SSH-level keepalive: ask the remote to reply, every 30 s.
    libssh2_keepalive_config(g_sess.sess, /*want_reply=*/1, 30);

    // TOFU host key check.
    uint8_t fp[32];
    if (!hostkey_fingerprint_sha256(g_sess.sess, fp)) {
        ESP_LOGE(kTag, "could not read host key");
        shutdown_session();
        return false;
    }
    char fp_hex[65];
    hex32(fp, fp_hex);
    bool first_use = false;
    int tofu = tofu_check_or_record(cfg.host, cfg.port, fp, &first_use);
    if (tofu == -1) {
        ESP_LOGE(kTag, "HOST KEY MISMATCH for %s:%u (got SHA256:%s)",
                 cfg.host, cfg.port, fp_hex);
        shutdown_session();
        return false;
    }
    if (tofu == -2) {
        ESP_LOGW(kTag, "TOFU NVS unavailable — proceeding without persistence");
    }
    ESP_LOGI(kTag, "host key SHA256:%s%s",
             fp_hex, first_use ? "  (trusted on first use)" : "");
    snprintf(g_sess.banner, sizeof(g_sess.banner),
             "host key SHA256:%.16s...%s", fp_hex,
             first_use ? "  (first use)" : "");

    // Ask the server which userauth methods it advertises — useful when
    // diagnosing why publickey or password is silently rejected.
    {
        size_t ulen = std::strlen(cfg.user);
        char* methods = libssh2_userauth_list(g_sess.sess, cfg.user, ulen);
        if (methods) {
            ESP_LOGI(kTag, "server userauth methods: %s", methods);
        } else {
            ESP_LOGW(kTag, "userauth_list returned null (server may have "
                           "already authenticated as 'none')");
        }
    }

    rc = -1;  // start at "no auth attempted yet"
#if CONFIG_TAB5_SSH_PUBKEY_AUTH
    {
        size_t privlen = static_cast<size_t>(
            _binary_id_rsa_end - _binary_id_rsa_start);
        ESP_LOGI(kTag,
                 "publickey auth: trying key from EMBED_FILES (%u bytes, "
                 "starts with: %.20s)",
                 static_cast<unsigned>(privlen),
                 reinterpret_cast<const char*>(_binary_id_rsa_start));
        // Pass nullptr/0 for the public-key buffer — libssh2 derives the
        // public half from the private key when the pubkey arg is empty.
        rc = libssh2_userauth_publickey_frommemory(
            g_sess.sess, cfg.user, std::strlen(cfg.user),
            nullptr, 0,
            reinterpret_cast<const char*>(_binary_id_rsa_start), privlen,
            CONFIG_TAB5_SSH_KEY_PASSPHRASE);
        if (rc == 0) {
            ESP_LOGI(kTag, "publickey auth OK");
        } else {
            char* err_msg = nullptr;
            int   err_len = 0;
            int err_code = libssh2_session_last_error(g_sess.sess,
                                                      &err_msg, &err_len, 0);
            ESP_LOGW(kTag,
                     "publickey auth failed: rc=%d session_last_error=%d "
                     "msg='%.*s' — falling back to password",
                     rc, err_code, err_len, err_msg ? err_msg : "");
            // Common rc decoding for quick reference:
            //   -16 LIBSSH2_ERROR_PUBLICKEY_UNRECOGNIZED — server rejected key
            //   -17 LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED  — signature mismatch
            //   -18 LIBSSH2_ERROR_AUTHENTICATION_FAILED — wrong user / "publickey"
            //                                            not in server methods
            //   -19 LIBSSH2_ERROR_FILE                  — key parse / decrypt failure
            //   -10 LIBSSH2_ERROR_ALLOC                 — OOM
        }
    }
#endif
    if (rc != 0) {
        rc = libssh2_userauth_password(g_sess.sess, cfg.user, cfg.password);
        if (rc != 0) {
            char* err_msg = nullptr;
            int   err_len = 0;
            int err_code = libssh2_session_last_error(g_sess.sess,
                                                      &err_msg, &err_len, 0);
            ESP_LOGE(kTag, "password auth failed: rc=%d session_last_error=%d "
                           "msg='%.*s'",
                     rc, err_code, err_len, err_msg ? err_msg : "");
        }
    }
    if (rc != 0) {
        ESP_LOGE(kTag, "auth failed: %d", rc);
        shutdown_session();
        return false;
    }

    g_sess.chan = libssh2_channel_open_session(g_sess.sess);
    if (!g_sess.chan) {
        ESP_LOGE(kTag, "open_session failed");
        shutdown_session();
        return false;
    }

    if (libssh2_channel_request_pty_ex(g_sess.chan, "xterm-256color", 14,
                                       nullptr, 0,
                                       cfg.cols ? cfg.cols : 80,
                                       cfg.rows ? cfg.rows : 24,
                                       LIBSSH2_TERM_WIDTH_PX,
                                       LIBSSH2_TERM_HEIGHT_PX) != 0) {
        ESP_LOGE(kTag, "request_pty failed");
        shutdown_session();
        return false;
    }

    if (libssh2_channel_shell(g_sess.chan) != 0) {
        ESP_LOGE(kTag, "channel_shell failed");
        shutdown_session();
        return false;
    }

    // Switch to non-blocking for the steady-state read/write loop.
    libssh2_session_set_blocking(g_sess.sess, 0);
    int flags = fcntl(g_sess.sock, F_GETFL, 0);
    fcntl(g_sess.sock, F_SETFL, flags | O_NONBLOCK);

    g_sess.tx = xStreamBufferCreate(kTxStreamBytes, 1);
    if (!g_sess.tx) {
        ESP_LOGE(kTag, "stream buffer alloc failed");
        shutdown_session();
        return false;
    }
    g_sess.rx_apply = std::move(rx_sink);
    g_sess.running = true;

    BaseType_t ok = xTaskCreate(&ssh_task, "ssh_task",
                                cfg.task_stack_bytes ? cfg.task_stack_bytes : 8192,
                                nullptr,
                                tskIDLE_PRIORITY + 5, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "task spawn failed");
        g_sess.running = false;
        shutdown_session();
        return false;
    }
    ESP_LOGI(kTag, "shell up");
    return true;
}

void SshConnection::send(std::span<const uint8_t> bytes) {
    if (!g_sess.running || !g_sess.tx) return;
    xStreamBufferSend(g_sess.tx, bytes.data(), bytes.size(), 0);
}

void SshConnection::resize(uint16_t cols, uint16_t rows) {
    if (!g_sess.running || cols == 0 || rows == 0) return;
    // Rows first so the consumer's exchange(cols=0) check never sees a
    // half-published pair.
    g_sess.pending_rows.store(rows);
    g_sess.pending_cols.store(cols);
}

bool SshConnection::is_connected() const {
    return g_sess.running;
}

void SshConnection::stop() {
    if (!g_sess.running) return;
    g_sess.running = false;
    // ssh_task observes !running on its next tick, exits its loop, and
    // calls shutdown_session() before vTaskDelete. We don't join here —
    // teardown is cooperative.
}

}  // namespace tab5
