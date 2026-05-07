/*
 * SPDX-FileCopyrightText: 2025-2026 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <sys/queue.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "usb/ftdi_host_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FTDI device structure
 *
 * Internal structure for managing FTDI device state.
 * Follows the pattern from CDC-ACM driver.
 */
typedef struct ftdi_dev_s ftdi_dev_t;

struct ftdi_dev_s {
    // USB device handle
    usb_device_handle_t dev_hdl;

    // Device information
    ftdi_chip_type_t chip_type;
    uint16_t vid;
    uint16_t pid;

    // Data endpoints (Bulk IN/OUT)
    struct {
        usb_transfer_t *out_xfer;         // Bulk OUT transfer
        usb_transfer_t *in_xfer;          // Bulk IN transfer
        SemaphoreHandle_t out_mux;        // Mutex for OUT transfer
        uint16_t in_mps;                  // IN endpoint max packet size
        uint8_t *in_data_buffer_base;     // Base pointer for IN buffer (for freeing)
        const usb_intf_desc_t *intf_desc; // Interface descriptor
        uint8_t bulk_in_ep;               // Bulk IN endpoint address
        uint8_t bulk_out_ep;              // Bulk OUT endpoint address
    } data;

    // Control transfer
    usb_transfer_t *ctrl_transfer;
    SemaphoreHandle_t ctrl_mux;           // Mutex for control transfer

    // Callbacks
    ftdi_sio_data_callback_t data_cb;
    ftdi_sio_host_dev_callback_t event_cb;
    void *cb_arg;

    // Current modem status (cached from Bulk IN packets)
    ftdi_modem_status_t modem_status_current;

    // Device list entry
    SLIST_ENTRY(ftdi_dev_s) list_entry;
};

/**
 * @brief Get device handle from FTDI handle
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @return USB device handle
 */
static inline usb_device_handle_t ftdi_host_get_usb_device_handle(ftdi_sio_dev_hdl_t ftdi_hdl)
{
    ftdi_dev_t *ftdi_dev = (ftdi_dev_t *)ftdi_hdl;
    return ftdi_dev->dev_hdl;
}

/**
 * @brief Get chip type from FTDI handle
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @return Chip type
 */
static inline ftdi_chip_type_t ftdi_host_get_chip_type(ftdi_sio_dev_hdl_t ftdi_hdl)
{
    ftdi_dev_t *ftdi_dev = (ftdi_dev_t *)ftdi_hdl;
    return ftdi_dev->chip_type;
}

/**
 * @brief Get current modem status from FTDI handle
 *
 * @param[in] ftdi_hdl FTDI device handle
 * @param[out] status_out Output modem status
 * @return ESP_OK on success
 */
static inline esp_err_t ftdi_host_get_modem_status(ftdi_sio_dev_hdl_t ftdi_hdl,
                                                    ftdi_modem_status_t *status_out)
{
    if (ftdi_hdl == NULL || status_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ftdi_dev_t *ftdi_dev = (ftdi_dev_t *)ftdi_hdl;
    *status_out = ftdi_dev->modem_status_current;
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif
