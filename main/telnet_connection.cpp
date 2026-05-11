#include "telnet_connection.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

namespace tab5 {

namespace {

constexpr const char* kTag       = "telnet";
constexpr size_t kTxStreamBytes  = 4096;
constexpr size_t kIoChunk        = 1024;

// RFC 854 / 855
constexpr uint8_t IAC  = 0xFF;
constexpr uint8_t DONT = 0xFE;
constexpr uint8_t DO   = 0xFD;
constexpr uint8_t WONT = 0xFC;
constexpr uint8_t WILL = 0xFB;
constexpr uint8_t SB   = 0xFA;
constexpr uint8_t SE   = 0xF0;

// Common option codes
constexpr uint8_t OPT_BINARY = 0x00;
constexpr uint8_t OPT_ECHO   = 0x01;
constexpr uint8_t OPT_SGA    = 0x03;  // suppress-go-ahead
constexpr uint8_t OPT_NAWS   = 0x1F;  // window size

struct Session {
    int sock = -1;
    StreamBufferHandle_t tx = nullptr;
    ByteSink rx_apply;
    std::atomic<bool> running{false};
    // See SshConnection::start for rationale — gate restart on full
    // teardown of the previous task.
    std::atomic<bool> task_alive{false};

    // Reported pty size — sent via NAWS SB right after server says DO NAWS,
    // and again whenever resize() is called.
    std::atomic<uint16_t> cols{80};
    std::atomic<uint16_t> rows{24};
    std::atomic<bool>     naws_accepted{false};
    std::atomic<bool>     resize_pending{false};

    // IAC parser state
    enum class S : uint8_t {
        Data, Iac, Iac2, SbHead, SbData, SbIac,
    } pstate = S::Data;
    uint8_t cmd_byte = 0;  // last DO/WILL/etc seen
    uint8_t sb_option = 0; // current subnegotiation option
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
    close_sock_locked();
    if (g_sess.tx) {
        vStreamBufferDelete(g_sess.tx);
        g_sess.tx = nullptr;
    }
}

bool dial(const char* host, uint16_t port) {
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    struct addrinfo hints = {};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || !res) {
        ESP_LOGE(kTag, "getaddrinfo(%s) -> %d", host, rc);
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

// Send raw bytes directly to the socket (used for IAC replies). Bypasses
// the TX stream buffer and IAC escaping — caller has already crafted the
// final wire bytes.
void write_raw(const uint8_t* data, size_t n) {
    if (g_sess.sock < 0) return;
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::send(g_sess.sock, data + off, n - off, 0);
        if (w <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(2));
                continue;
            }
            ESP_LOGW(kTag, "raw write failed: errno=%d", errno);
            return;
        }
        off += static_cast<size_t>(w);
    }
}

void send_naws_sb() {
    uint16_t c = g_sess.cols.load();
    uint16_t r = g_sess.rows.load();
    uint8_t buf[9] = {
        IAC, SB, OPT_NAWS,
        static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c & 0xFF),
        static_cast<uint8_t>(r >> 8), static_cast<uint8_t>(r & 0xFF),
        IAC, SE,
    };
    write_raw(buf, sizeof(buf));
}

// Server sent IAC <cmd> <opt>. Reply per a small policy:
//   DO NAWS   -> WILL NAWS  + immediate SB NAWS <cols rows>
//   DO ECHO/SGA/BINARY -> WONT (we don't need those modes from client side)
//   DO <other>         -> WONT
//   WILL ECHO          -> DO (let server echo)
//   WILL SGA           -> DO
//   WILL <other>       -> DONT
//   DONT/WONT          -> mirror (DONT/WONT)
void handle_negotiation(uint8_t cmd, uint8_t opt) {
    uint8_t reply[3] = { IAC, 0, opt };
    switch (cmd) {
        case DO:
            if (opt == OPT_NAWS) {
                reply[1] = WILL;
                write_raw(reply, sizeof(reply));
                g_sess.naws_accepted = true;
                send_naws_sb();
                return;
            }
            reply[1] = WONT;
            break;
        case WILL:
            reply[1] = (opt == OPT_ECHO || opt == OPT_SGA) ? DO : DONT;
            break;
        case DONT:
            reply[1] = WONT;
            break;
        case WONT:
            reply[1] = DONT;
            break;
        default:
            return;
    }
    write_raw(reply, sizeof(reply));
}

