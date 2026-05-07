/*
 * SPDX-FileCopyrightText: 2024 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "usb/usb_types_ch9.h"
#include "usb/ftdi_host_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Parsed FTDI interface information
 */
typedef struct {
    const usb_intf_desc_t *intf_desc;  // Interface descriptor
    uint8_t bulk_in_ep;                // Bulk IN endpoint address
    uint16_t bulk_in_mps;              // Bulk IN max packet size
    uint8_t bulk_out_ep;               // Bulk OUT endpoint address
    uint16_t bulk_out_mps;             // Bulk OUT max packet size
} ftdi_intf_info_t;

/**
 * @brief Detect FTDI chip type from USB PID
 *
 * @param[in] pid USB Product ID
 * @return Chip type
 */
ftdi_chip_type_t ftdi_parse_chip_type(uint16_t pid);

/**
 * @brief Parse FTDI device interface descriptors
 *
 * FTDI devices use vendor-specific interface class with 2 bulk endpoints.
 * This function finds and validates the interface descriptors.
 *
 * @param[in] config_desc Configuration descriptor
 * @param[in] intf_idx Interface index (0 for single port, 1-3 for multi-port)
 * @param[out] info_out Parsed interface information
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if interface not found
 */
esp_err_t ftdi_parse_interface_descriptor(const usb_config_desc_t *config_desc,
                                           uint8_t intf_idx,
                                           ftdi_intf_info_t *info_out);

#ifdef __cplusplus
}
#endif
