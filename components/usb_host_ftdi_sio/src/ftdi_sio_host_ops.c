/*
 * SPDX-FileCopyrightText: 2024 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "usb/usb_types_ch9.h"
#include "usb/ftdi_sio_host.h"
#include "usb/ftdi_sio_host_ops.h"
#include "esp_private/ftdi_host_common.h"
#include "ftdi_host_protocol.h"

static const char *TAG = "ftdi_sio_ops";

esp_err_t ftdi_sio_host_set_baudrate(ftdi_sio_dev_hdl_t ftdi_hdl, uint32_t baudrate)
{
    ESP_RETURN_ON_FALSE(ftdi_hdl, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    ftdi_dev_t *ftdi_dev = (ftdi_dev_t *)ftdi_hdl;
    ftdi_control_request_t req;

    ESP_RETURN_ON_ERROR(
        ftdi_protocol_build_set_baudrate(&req, baudrate, ftdi_dev->chip_type),
        TAG, "Failed to build baudrate request");

    return ftdi_sio_host_send_custom_request(
               ftdi_hdl,
               USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
               req.request,
               req.value,
               req.index,
               0,
               NULL);
}

esp_err_t ftdi_sio_host_set_line_property(ftdi_sio_dev_hdl_t ftdi_hdl,
        ftdi_data_bits_t bits,
        ftdi_stop_bits_t stype,
        ftdi_parity_t parity)
{
    ESP_RETURN_ON_FALSE(ftdi_hdl, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    ftdi_control_request_t req;

    ESP_RETURN_ON_ERROR(
        ftdi_protocol_build_set_line_property(&req, bits, stype, parity),
        TAG, "Failed to build line property request");

    return ftdi_sio_host_send_custom_request(
               ftdi_hdl,
               USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
               req.request,
               req.value,
               req.index,
               0,
               NULL);
}

esp_err_t ftdi_sio_host_set_modem_control(ftdi_sio_dev_hdl_t ftdi_hdl, bool dtr, bool rts)
{
    ESP_RETURN_ON_FALSE(ftdi_hdl, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    ftdi_control_request_t req;

    ESP_RETURN_ON_ERROR(
        ftdi_protocol_build_set_modem_ctrl(&req, dtr, rts),
        TAG, "Failed to build modem control request");

    return ftdi_sio_host_send_custom_request(
               ftdi_hdl,
               USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
               req.request,
               req.value,
               req.index,
               0,
               NULL);
}

esp_err_t ftdi_sio_host_reset(ftdi_sio_dev_hdl_t ftdi_hdl)
{
    ESP_RETURN_ON_FALSE(ftdi_hdl, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    ftdi_control_request_t req;

    ESP_RETURN_ON_ERROR(
        ftdi_protocol_build_reset(&req, FTDI_SIO_RESET_SIO),
        TAG, "Failed to build reset request");

    return ftdi_sio_host_send_custom_request(
               ftdi_hdl,
               USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
               req.request,
               req.value,
               req.index,
               0,
               NULL);
}

esp_err_t ftdi_sio_host_purge_rx_buffer(ftdi_sio_dev_hdl_t ftdi_hdl)
{
    ESP_RETURN_ON_FALSE(ftdi_hdl, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    ftdi_control_request_t req;

    ESP_RETURN_ON_ERROR(
        ftdi_protocol_build_reset(&req, FTDI_SIO_RESET_PURGE_RX),
        TAG, "Failed to build purge RX request");

    return ftdi_sio_host_send_custom_request(
               ftdi_hdl,
               USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
               req.request,
               req.value,
               req.index,
               0,
               NULL);
}

esp_err_t ftdi_sio_host_purge_tx_buffer(ftdi_sio_dev_hdl_t ftdi_hdl)
{
    ESP_RETURN_ON_FALSE(ftdi_hdl, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    ftdi_control_request_t req;

    ESP_RETURN_ON_ERROR(
        ftdi_protocol_build_reset(&req, FTDI_SIO_RESET_PURGE_TX),
        TAG, "Failed to build purge TX request");

    return ftdi_sio_host_send_custom_request(
               ftdi_hdl,
               USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
               req.request,
               req.value,
               req.index,
               0,
               NULL);
}

esp_err_t ftdi_sio_host_set_latency_timer(ftdi_sio_dev_hdl_t ftdi_hdl, uint8_t latency_ms)
{
    ESP_RETURN_ON_FALSE(ftdi_hdl, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    ftdi_control_request_t req;

    ESP_RETURN_ON_ERROR(
        ftdi_protocol_build_set_latency_timer(&req, latency_ms),
        TAG, "Failed to build latency timer request");

    return ftdi_sio_host_send_custom_request(
               ftdi_hdl,
               USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
               req.request,
               req.value,
               req.index,
               0,
               NULL);
}

esp_err_t ftdi_sio_host_get_modem_status(ftdi_sio_dev_hdl_t ftdi_hdl,
        ftdi_modem_status_t *status)
{
    return ftdi_host_get_modem_status(ftdi_hdl, status);
}