// Drain one chunk of RX, parse out IAC sequences, push the rest to
// rx_apply. Returns false on connection error / EOF.
bool process_rx(const uint8_t* buf, size_t n) {
    std::vector<uint8_t> data;
    data.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = buf[i];
        using S = Session::S;
        switch (g_sess.pstate) {
            case S::Data:
                if (b == IAC) g_sess.pstate = S::Iac;
                else data.push_back(b);
                break;
            case S::Iac:
                if (b == IAC) {                 // literal 0xFF
                    data.push_back(IAC);
                    g_sess.pstate = S::Data;
                } else if (b == SB) {
                    g_sess.pstate = S::SbHead;
                } else if (b == DO || b == DONT || b == WILL || b == WONT) {
                    g_sess.cmd_byte = b;
                    g_sess.pstate = S::Iac2;
                } else {
                    // Single-byte commands (NOP, AYT, etc.) — just consume.
                    g_sess.pstate = S::Data;
                }
                break;
            case S::Iac2:
                handle_negotiation(g_sess.cmd_byte, b);
                g_sess.pstate = S::Data;
                break;
            case S::SbHead:
                g_sess.sb_option = b;
                g_sess.pstate = S::SbData;
                break;
            case S::SbData:
                if (b == IAC) g_sess.pstate = S::SbIac;
                // else: drop subnegotiation payload (we don't act on any)
                break;
            case S::SbIac:
                if (b == SE) g_sess.pstate = S::Data;
                else if (b == IAC) g_sess.pstate = S::SbData;  // escaped 0xFF
                else g_sess.pstate = S::Data;                  // resync
                break;
        }
    }
    if (!data.empty() && g_sess.rx_apply) {
        g_sess.rx_apply(std::span<const uint8_t>(data.data(), data.size()));
    }
    return true;
}

void telnet_task(void*) {
    g_sess.task_alive.store(true);
    uint8_t rxbuf[kIoChunk];
    uint8_t txbuf[kIoChunk];

    while (g_sess.running) {
        if (g_sess.resize_pending.exchange(false) &&
            g_sess.naws_accepted.load()) {
            send_naws_sb();
        }

        // RX
        ssize_t n = recv(g_sess.sock, rxbuf, sizeof(rxbuf), 0);
        if (n > 0) {
            if (!process_rx(rxbuf, static_cast<size_t>(n))) {
                ESP_LOGW(kTag, "rx parse error");
                g_sess.running = false;
                break;
            }
        } else if (n == 0) {
            ESP_LOGW(kTag, "remote closed connection");
            g_sess.running = false;
            break;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE(kTag, "recv errno=%d", errno);
            g_sess.running = false;
            break;
        }

        // TX: drain stream buffer, escape any literal 0xFF as 0xFF 0xFF.
        size_t got = xStreamBufferReceive(g_sess.tx, txbuf, sizeof(txbuf), 0);
        if (got > 0) {
            std::vector<uint8_t> esc;
            esc.reserve(got);
            for (size_t i = 0; i < got; ++i) {
                esc.push_back(txbuf[i]);
                if (txbuf[i] == IAC) esc.push_back(IAC);
            }
            write_raw(esc.data(), esc.size());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(kTag, "telnet_task exiting");
    shutdown_session();
    g_sess.task_alive.store(false);
    vTaskDelete(nullptr);
}

}  // namespace

TelnetConnection::TelnetConnection(const TelnetConfig& cfg) : cfg_(cfg) {
    snprintf(label_, sizeof(label_), "%s:%u",
             cfg.host ? cfg.host : "?",
             static_cast<unsigned>(cfg.port));
}

TelnetConnection::~TelnetConnection() { stop(); }

bool TelnetConnection::start(ByteSink rx_sink) {
    if (g_sess.task_alive.load()) {
        ESP_LOGD(kTag, "previous task still tearing down — caller should retry");
        return false;
    }
    if (g_sess.running) {
        ESP_LOGW(kTag, "already running");
        return false;
    }
    if (!dial(cfg_.host, cfg_.port)) return false;

    // Set non-blocking after the blocking connect succeeded.
    int flags = fcntl(g_sess.sock, F_GETFL, 0);
    fcntl(g_sess.sock, F_SETFL, flags | O_NONBLOCK);

    g_sess.tx = xStreamBufferCreate(kTxStreamBytes, 1);
    if (!g_sess.tx) {
        ESP_LOGE(kTag, "stream buffer alloc failed");
        close_sock_locked();
        return false;
    }
    g_sess.rx_apply = std::move(rx_sink);
    g_sess.cols.store(cfg_.cols ? cfg_.cols : 80);
    g_sess.rows.store(cfg_.rows ? cfg_.rows : 24);
    g_sess.naws_accepted.store(false);
    g_sess.resize_pending.store(false);
    g_sess.pstate = Session::S::Data;
    g_sess.running = true;

    // Proactively advertise NAWS so the server can ask for our size.
    uint8_t hello[] = { IAC, WILL, OPT_NAWS };
    write_raw(hello, sizeof(hello));

    BaseType_t ok = xTaskCreate(&telnet_task, "telnet_task",
                                cfg_.task_stack_bytes ? cfg_.task_stack_bytes : 6144,
                                nullptr,
                                tskIDLE_PRIORITY + 5, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "task spawn failed");
        g_sess.running = false;
        shutdown_session();
        return false;
    }
    ESP_LOGI(kTag, "telnet up to %s", label_);
    return true;
}

void TelnetConnection::stop() {
    if (!g_sess.running) return;
    g_sess.running = false;
}

void TelnetConnection::send(std::span<const uint8_t> bytes) {
    if (!g_sess.running || !g_sess.tx) return;
    xStreamBufferSend(g_sess.tx, bytes.data(), bytes.size(), 0);
}

void TelnetConnection::resize(uint16_t cols, uint16_t rows) {
    if (!g_sess.running || cols == 0 || rows == 0) return;
    g_sess.cols.store(cols);
    g_sess.rows.store(rows);
    g_sess.resize_pending.store(true);
}

bool TelnetConnection::is_connected() const {
    return g_sess.running;
}

}  // namespace tab5
