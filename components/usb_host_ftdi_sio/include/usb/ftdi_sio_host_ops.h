/*
 * SPDX-FileCopyrightText: 2024 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_USB_HOST_ENABLE_FTDI_SIO_DRIVER

#include "esp_err.h"
#include "usb/ftdi_host_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set baud rate
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @param[in] baudrate Desired baud rate (300 to 921600 for FT232R)
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_set_baudrate(ftdi_sio_dev_hdl_t ftdi_hdl, uint32_t baudrate);

/**
 * @brief Set line properties
 *
 * Configures data bits, stop bits, and parity.
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @param[in] bits Data bits (7 or 8)
 * @param[in] stype Stop bits (1, 1.5, or 2)
 * @param[in] parity Parity (none, odd, even, mark, or space)
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_set_line_property(ftdi_sio_dev_hdl_t ftdi_hdl,
                                           ftdi_data_bits_t bits,
                                           ftdi_stop_bits_t stype,
                                           ftdi_parity_t parity);

/**
 * @brief Set modem control signals (DTR/RTS)
 *
 * Controls the DTR and RTS output pins.
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @param[in] dtr DTR signal state (true = high, false = low)
 * @param[in] rts RTS signal state (true = high, false = low)
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_set_modem_control(ftdi_sio_dev_hdl_t ftdi_hdl,
                                           bool dtr,
                                           bool rts);

/**
 * @brief Reset device
 *
 * Resets the FTDI device.
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_reset(ftdi_sio_dev_hdl_t ftdi_hdl);

/**
 * @brief Purge RX buffer
 *
 * Clears the device's receive buffer.
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_purge_rx_buffer(ftdi_sio_dev_hdl_t ftdi_hdl);

/**
 * @brief Purge TX buffer
 *
 * Clears the device's transmit buffer.
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_purge_tx_buffer(ftdi_sio_dev_hdl_t ftdi_hdl);

/**
 * @brief Set latency timer
 *
 * The latency timer controls how long the device waits before sending
 * a packet with less than 62 bytes. Default is 16ms.
 * Valid range: 1-255 ms
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @param[in] latency_ms Latency in milliseconds (1-255)
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_set_latency_timer(ftdi_sio_dev_hdl_t ftdi_hdl, uint8_t latency_ms);

/**
 * @brief Get current modem status
 *
 * Returns the cached modem status from the most recent Bulk IN packet.
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @param[out] status Output modem status
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_get_modem_status(ftdi_sio_dev_hdl_t ftdi_hdl,
                                          ftdi_modem_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_USB_HOST_ENABLE_FTDI_SIO_DRIVER */
