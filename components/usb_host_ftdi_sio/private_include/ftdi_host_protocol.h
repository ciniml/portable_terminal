/*
 * SPDX-FileCopyrightText: 2024 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "usb/ftdi_host_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FTDI SIO Request Codes (vendor-specific control requests)
 */
#define FTDI_SIO_RESET              0  // Reset the port
#define FTDI_SIO_SET_MODEM_CTRL     1  // Set modem control (DTR/RTS)
#define FTDI_SIO_SET_FLOW_CTRL      2  // Set flow control
#define FTDI_SIO_SET_BAUDRATE       3  // Set baud rate divisor
#define FTDI_SIO_SET_DATA           4  // Set line properties (data/parity/stop bits)
#define FTDI_SIO_GET_MODEM_STATUS   5  // Get modem status (obsolete, use interrupt IN)
#define FTDI_SIO_SET_EVENT_CHAR     6  // Set event character
#define FTDI_SIO_SET_ERROR_CHAR     7  // Set error character
#define FTDI_SIO_SET_LATENCY_TIMER  9  // Set latency timer (default 16ms)
#define FTDI_SIO_GET_LATENCY_TIMER  10 // Get latency timer

/**
 * @brief FTDI SIO Reset values
 */
#define FTDI_SIO_RESET_SIO          0  // Reset device
#define FTDI_SIO_RESET_PURGE_RX     1  // Purge RX buffer
#define FTDI_SIO_RESET_PURGE_TX     2  // Purge TX buffer

/**
 * @brief FTDI modem control bits
 */
#define FTDI_SIO_SET_DTR_MASK       0x01
#define FTDI_SIO_SET_DTR_HIGH       0x0101
#define FTDI_SIO_SET_DTR_LOW        0x0100
#define FTDI_SIO_SET_RTS_MASK       0x02
#define FTDI_SIO_SET_RTS_HIGH       0x0202
#define FTDI_SIO_SET_RTS_LOW        0x0200

/**
 * @brief FTDI control request structure
 *
 * Platform-independent representation of an FTDI vendor control request.
 * This structure can be used for testing on Linux with libusb.
 */
typedef struct {
    uint8_t request;    // FTDI SIO request code
    uint16_t value;     // Request-specific value
    uint16_t index;     // Interface index (0 for single port, 1-4 for multi-port)
} ftdi_control_request_t;

/**
 * @brief Build FTDI reset control request
 *
 * @param[out] req_out Output control request structure
 * @param[in] reset_type Reset type (FTDI_SIO_RESET_SIO, FTDI_SIO_RESET_PURGE_RX, or FTDI_SIO_RESET_PURGE_TX)
 * @return ESP_OK on success
 */
esp_err_t ftdi_protocol_build_reset(ftdi_control_request_t *req_out, uint16_t reset_type);

/**
 * @brief Build FTDI set baud rate control request
 *
 * @param[out] req_out Output control request structure
 * @param[in] baudrate Desired baud rate (300 to 921600)
 * @param[in] chip_type FTDI chip type (affects calculation)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if baud rate is not supported
 */
esp_err_t ftdi_protocol_build_set_baudrate(ftdi_control_request_t *req_out,
                                            uint32_t baudrate,
                                            ftdi_chip_type_t chip_type);

/**
 * @brief Build FTDI set line property control request
 *
 * @param[out] req_out Output control request structure
 * @param[in] bits Data bits (7 or 8)
 * @param[in] stype Stop bits (1, 1.5, or 2)
 * @param[in] parity Parity (none, odd, even, mark, or space)
 * @return ESP_OK on success
 */
esp_err_t ftdi_protocol_build_set_line_property(ftdi_control_request_t *req_out,
                                                 ftdi_data_bits_t bits,
                                                 ftdi_stop_bits_t stype,
                                                 ftdi_parity_t parity);

/**
 * @brief Build FTDI set modem control request (DTR/RTS)
 *
 * @param[out] req_out Output control request structure
 * @param[in] dtr DTR signal state (true = high, false = low)
 * @param[in] rts RTS signal state (true = high, false = low)
 * @return ESP_OK on success
 */
esp_err_t ftdi_protocol_build_set_modem_ctrl(ftdi_control_request_t *req_out,
                                              bool dtr,
                                              bool rts);

/**
 * @brief Build FTDI set latency timer control request
 *
 * The latency timer controls how long the device waits before sending
 * a packet with less than 62 bytes. Default is 16ms.
 *
 * @param[out] req_out Output control request structure
 * @param[in] latency_ms Latency in milliseconds (1-255)
 * @return ESP_OK on success
 */
esp_err_t ftdi_protocol_build_set_latency_timer(ftdi_control_request_t *req_out,
                                                 uint8_t latency_ms);

/**
 * @brief Parse modem status from FTDI bulk IN packet
 *
 * FTDI devices include modem status in the first 2 bytes of every Bulk IN packet.
 *
 * @param[in] data Pointer to first 2 bytes of bulk IN packet
 * @param[out] status_out Output modem status structure
 * @return ESP_OK on success
 */
esp_err_t ftdi_protocol_parse_modem_status(const uint8_t data[2],
                                            ftdi_modem_status_t *status_out);

/**
 * @brief Calculate FTDI baud rate divisor
 *
 * FTDI devices use a divisor-based baud rate calculation with fractional support.
 * For FT232R: baudrate = 3000000 / divisor
 * Divisor encoding supports fractional parts: 0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875
 *
 * @param[in] baudrate Desired baud rate
 * @param[in] chip_type FTDI chip type (different chips have different base clocks)
 * @param[out] value_out Lower 16 bits of divisor (wValue field)
 * @param[out] index_out Upper 16 bits of divisor (wIndex field)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if baud rate is not achievable
 */
esp_err_t ftdi_calculate_baudrate_divisor(uint32_t baudrate,
                                          ftdi_chip_type_t chip_type,
                                          uint16_t *value_out,
                                          uint16_t *index_out);

#ifdef __cplusplus
}
#endif
