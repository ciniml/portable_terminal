/*
 * Tab5 ESP32-C6 slave updater (one-shot).
 *
 *   IDF 5.4.x + esp_hosted ^1.4.x speaks the same protocol as the
 *   factory C6 slave (V1.4.1), so we can open the SDIO link without
 *   touching the C6 first. esp_hosted 1.4 doesn't expose chunked OTA
 *   APIs publicly — its `esp_hosted_slave_ota(url)` is HTTP-only — so
 *   we reach for the internal rpc_ota_begin/write/end symbols and feed
 *   them the embedded 2.x slave binary directly.
 *
 *   Flash this updater once with updater.sh, watch the monitor, then
 *   reflash the main IDF 6.0 firmware over it when the log reports
 *   "Slave update completed".
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_hosted.h"
#include "esp_hosted_api_types.h"

/* esp_hosted 1.4 keeps these inside its rpc_wrap.c but the symbols are
 * non-static, so we can call them directly from here. */
extern int rpc_ota_begin(void);
extern int rpc_ota_write(uint8_t* ota_data, uint32_t ota_data_len);
extern int rpc_ota_end(void);

/* From transport_drv.h — visible publicly inside the component but not
 * re-exported via esp_hosted.h. Used to wait for the slave handshake
 * before kicking off RPCs. */
extern uint8_t is_transport_tx_ready(void);

static const char* TAG = "c6_updater";

/* network_adapter.bin embedded by EMBED_FILES in main/CMakeLists.txt */
extern const uint8_t slave_bin_start[] asm("_binary_network_adapter_bin_start");
extern const uint8_t slave_bin_end[]   asm("_binary_network_adapter_bin_end");

/* ---- Tab5 C6 power: PI4IOE2 (0x44) bit 0 = WLAN_PWR_EN ---------- */

#define BSP_I2C_SCL   GPIO_NUM_32
#define BSP_I2C_SDA   GPIO_NUM_31
#define PI4IOE2_ADDR  0x44
#define REG_CHIP_RST  0x01
#define REG_IO_DIR    0x03
#define REG_OUT_SET   0x05
#define REG_OUT_HIZ   0x07
#define REG_PULL_SEL  0x09
#define REG_PULL_EN   0x0B
#define REG_IN_DEF    0x0F
#define REG_INT_MASK  0x11

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_pi4ioe2;

static esp_err_t i2c_setup(void) {
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = BSP_I2C_SCL,
        .sda_io_num = BSP_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PI4IOE2_ADDR,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_pi4ioe2);
}

static void pi4ioe2_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    ESP_ERROR_CHECK(i2c_master_transmit(s_pi4ioe2, buf, 2, 100));
}

static uint8_t pi4ioe2_read_reg(uint8_t reg) {
    uint8_t out = 0;
    ESP_ERROR_CHECK(i2c_master_transmit_receive(s_pi4ioe2, &reg, 1, &out, 1, 100));
    return out;
}

static void c6_power_up(void) {
    pi4ioe2_write_reg(REG_CHIP_RST, 0xFF);
    (void)pi4ioe2_read_reg(REG_CHIP_RST);
    pi4ioe2_write_reg(REG_IO_DIR,   0b10111001);
    pi4ioe2_write_reg(REG_OUT_HIZ,  0b00000110);
    pi4ioe2_write_reg(REG_PULL_SEL, 0b10111001);
    pi4ioe2_write_reg(REG_PULL_EN,  0b11111001);
    pi4ioe2_write_reg(REG_IN_DEF,   0b01000000);
    pi4ioe2_write_reg(REG_INT_MASK, 0b10111111);
    pi4ioe2_write_reg(REG_OUT_SET,  0b00001001);
    uint8_t r = pi4ioe2_read_reg(REG_OUT_SET);
    ESP_LOGI(TAG, "PI4IOE2 OUT_SET=0x%02x (bit0=WLAN_PWR_EN=%d)", r, !!(r & 1));
}

/* ----------------------------------------------------------------- */

