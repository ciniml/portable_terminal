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
 * @brief Install FTDI SIO host driver
 *
 * This function initializes the FTDI driver and starts the USB host client task.
 *
 * @param[in] driver_config Driver configuration (NULL for defaults)
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_install(const ftdi_sio_host_driver_config_t *driver_config);

/**
 * @brief Uninstall FTDI SIO host driver
 *
 * This function stops the driver task and releases all resources.
 * All opened devices must be closed before calling this function.
 *
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_uninstall(void);

/**
 * @brief Open FTDI device
 *
 * Opens an FTDI device by VID/PID and interface index.
 * Use FTDI_HOST_ANY_VID/FTDI_HOST_ANY_PID to match any device.
 *
 * @param[in] vid USB Vendor ID (use FTDI_VID for FTDI devices)
 * @param[in] pid USB Product ID (use FTDI_PID_* constants)
 * @param[in] interface_idx Interface index (0 for single port, 1-3 for multi-port)
 * @param[in] dev_config Device configuration (NULL for defaults)
 * @param[out] ftdi_hdl_ret FTDI device handle
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_open(uint16_t vid,
                              uint16_t pid,
                              uint8_t interface_idx,
                              const ftdi_sio_host_device_config_t *dev_config,
                              ftdi_sio_dev_hdl_t *ftdi_hdl_ret);

/**
 * @brief Close FTDI device
 *
 * Closes the device and releases all associated resources.
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_close(ftdi_sio_dev_hdl_t ftdi_hdl);

/**
 * @brief Transmit data (blocking)
 *
 * Sends data to the FTDI device. This function blocks until all data
 * is transmitted or timeout occurs.
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @param[in] data Pointer to data to send
 * @param[in] data_len Length of data to send
 * @param[in] timeout_ms Timeout in milliseconds (0 = wait forever)
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_data_tx_blocking(ftdi_sio_dev_hdl_t ftdi_hdl,
                                          const uint8_t *data,
                                          size_t data_len,
                                          uint32_t timeout_ms);

/**
 * @brief Send custom control request
 *
 * Sends a vendor-specific control request to the device.
 * This is a low-level function for advanced use cases.
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @param[in] bmRequestType Request type
 * @param[in] bRequest Request code
 * @param[in] wValue Value field
 * @param[in] wIndex Index field
 * @param[in] wLength Data length
 * @param[in] data Data buffer (for OUT requests, can be NULL for IN requests)
 * @return ESP_OK on success
 */
esp_err_t ftdi_sio_host_send_custom_request(ftdi_sio_dev_hdl_t ftdi_hdl,
                                             uint8_t bmRequestType,
                                             uint8_t bRequest,
                                             uint16_t wValue,
                                             uint16_t wIndex,
                                             uint16_t wLength,
                                             uint8_t *data);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_USB_HOST_ENABLE_FTDI_SIO_DRIVER */
