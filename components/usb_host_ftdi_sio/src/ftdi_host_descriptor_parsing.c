/*
 * SPDX-FileCopyrightText: 2024 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "usb/usb_helpers.h"
#include "ftdi_host_descriptor_parsing.h"
#include "esp_log.h"

#define TAG "ftdi_sio"

ftdi_chip_type_t ftdi_parse_chip_type(uint16_t pid)
{
    switch (pid) {
    case FTDI_PID_FT232R:
        return FTDI_CHIP_TYPE_232R;
    case FTDI_PID_FT232H:
        return FTDI_CHIP_TYPE_232H;
    case FTDI_PID_FT2232D:
        return FTDI_CHIP_TYPE_2232D;
    case FTDI_PID_FT4232H:
        return FTDI_CHIP_TYPE_4232H;
    case FTDI_PID_FT230X:
        return FTDI_CHIP_TYPE_230X;
    default:
        return FTDI_CHIP_TYPE_UNKNOWN;
    }
}

esp_err_t ftdi_parse_interface_descriptor(const usb_config_desc_t *config_desc,
                                           uint8_t intf_idx,
                                           ftdi_intf_info_t *info_out)
{
    if (config_desc == NULL || info_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info_out, 0, sizeof(ftdi_intf_info_t));

    int offset = 0;
    const usb_intf_desc_t *intf_desc = NULL;
    const usb_ep_desc_t *bulk_in_ep_desc = NULL;
    const usb_ep_desc_t *bulk_out_ep_desc = NULL;

    // Find the interface descriptor
    intf_desc = usb_parse_interface_descriptor(config_desc, intf_idx, 0, &offset);
    if (intf_desc == NULL) {
        ESP_LOGE(TAG, "Interface descriptor not found for index %d", intf_idx);
        return ESP_ERR_NOT_FOUND;
    }

    // FTDI devices use vendor-specific interface class (0xFF)
    // and have 2 bulk endpoints (IN and OUT)
    if (intf_desc->bInterfaceClass != USB_CLASS_VENDOR_SPEC) {
        ESP_LOGE(TAG, "Interface class is not vendor-specific: 0x%02X", intf_desc->bInterfaceClass);
        return ESP_ERR_NOT_FOUND;
    }

    if (intf_desc->bNumEndpoints < 2) {
        ESP_LOGE(TAG, "Interface does not have enough endpoints: %d", intf_desc->bNumEndpoints);
        return ESP_ERR_NOT_FOUND;
    }

    // Find bulk endpoints
    for (int i = 0; i < intf_desc->bNumEndpoints; i++) {
        int interface_descriptor_offset = offset;
        const usb_ep_desc_t *ep_desc = usb_parse_endpoint_descriptor_by_index(
            intf_desc, i, config_desc->wTotalLength, &interface_descriptor_offset);

        if (ep_desc == NULL) {
            ESP_LOGW(TAG, "Could not parse endpoint descriptor at index %d", i);
            continue;
        }

        ESP_LOGI(TAG, "Found endpoint: Address=0x%02X, Attributes=0x%02X",
                 ep_desc->bEndpointAddress, ep_desc->bmAttributes);

        // Check if this is a bulk endpoint
        if ((ep_desc->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) == USB_BM_ATTRIBUTES_XFER_BULK) {
            if (ep_desc->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                // Bulk IN
                bulk_in_ep_desc = ep_desc;
            } else {
                // Bulk OUT
                bulk_out_ep_desc = ep_desc;
            }
        }
    }

    // Verify we found both endpoints
    if (bulk_in_ep_desc == NULL || bulk_out_ep_desc == NULL) {
        ESP_LOGE(TAG, "Could not find both bulk IN and OUT endpoints");
        return ESP_ERR_NOT_FOUND;
    }

    // Fill output structure
    info_out->intf_desc = intf_desc;
    info_out->bulk_in_ep = bulk_in_ep_desc->bEndpointAddress;
    info_out->bulk_in_mps = bulk_in_ep_desc->wMaxPacketSize;
    info_out->bulk_out_ep = bulk_out_ep_desc->bEndpointAddress;
    info_out->bulk_out_mps = bulk_out_ep_desc->wMaxPacketSize;

    return ESP_OK;
}