/* esp_hosted 1.4's internal chunk size — keep at the same value the
 * library uses internally to avoid stressing the RPC layer. */
#define OTA_CHUNK_SIZE 1400

static void run_ota(void) {
    const uint8_t* p = slave_bin_start;
    const size_t total = (size_t)(slave_bin_end - slave_bin_start);
    if (total < 1024) {
        ESP_LOGE(TAG, "Embedded slave image is implausibly small (%u bytes).",
                 (unsigned)total);
        ESP_LOGE(TAG, "Did slave_c6_fw/build.sh run before updater.sh?");
        return;
    }
    ESP_LOGI(TAG, "Embedded slave image: %u bytes", (unsigned)total);

    ESP_LOGI(TAG, "Querying co-processor FW version...");
    esp_hosted_coprocessor_fwver_t ver = {0};
    esp_err_t e = esp_hosted_get_coprocessor_fwversion(&ver);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "current slave: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 ver.major1, ver.minor1, ver.patch1);
    } else {
        ESP_LOGW(TAG, "fwver query: %s — continuing anyway", esp_err_to_name(e));
    }

    ESP_LOGI(TAG, "OTA begin");
    int rc = rpc_ota_begin();
    if (rc != 0) {
        ESP_LOGE(TAG, "rpc_ota_begin failed: %d", rc);
        return;
    }

    size_t off = 0;
    while (off < total) {
        size_t n = total - off;
        if (n > OTA_CHUNK_SIZE) n = OTA_CHUNK_SIZE;
        rc = rpc_ota_write((uint8_t*)(p + off), (uint32_t)n);
        if (rc != 0) {
            ESP_LOGE(TAG, "rpc_ota_write at %u/%u: %d",
                     (unsigned)off, (unsigned)total, rc);
            (void)rpc_ota_end();
            return;
        }
        off += n;
        if ((off % (OTA_CHUNK_SIZE * 32)) < OTA_CHUNK_SIZE || off == total) {
            ESP_LOGI(TAG, "OTA progress: %u / %u (%u%%)",
                     (unsigned)off, (unsigned)total,
                     (unsigned)((uint64_t)off * 100 / total));
        }
    }

    ESP_LOGI(TAG, "OTA end");
    rc = rpc_ota_end();
    if (rc != 0) {
        ESP_LOGE(TAG, "rpc_ota_end failed: %d", rc);
        return;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "===========================================================");
    ESP_LOGI(TAG, " Slave update completed. Power-cycle the Tab5, then run");
    ESP_LOGI(TAG, " 'make flash monitor' from the main project to install the");
    ESP_LOGI(TAG, " IDF 6.0 / esp_hosted 2.x terminal app over this updater.");
    ESP_LOGI(TAG, "===========================================================");
}

void app_main(void) {
    ESP_LOGI(TAG, "Tab5 C6 slave updater (IDF 5.4, esp_hosted 1.4 host)");

    ESP_ERROR_CHECK(i2c_setup());
    c6_power_up();
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "esp_hosted_init() ...");
    esp_err_t e = esp_hosted_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_init failed: %s", esp_err_to_name(e));
        return;
    }
    ESP_LOGI(TAG, "esp_hosted_init OK; resetting + handshaking slave...");

    /* In esp_hosted 1.4 the init() only allocates transport state. The
     * actual slave reset and handshake happen here — this call blocks
     * until the slave sends its INIT event and transport flips to
     * TX_ACTIVE (or retries forever). */
    e = esp_hosted_slave_reset();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_slave_reset failed: %s", esp_err_to_name(e));
        return;
    }

    /* Sanity poll: TX should be ready by now. */
    int waited = 0;
    while (!is_transport_tx_ready() && waited < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
    if (!is_transport_tx_ready()) {
        ESP_LOGE(TAG, "transport still not TX_ACTIVE after slave_reset");
        return;
    }
    ESP_LOGI(TAG, "transport TX_ACTIVE — starting OTA");

    run_ota();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
