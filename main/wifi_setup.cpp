#include "wifi_setup.hpp"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#include "M5Unified.h"

#include "c6_fw_update.hpp"

// esp_hosted_init runs in a constructor (port_esp_hosted_host_init.c); we
// only need esp_hosted_get_coprocessor_fwversion + a check on transport
// readiness from c6_fw_update.cpp, which pulls the header itself. No
// direct esp_hosted include needed here.

namespace tab5 {

namespace {

constexpr const char* kTag = "tab5_wifi";
constexpr int kBitConnected = BIT0;
constexpr int kBitFailed = BIT1;
constexpr int kMaxRetry = 5;

EventGroupHandle_t g_events = nullptr;
WifiStatus g_status{};
int g_retry = 0;

void on_wifi_event(void* /*arg*/, esp_event_base_t base, int32_t id,
                   void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_status.connected = false;
        if (g_retry < kMaxRetry) {
            esp_wifi_connect();
            ++g_retry;
            ESP_LOGW(kTag, "retrying connect (%d/%d)", g_retry, kMaxRetry);
        } else {
            xEventGroupSetBits(g_events, kBitFailed);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        g_retry = 0;
        g_status.connected = true;
        g_status.ip4 = event->ip_info.ip.addr;
        ESP_LOGI(kTag, "got IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(g_events, kBitConnected);
    }
}

}  // namespace

// Tab5 routes the C6 Wi-Fi co-processor's power-enable line through bit 0
// of PI4IOE2 (I2C addr 0x44). Replicate the comprehensive BSP init from
// M5Tab5-UserDemo so the chip is in a known state before driving the
// WLAN_PWR_EN pin — M5Unified's per-bit setters don't touch chip-reset,
// pull configuration, or the IN_DEF_STA register.
namespace {
constexpr uint8_t kPi4ioe2Addr = 0x44;
constexpr uint8_t kRegChipReset = 0x01;
constexpr uint8_t kRegIoDir     = 0x03;
constexpr uint8_t kRegOutSet    = 0x05;
constexpr uint8_t kRegOutHighZ  = 0x07;
constexpr uint8_t kRegPullSel   = 0x09;
constexpr uint8_t kRegPullEn    = 0x0B;
constexpr uint8_t kRegInDefSta  = 0x0F;
constexpr uint8_t kRegIntMask   = 0x11;

void pi4ioe2_init_for_wifi() {
    auto& bus = M5.In_I2C;
    // Chip reset (write any non-zero, then read to clear).
    bus.writeRegister8(kPi4ioe2Addr, kRegChipReset, 0xFF, 400000);
    uint8_t scratch = 0;
    bus.readRegister(kPi4ioe2Addr, kRegChipReset, &scratch, 1, 400000);

    // Mirror BSP: P0(WLAN_EN), P3(USB5V_EN), P4, P5, P7 = output; P1,P2,P6 = input.
    bus.writeRegister8(kPi4ioe2Addr, kRegIoDir,    0b10111001, 400000);
    bus.writeRegister8(kPi4ioe2Addr, kRegOutHighZ, 0b00000110, 400000);
    bus.writeRegister8(kPi4ioe2Addr, kRegPullSel,  0b10111001, 400000);
    bus.writeRegister8(kPi4ioe2Addr, kRegPullEn,   0b11111001, 400000);
    bus.writeRegister8(kPi4ioe2Addr, kRegInDefSta, 0b01000000, 400000);
    bus.writeRegister8(kPi4ioe2Addr, kRegIntMask,  0b10111111, 400000);
}

void tab5_c6_power_enable(bool en) {
    auto& bus = M5.In_I2C;
    // Read-modify-write OUT_SET (reg 0x05). Bit 0 = WLAN_PWR_EN.
    uint8_t cur = 0;
    bus.readRegister(kPi4ioe2Addr, kRegOutSet, &cur, 1, 400000);
    uint8_t next = en ? (cur | 0x01) : (cur & ~0x01);
    bus.writeRegister8(kPi4ioe2Addr, kRegOutSet, next, 400000);

    uint8_t readback = 0;
    bus.readRegister(kPi4ioe2Addr, kRegOutSet, &readback, 1, 400000);
    ESP_LOGI(kTag, "C6 power-enable: requested=%d OUT_SET=0x%02x (bit0=%d)",
             static_cast<int>(en), readback,
             static_cast<int>((readback & 0x01) != 0));
    if (((readback & 0x01) != 0) != en) {
        ESP_LOGE(kTag, "PI4IOE2 bit 0 readback mismatch — I2C did not "
                 "reach 0x44 (check M5.In_I2C bus state)");
    }
}
}  // namespace

term::Result<void> wifi_sta_connect(std::string_view ssid,
                                    std::string_view psk,
                                    int timeout_s) {
    if (ssid.empty()) {
        ESP_LOGE(kTag, "SSID is empty");
        return std::unexpected(term::Error::NotInitialized);
    }

    // Bring PI4IOE2 (0x44) into a known state — M5Unified only constructs
    // the IO expander; comprehensive register init (chip reset, dirs, pulls,
    // high-Z) is done here, matching M5Tab5-UserDemo's BSP. Then force a
    // power-cycle of the C6 (off → wait → on) so it boots fresh into its
    // current slave firmware. Without the explicit off-step the C6 may
    // retain state from a previous boot (the WLAN_PWR_EN pin doesn't always
    // hard-reset the chip if it's already powered) and CMD5 / op_cond
    // times out at the SDIO layer.
    pi4ioe2_init_for_wifi();
    tab5_c6_power_enable(false);
    vTaskDelay(pdMS_TO_TICKS(300));
    tab5_c6_power_enable(true);
    vTaskDelay(pdMS_TO_TICKS(1500));

    // Boot-time C6 slave-firmware auto-update. esp_hosted_init has already
    // been called from its constructor; once the C6 finishes its post-
    // power-up handshake the transport is up and we can query the version.
    // c6_fw::update_if_needed() polls briefly for that, returns 0 if the
    // C6 is up to date or no embedded blob is present, 1 if it streamed
    // a newer image. After a successful update we restart so the C6
    // boots into its new image and the host transport handshakes fresh.
    int c6_upd = tab5::c6_fw::update_if_needed();
    if (c6_upd == 1) {
        ESP_LOGW(kTag,
                 "C6 firmware updated — restarting P4 to renegotiate link.");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else if (c6_upd == -1) {
        ESP_LOGE(kTag, "C6 firmware auto-update failed; continuing "
                 "with the existing slave image.");
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    g_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, nullptr));

    wifi_config_t wcfg{};
    size_t n = std::min<size_t>(ssid.size(), sizeof(wcfg.sta.ssid) - 1);
    std::memcpy(wcfg.sta.ssid, ssid.data(), n);
    n = std::min<size_t>(psk.size(), sizeof(wcfg.sta.password) - 1);
    std::memcpy(wcfg.sta.password, psk.data(), n);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    std::strncpy(g_status.ssid, ssid.data(),
                 std::min(ssid.size(), sizeof(g_status.ssid) - 1));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        g_events, kBitConnected | kBitFailed, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_s * 1000));
    if (bits & kBitConnected) return {};
    if (bits & kBitFailed) {
        ESP_LOGE(kTag, "Wi-Fi connect failed after retries");
    } else {
        ESP_LOGE(kTag, "Wi-Fi connect timed out after %ds", timeout_s);
    }
    return std::unexpected(term::Error::BackendError);
}

WifiStatus wifi_status() {
    return g_status;
}

}  // namespace tab5
