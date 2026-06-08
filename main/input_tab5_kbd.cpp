// Tab5 clip-on QWERTY keyboard (ExtPort1, I2C 0x6D) — standalone driver.
//
// Talks the register-level protocol that M5Stack's `unit_Tab5Keyboard.hpp`
// documents (see the comments below). We sidestep M5UnitUnified entirely
// here: pulling that framework in tripped an unresolved C++ static-init /
// FreeRTOS-heap interaction under our IDF 6.0 build that we couldn't root-
// cause in-session, so this driver speaks raw `esp_driver_i2c` + GPIO ISR
// instead. About 200 lines, no external library.
//
// Operation:
//   - Configure HID mode (REG_MODE_KEYBOARD = 1). The firmware then
//     reports key events as a {modifier, keycode} pair from REG_HID_EVENT
//     using the standard USB HID Keyboard Page (0x07) — exactly the same
//     bytes a USB HID dongle would produce, so we run them through the
//     shared main/hid_translate translator.
//   - Wire the active-low INT pin (GPIO50) to a binary semaphore via
//     gpio_isr_handler; the reader task sleeps on it and drains every
//     queued event on each pulse, then clears the INT by writing 0 to
//     REG_INT_STAT. Falls back to a 100 ms polling tick if no INT GPIO is
//     configured (cfg.int_gpio < 0).
//
// Register map (excerpt — full table is in M5Unit-KEYBOARD's
// unit_Tab5Keyboard.hpp):
//   0x00 INT_CFG     R/W  per-mode INT enable; bit0=Normal bit1=HID bit2=Char
//   0x01 INT_STAT    R/W  pending status; write 0 to clear + release INT pin
//   0x02 EVENT_NUM   R/W  queue length 0..32; write 0 clears queue + INT
//   0x10 MODE_KEYBD  R/W  0=Normal 1=HID 2=Character (default 0)
//   0x30 HID_EVENT   R    2-byte: [0x30]=modifier [0x31]=HID keycode
//                         all 0xFF = queue empty for this mode
//   0xFE FIRMWARE_VER R   single-byte firmware revision

#include "input_tab5_kbd.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hid_translate.hpp"

namespace tab5 {

namespace {

constexpr const char* kTag = "tab5_kbd";

// Register addresses (excerpt — see file header for the full map).
constexpr uint8_t kRegIntCfg        = 0x00;
constexpr uint8_t kRegIntStat       = 0x01;
constexpr uint8_t kRegEventNum      = 0x02;
constexpr uint8_t kRegModeKeyboard  = 0x10;
constexpr uint8_t kRegHidEvent      = 0x30;
constexpr uint8_t kRegFirmwareVer   = 0xFE;

// Mode register values.
constexpr uint8_t kModeNormal       = 0x00;
constexpr uint8_t kModeHid          = 0x01;
// constexpr uint8_t kModeCharacter = 0x02;  // unused

// INT_CFG bits.
constexpr uint8_t kIntEnNormal      = 0x01;
constexpr uint8_t kIntEnHid         = 0x02;
constexpr uint8_t kIntEnCharacter   = 0x04;

// HID-mode queue-empty sentinel — both bytes report 0xFF.
constexpr uint8_t kHidEventEmpty    = 0xFF;

ByteSink                 g_sink;
TaskHandle_t             g_task = nullptr;
i2c_master_bus_handle_t  g_bus  = nullptr;
i2c_master_dev_handle_t  g_dev  = nullptr;
SemaphoreHandle_t        g_int_sem = nullptr;   // signalled by GPIO ISR
int                      g_int_gpio = -1;
std::atomic<bool>        g_isr_installed{false};

esp_err_t reg_read(uint8_t reg, uint8_t* buf, size_t len) {
    return i2c_master_transmit_receive(g_dev, &reg, 1, buf, len,
                                       /*timeout_ms=*/50);
}
esp_err_t reg_write(uint8_t reg, uint8_t value) {
    const uint8_t tx[2] = { reg, value };
    return i2c_master_transmit(g_dev, tx, 2, /*timeout_ms=*/50);
}

void IRAM_ATTR isr_handler(void* /*arg*/) {
    BaseType_t hp_woken = pdFALSE;
    xSemaphoreGiveFromISR(g_int_sem, &hp_woken);
    portYIELD_FROM_ISR(hp_woken);
}

[[noreturn]] void reader_task(void* /*arg*/) {
    while (true) {
        // INT-driven when the GPIO ISR is up; otherwise 100 ms polling.
        if (g_int_sem) {
            xSemaphoreTake(g_int_sem, pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // Drain everything the firmware queued. EVENT_NUM auto-decrements
        // on each HID_EVENT read, so this loop terminates naturally.
        for (int safety = 32; safety > 0; --safety) {
            uint8_t count = 0;
            if (reg_read(kRegEventNum, &count, 1) != ESP_OK) break;
            if (count == 0) break;

            uint8_t ev[2] = { 0, 0 };
            if (reg_read(kRegHidEvent, ev, 2) != ESP_OK) break;
            const uint8_t modifier = ev[0];
            const uint8_t keycode  = ev[1];
            if (keycode == kHidEventEmpty && modifier == kHidEventEmpty) break;
            if (keycode == 0) continue;   // modifier-only release event

            uint8_t buf[hid_translate::kMaxBytes];
            size_t  n = hid_translate::emit_key(keycode, modifier, buf);
            if (n && g_sink) {
                g_sink(std::span<const uint8_t>(buf, n));
            }
        }

        // Release the INT pin so the next event triggers another ISR.
        // Per the datasheet writing 0 to INT_STAT clears the status and
        // releases the INT signal.
        (void)reg_write(kRegIntStat, 0x00);
    }
}

}  // namespace

term::Result<void> start_tab5_kbd_input(ByteSink sink,
                                       const Tab5KbdConfig& cfg) {
    if (g_task) {
        ESP_LOGW(kTag, "already started");
        return {};
    }

    g_sink     = std::move(sink);
    g_int_gpio = cfg.int_gpio;

    // --- I2C bus ----------------------------------------------------
    // M5Unified's board init claims I2C_NUM_0 (Ex_I2C, GPIO 53/54) and
    // I2C_NUM_1 (In_I2C, GPIO 31/32) via the legacy driver/i2c.h API,
    // and the keyboard is on different pins (ExtPort1 GPIO 0/1). Letting
    // the new driver auto-pick a free controller (port = -1) lands on
    // I2C_NUM_2 on ESP32-P4 (the LP_I2C controller exposed in the
    // unified i2c_port_t enum); the legacy buses keep working in
    // parallel.
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port          = -1;   // auto-select
    bus_cfg.sda_io_num        = static_cast<gpio_num_t>(cfg.sda_gpio);
    bus_cfg.scl_io_num        = static_cast<gpio_num_t>(cfg.scl_gpio);
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &g_bus);
    if (err != ESP_OK || !g_bus) {
        ESP_LOGE(kTag, "i2c_new_master_bus -> %s", esp_err_to_name(err));
        return std::unexpected(term::Error::NotInitialized);
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = 0x6D;
    dev_cfg.scl_speed_hz    = cfg.i2c_hz;
    err = i2c_master_bus_add_device(g_bus, &dev_cfg, &g_dev);
    if (err != ESP_OK || !g_dev) {
        ESP_LOGE(kTag, "bus_add_device -> %s", esp_err_to_name(err));
        i2c_del_master_bus(g_bus);
        g_bus = nullptr;
        return std::unexpected(term::Error::NotInitialized);
    }

    // --- Device handshake ------------------------------------------
    uint8_t fw = 0;
    err = reg_read(kRegFirmwareVer, &fw, 1);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "read FIRMWARE_VER -> %s (keyboard plugged in?)",
                 esp_err_to_name(err));
        i2c_master_bus_rm_device(g_dev);
        i2c_del_master_bus(g_bus);
        g_dev = nullptr; g_bus = nullptr;
        return std::unexpected(term::Error::NotInitialized);
    }

