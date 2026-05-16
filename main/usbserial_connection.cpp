#include "usbserial_connection.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/ftdi_host_types.h"
#include "usb/ftdi_sio_host.h"
#include "usb/ftdi_sio_host_ops.h"

namespace tab5 {

namespace {

constexpr const char* kTag = "usb_serial";

struct Session {
    ftdi_sio_dev_hdl_t hdl = nullptr;
    ByteSink           rx;
    std::atomic<bool>  running{false};
    std::atomic<bool>  driver_installed{false};
};

Session g_sess;

void on_rx(const uint8_t* data, size_t len, void*) {
    if (g_sess.rx && len) {
        g_sess.rx(std::span<const uint8_t>(data, len));
    }
}

void on_event(ftdi_sio_host_dev_event_t event, void*) {
    if (event == FTDI_SIO_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(kTag, "device disconnected");
        g_sess.running.store(false);
        if (g_sess.hdl) {
            ftdi_sio_host_close(g_sess.hdl);
            g_sess.hdl = nullptr;
        }
    } else if (event == FTDI_SIO_HOST_ERROR) {
        ESP_LOGW(kTag, "device error");
    }
}

}  // namespace

UsbSerialConnection::UsbSerialConnection(const UsbSerialConfig& cfg)
    : cfg_(cfg) {
    snprintf(label_, sizeof(label_), "ftdi @ %lu %u%c%u",
             static_cast<unsigned long>(cfg.baud),
             cfg.data_bits,
             cfg.parity == 0 ? 'N' :
             cfg.parity == 1 ? 'O' :
             cfg.parity == 2 ? 'E' :
             cfg.parity == 3 ? 'M' :
             cfg.parity == 4 ? 'S' : '?',
             cfg.stop_bits == 0 ? 1 : cfg.stop_bits == 1 ? 2 : 2);
}

UsbSerialConnection::~UsbSerialConnection() { stop(); }

bool UsbSerialConnection::start(ByteSink rx_sink) {
    if (g_sess.running.load()) {
        ESP_LOGW(kTag, "already running");
        return false;
    }
    if (!g_sess.driver_installed.load()) {
        ftdi_sio_host_driver_config_t drv = FTDI_SIO_HOST_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = ftdi_sio_host_install(&drv);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "ftdi_sio_host_install -> %s",
                     esp_err_to_name(err));
            return false;
        }
        g_sess.driver_installed.store(true);
    }

    g_sess.rx = std::move(rx_sink);

    ftdi_sio_host_device_config_t dev = FTDI_SIO_HOST_DEVICE_CONFIG_DEFAULT();
    dev.data_cb  = on_rx;
    dev.event_cb = on_event;
    dev.connection_timeout_ms = 2000;
    esp_err_t err = ftdi_sio_host_open(FTDI_HOST_ANY_VID, FTDI_HOST_ANY_PID,
                                       0, &dev, &g_sess.hdl);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "ftdi_sio_host_open -> %s (no FTDI plugged in?)",
                 esp_err_to_name(err));
        g_sess.hdl = nullptr;
        return false;
    }

    ftdi_sio_host_set_baudrate(g_sess.hdl, cfg_.baud);
    ftdi_sio_host_set_line_property(g_sess.hdl,
        static_cast<ftdi_data_bits_t>(cfg_.data_bits),
        static_cast<ftdi_stop_bits_t>(cfg_.stop_bits),
        static_cast<ftdi_parity_t>(cfg_.parity));

    g_sess.running.store(true);
    ESP_LOGI(kTag, "%s", label_);
    return true;
}

void UsbSerialConnection::stop() {
    if (!g_sess.running.load()) return;
    g_sess.running.store(false);
    if (g_sess.hdl) {
        ftdi_sio_host_close(g_sess.hdl);
        g_sess.hdl = nullptr;
    }
}

void UsbSerialConnection::send(std::span<const uint8_t> bytes) {
    if (!g_sess.running.load() || !g_sess.hdl || bytes.empty()) return;
    ftdi_sio_host_data_tx_blocking(g_sess.hdl, bytes.data(), bytes.size(),
                                   /*timeout_ms=*/200);
}

bool UsbSerialConnection::is_connected() const {
    return g_sess.running.load();
}

}  // namespace tab5
