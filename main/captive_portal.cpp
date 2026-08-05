// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0

#include "captive_portal.hpp"

#include "sdkconfig.h"

#if CONFIG_TAB5_HTTP_CONFIG_ENABLED

#include <atomic>
#include <cstdint>
#include <cstring>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

namespace tab5::captive_portal {

namespace {

constexpr const char* kTag       = "captive";
constexpr std::uint16_t kDnsPort = 53;
// Hard-coded AP IP — esp_netif default for `default_wifi_ap` is 192.168.4.1
// and we don't reconfigure it. Encoded big-endian for the DNS A-record body.
constexpr std::uint8_t kApIpBytes[4] = {192, 168, 4, 1};

std::atomic<bool> g_dns_run{false};
TaskHandle_t g_dns_task = nullptr;

// Minimal DNS responder: copies the question section verbatim and appends a
// single A answer pointing to the AP IP. Ignores QTYPE/QCLASS (returns the
// same A record for everything — sufficient for captive-portal probing
// which is always A on common Apple/Microsoft/Google host names).
void dns_task(void*)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(kTag, "socket failed: %d", errno);
        g_dns_task = nullptr;
        g_dns_run.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(kDnsPort);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ESP_LOGE(kTag, "bind :53 failed: %d", errno);
        close(sock);
        g_dns_task = nullptr;
        g_dns_run.store(false, std::memory_order_release);
        vTaskDelete(nullptr);
        return;
    }
    // Short recv timeout so the task can notice g_dns_run going false and
    // exit cleanly within a few hundred ms.
    timeval tv{};
    tv.tv_sec  = 0;
    tv.tv_usec = 500 * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::uint8_t buf[512];
    while (g_dns_run.load(std::memory_order_acquire)) {
        sockaddr_in src{};
        socklen_t sl = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                        reinterpret_cast<sockaddr*>(&src), &sl);
        if (n <= 12) continue; // not enough for a DNS header
        // Header is 12 B: ID(2) FLAGS(2) QD(2) AN(2) NS(2) AR(2).
        // Walk the question section: each label is <len>...<bytes>, ends at
        // a 0 byte, followed by QTYPE(2) + QCLASS(2). We don't actually
        // parse the labels — we just find the end so we can size the echo.
        int q = 12;
        while (q < n && buf[q] != 0) {
            int label_len = buf[q];
            if (label_len & 0xC0) break; // compression pointer — give up gracefully
            q += label_len + 1;
        }
        if (q >= n) continue;
        q += 1 + 4; // null terminator + QTYPE + QCLASS
        if (q > n) continue;

        // Build response: copy request, flip header to response, set
        // ANCOUNT=1, append a single compressed-name A record pointing
        // at the AP IP with TTL 60s.
        std::uint8_t resp[512];
        std::memcpy(resp, buf, q);
        resp[2] = 0x81; resp[3] = 0x80; // QR=1, RD=1, RA=1, RCODE=0
        resp[6] = 0x00; resp[7] = 0x01; // ANCOUNT = 1
        resp[8] = 0x00; resp[9] = 0x00; // NSCOUNT = 0
        resp[10] = 0x00; resp[11] = 0x00; // ARCOUNT = 0

        int a = q;
        resp[a++] = 0xC0; resp[a++] = 0x0C;        // name pointer → offset 12
        resp[a++] = 0x00; resp[a++] = 0x01;        // TYPE A
        resp[a++] = 0x00; resp[a++] = 0x01;        // CLASS IN
        resp[a++] = 0x00; resp[a++] = 0x00;        // TTL hi
        resp[a++] = 0x00; resp[a++] = 0x3C;        // TTL lo = 60
        resp[a++] = 0x00; resp[a++] = 0x04;        // RDLENGTH = 4
        resp[a++] = kApIpBytes[0]; resp[a++] = kApIpBytes[1];
        resp[a++] = kApIpBytes[2]; resp[a++] = kApIpBytes[3];

        sendto(sock, resp, a, 0,
               reinterpret_cast<sockaddr*>(&src), sl);
    }
    close(sock);
    g_dns_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

void start()
{
    if (g_dns_run.exchange(true, std::memory_order_acq_rel)) return;
    // Internal-RAM stack (uses lwIP socket APIs which are flash-safe but
    // we keep the convention consistent with other tiny network tasks).
    xTaskCreate(&dns_task, "captive-dns", 3072, nullptr,
                tskIDLE_PRIORITY + 1, &g_dns_task);
}

void stop()
{
    g_dns_run.store(false, std::memory_order_release);
    // Task self-deletes on next loop iteration after the 500 ms recv
    // timeout fires. Don't vTaskDelete here — letting it exit cleanly
    // avoids leaking the bound socket.
}

}  // namespace tab5::captive_portal

#endif  // CONFIG_TAB5_HTTP_CONFIG_ENABLED