    // HID mode + INT enabled for HID events only. Clear any stale state
    // first so we start from a known-empty queue.
    (void)reg_write(kRegEventNum, 0);
    (void)reg_write(kRegIntStat, 0);
    err = reg_write(kRegModeKeyboard, kModeHid);
    if (err == ESP_OK) err = reg_write(kRegIntCfg, kIntEnHid);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "configure HID mode -> %s (continuing)",
                 esp_err_to_name(err));
    }

    // --- INT GPIO ---------------------------------------------------
    if (g_int_gpio >= 0) {
        g_int_sem = xSemaphoreCreateBinary();
        gpio_config_t gc = {};
        gc.intr_type    = GPIO_INTR_NEGEDGE;  // active-low INT
        gc.mode         = GPIO_MODE_INPUT;
        gc.pin_bit_mask = 1ULL << g_int_gpio;
        gc.pull_up_en   = GPIO_PULLUP_ENABLE;
        gpio_config(&gc);

        // First instance installs the global ISR service; subsequent
        // instances inherit it (ESP_ERR_INVALID_STATE is fine).
        esp_err_t r = gpio_install_isr_service(0);
        if (r == ESP_OK || r == ESP_ERR_INVALID_STATE) {
            g_isr_installed = true;
            gpio_isr_handler_add(static_cast<gpio_num_t>(g_int_gpio),
                                 &isr_handler, nullptr);
        } else {
            ESP_LOGW(kTag, "gpio_install_isr_service -> %s; falling back to polling",
                     esp_err_to_name(r));
            vSemaphoreDelete(g_int_sem);
            g_int_sem = nullptr;
        }
    }

    ESP_LOGI(kTag, "Tab5 Keyboard ready (fw=0x%02x sda=%d scl=%d int=%d %s)",
             fw, cfg.sda_gpio, cfg.scl_gpio, cfg.int_gpio,
             g_int_sem ? "INT" : "polling");

    BaseType_t ok = xTaskCreate(reader_task, "tab5_kbd",
                                cfg.task_stack, nullptr,
                                tskIDLE_PRIORITY + 3, &g_task);
    if (ok != pdPASS) {
        ESP_LOGE(kTag, "task create failed");
        g_task = nullptr;
        return std::unexpected(term::Error::NotInitialized);
    }
    return {};
}

}  // namespace tab5
