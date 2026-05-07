/*
 * SPDX-FileCopyrightText: 2024 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"

#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/ftdi_sio_host.h"
#include "esp_private/ftdi_host_common.h"
#include "ftdi_host_protocol.h"
#include "ftdi_host_descriptor_parsing.h"

static const char *TAG = "ftdi_sio";

// Control transfer constants
#define FTDI_CTRL_TRANSFER_SIZE (64)
#define FTDI_CTRL_TIMEOUT_MS    (5000)

// Default buffer sizes
#define FTDI_DEFAULT_IN_BUFFER_SIZE  (512)
#define FTDI_DEFAULT_OUT_BUFFER_SIZE (512)

// FTDI spinlock
static portMUX_TYPE ftdi_sio_lock = portMUX_INITIALIZER_UNLOCKED;
#define FTDI_SIO_ENTER_CRITICAL()   portENTER_CRITICAL(&ftdi_sio_lock)
#define FTDI_SIO_EXIT_CRITICAL()    portEXIT_CRITICAL(&ftdi_sio_lock)

// FTDI events
#define FTDI_SIO_TEARDOWN          BIT0
#define FTDI_SIO_TEARDOWN_COMPLETE BIT1

// FTDI driver object
typedef struct {
    usb_host_client_handle_t ftdi_client_hdl;
    SemaphoreHandle_t open_close_mutex;
    EventGroupHandle_t event_group;
    ftdi_sio_new_dev_callback_t new_dev_cb;
    void *new_dev_cb_arg;
    SLIST_HEAD(list_dev, ftdi_dev_s) ftdi_devices_list;
} ftdi_sio_obj_t;

static ftdi_sio_obj_t *p_ftdi_sio_obj = NULL;

// Default driver configuration
static const ftdi_sio_host_driver_config_t ftdi_driver_config_default = {
    .driver_task_stack_size = 4096,
    .driver_task_priority = 5,
    .xCoreID = -1,
    .new_dev_cb = NULL,
    .user_arg = NULL,
};

// Forward declarations
static void in_xfer_cb(usb_transfer_t *transfer);
static void out_xfer_cb(usb_transfer_t *transfer);
static void usb_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg);

/**
 * @brief Reset IN transfer to defaults
 */
static void ftdi_reset_in_transfer(ftdi_dev_t *ftdi_dev)
{
    assert(ftdi_dev->data.in_xfer);
    usb_transfer_t *transfer = ftdi_dev->data.in_xfer;
    uint8_t **ptr = (uint8_t **)(&(transfer->data_buffer));
    *ptr = ftdi_dev->data.in_data_buffer_base;
    transfer->num_bytes = transfer->data_buffer_size;
    transfer->num_bytes -= transfer->data_buffer_size % ftdi_dev->data.in_mps;
}

/**
 * @brief FTDI driver handling task
 */
static void ftdi_client_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ftdi_sio_obj_t *ftdi_obj = p_ftdi_sio_obj;
    assert(ftdi_obj->ftdi_client_hdl);

    while (1) {
        usb_host_client_handle_events(ftdi_obj->ftdi_client_hdl, portMAX_DELAY);
        EventBits_t events = xEventGroupGetBits(ftdi_obj->event_group);
        if (events & FTDI_SIO_TEARDOWN) {
            break;
        }
    }

    ESP_LOGD(TAG, "Deregistering client");
    ESP_ERROR_CHECK(usb_host_client_deregister(ftdi_obj->ftdi_client_hdl));
    xEventGroupSetBits(ftdi_obj->event_group, FTDI_SIO_TEARDOWN_COMPLETE);
    vTaskDelete(NULL);
}

/**
 * @brief Cancel transfer and reset endpoint
 */
static esp_err_t ftdi_reset_transfer_endpoint(usb_device_handle_t dev_hdl, usb_transfer_t *transfer)
{
    assert(dev_hdl);
    assert(transfer);

    ESP_RETURN_ON_ERROR(usb_host_endpoint_halt(dev_hdl, transfer->bEndpointAddress), TAG,);
    ESP_RETURN_ON_ERROR(usb_host_endpoint_flush(dev_hdl, transfer->bEndpointAddress), TAG,);
    usb_host_endpoint_clear(dev_hdl, transfer->bEndpointAddress);
    return ESP_OK;
}

/**
 * @brief Data received callback (Bulk IN)
 *
 * IMPORTANT: FTDI devices include modem status in the first 2 bytes of EVERY packet.
 * Each USB packet is in_mps bytes in size. We must process each packet separately
 * to strip the modem status bytes from each packet.
 */
static void in_xfer_cb(usb_transfer_t *transfer)
{
    ftdi_dev_t *ftdi_dev = (ftdi_dev_t *)transfer->context;
    assert(ftdi_dev);

    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        size_t total_bytes = transfer->actual_num_bytes;
        size_t offset = 0;
        const uint16_t mps = ftdi_dev->data.in_mps;

        // Process received data in chunks of MPS (Max Packet Size)
        // Each chunk contains: [2 bytes modem status][actual data]
        while (offset < total_bytes) {
            // Calculate chunk size (either full MPS or remaining bytes)
            size_t chunk_size = (total_bytes - offset) > mps ? mps : (total_bytes - offset);

            // Each packet must have at least 2 bytes for modem status
            if (chunk_size >= 2) {
                // Extract modem status from first 2 bytes of this packet
                ftdi_modem_status_t new_status;
                ftdi_protocol_parse_modem_status(transfer->data_buffer + offset, &new_status);

                // Check if modem status changed
                if (memcmp(&new_status, &ftdi_dev->modem_status_current, sizeof(ftdi_modem_status_t)) != 0) {
                    ftdi_dev->modem_status_current = new_status;
                    // Notify user of modem status change
                    if (ftdi_dev->event_cb) {
                        ftdi_dev->event_cb(FTDI_SIO_HOST_MODEM_STATUS, ftdi_dev->cb_arg);
                    }
                }

                // Pass actual data (skip first 2 bytes) to user callback
                size_t data_len = chunk_size - 2;
                if (data_len > 0 && ftdi_dev->data_cb) {
                    ftdi_dev->data_cb(transfer->data_buffer + offset + 2,
                                     data_len,
                                     ftdi_dev->cb_arg);
                }
            }

            offset += chunk_size;
        }
    } else if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGD(TAG, "Device disconnected");
        return;
    } else {
        ESP_LOGD(TAG, "Bulk IN transfer error: %d", transfer->status);
    }

    // Resubmit for continuous polling
    ESP_ERROR_CHECK(usb_host_transfer_submit(transfer));
}

/**
 * @brief Data send callback (Bulk OUT and Control)
 */
static void out_xfer_cb(usb_transfer_t *transfer)
{
    ftdi_dev_t *ftdi_dev = (ftdi_dev_t *)transfer->context;
    assert(ftdi_dev);

    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGD(TAG, "Transfer failed: status %d", transfer->status);
    }
}

/**
 * @brief USB Host Client event callback
 */
static void usb_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGD(TAG, "New USB device");
        if (p_ftdi_sio_obj->new_dev_cb) {
            const usb_device_desc_t *device_desc;
            usb_device_handle_t dev_hdl;
            // ESP-IDF v6.0: Open device temporarily to get descriptor
            esp_err_t err = usb_host_device_open(p_ftdi_sio_obj->ftdi_client_hdl,
                                                  event_msg->new_dev.address,
                                                  &dev_hdl);
            if (err == ESP_OK) {
                ESP_ERROR_CHECK(usb_host_get_device_descriptor(dev_hdl, &device_desc));
                if( device_desc->idVendor == FTDI_VID ) {
                    p_ftdi_sio_obj->new_dev_cb(device_desc->idVendor, device_desc->idProduct,
                                           p_ftdi_sio_obj->new_dev_cb_arg);
                }
                usb_host_device_close(p_ftdi_sio_obj->ftdi_client_hdl, dev_hdl);
            }
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE: {
        ESP_LOGD(TAG, "USB device removed");
        ftdi_dev_t *ftdi_dev;
        FTDI_SIO_ENTER_CRITICAL();
        SLIST_FOREACH(ftdi_dev, &p_ftdi_sio_obj->ftdi_devices_list, list_entry) {
            // ESP-IDF v6.0: dev_gone now provides dev_hdl directly
            if (event_msg->dev_gone.dev_hdl == ftdi_dev->dev_hdl) {
                if (ftdi_dev->event_cb) {
                    ftdi_dev->event_cb(FTDI_SIO_HOST_DEVICE_DISCONNECTED, ftdi_dev->cb_arg);
                }
            }
        }
        FTDI_SIO_EXIT_CRITICAL();
        break;
    }
    default:
        break;
    }
}

/**
 * @brief Free transfers
 */
static void ftdi_transfers_free(ftdi_dev_t *ftdi_dev)
{
    if (ftdi_dev->data.in_xfer) {
        usb_host_transfer_free(ftdi_dev->data.in_xfer);
        ftdi_dev->data.in_xfer = NULL;
    }
    if (ftdi_dev->data.out_xfer) {
        usb_host_transfer_free(ftdi_dev->data.out_xfer);
        ftdi_dev->data.out_xfer = NULL;
    }
    if (ftdi_dev->ctrl_transfer) {
        usb_host_transfer_free(ftdi_dev->ctrl_transfer);
        ftdi_dev->ctrl_transfer = NULL;
    }
    if (ftdi_dev->data.out_mux) {
        vSemaphoreDelete(ftdi_dev->data.out_mux);
        ftdi_dev->data.out_mux = NULL;
    }
    if (ftdi_dev->ctrl_mux) {
        vSemaphoreDelete(ftdi_dev->ctrl_mux);
        ftdi_dev->ctrl_mux = NULL;
    }
}

/**
 * @brief Allocate transfers for FTDI device
 */
static esp_err_t ftdi_transfers_allocate(ftdi_dev_t *ftdi_dev, size_t in_buf_size, size_t out_buf_size)
{
    esp_err_t ret;

    // Allocate Bulk IN transfer
    ESP_GOTO_ON_ERROR(
        usb_host_transfer_alloc(in_buf_size, 0, &ftdi_dev->data.in_xfer),
        err, TAG, "Unable to allocate IN transfer");
    ftdi_dev->data.in_xfer->device_handle = ftdi_dev->dev_hdl;  // ESP-IDF v6.0: Set device handle
    ftdi_dev->data.in_xfer->callback = in_xfer_cb;
    ftdi_dev->data.in_xfer->context = ftdi_dev;
    ftdi_dev->data.in_xfer->bEndpointAddress = ftdi_dev->data.bulk_in_ep;
    ftdi_dev->data.in_xfer->num_bytes = in_buf_size;
    ftdi_dev->data.in_data_buffer_base = ftdi_dev->data.in_xfer->data_buffer;

    // Allocate Bulk OUT transfer
    ESP_GOTO_ON_ERROR(
        usb_host_transfer_alloc(out_buf_size, 0, &ftdi_dev->data.out_xfer),
        err, TAG, "Unable to allocate OUT transfer");
    ftdi_dev->data.out_xfer->device_handle = ftdi_dev->dev_hdl;  // ESP-IDF v6.0: Set device handle
    ftdi_dev->data.out_xfer->callback = out_xfer_cb;
    ftdi_dev->data.out_xfer->context = ftdi_dev;
    ftdi_dev->data.out_xfer->bEndpointAddress = ftdi_dev->data.bulk_out_ep;
    ftdi_dev->data.out_xfer->timeout_ms = 1000;

    // Allocate Control transfer
    ESP_GOTO_ON_ERROR(
        usb_host_transfer_alloc(FTDI_CTRL_TRANSFER_SIZE, 0, &ftdi_dev->ctrl_transfer),
        err, TAG, "Unable to allocate CTRL transfer");
    ftdi_dev->ctrl_transfer->device_handle = ftdi_dev->dev_hdl;  // ESP-IDF v6.0: Set device handle
    ftdi_dev->ctrl_transfer->callback = out_xfer_cb;
    ftdi_dev->ctrl_transfer->context = ftdi_dev;
    ftdi_dev->ctrl_transfer->bEndpointAddress = 0;
    ftdi_dev->ctrl_transfer->timeout_ms = FTDI_CTRL_TIMEOUT_MS;

    // Create mutexes
    ftdi_dev->data.out_mux = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(ftdi_dev->data.out_mux, ESP_ERR_NO_MEM, err, TAG, "Unable to create OUT mutex");

    ftdi_dev->ctrl_mux = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(ftdi_dev->ctrl_mux, ESP_ERR_NO_MEM, err, TAG, "Unable to create CTRL mutex");

    return ESP_OK;

err:
    ftdi_transfers_free(ftdi_dev);
    return ret;
}

/**
 * @brief Start FTDI device
 */
static esp_err_t ftdi_start(ftdi_dev_t *ftdi_dev)
{
    assert(ftdi_dev);

    // Claim data interface
    ESP_RETURN_ON_ERROR(
        usb_host_interface_claim(
            p_ftdi_sio_obj->ftdi_client_hdl,
            ftdi_dev->dev_hdl,
            ftdi_dev->data.intf_desc->bInterfaceNumber,
            ftdi_dev->data.intf_desc->bAlternateSetting),
        TAG, "Could not claim interface");

    // Start polling IN endpoint
    if (ftdi_dev->data.in_xfer) {
        ESP_LOGD(TAG, "Submitting poll for BULK IN transfer");
        ESP_ERROR_CHECK(usb_host_transfer_submit(ftdi_dev->data.in_xfer));
    }

    // Add device to list
    FTDI_SIO_ENTER_CRITICAL();
    SLIST_INSERT_HEAD(&p_ftdi_sio_obj->ftdi_devices_list, ftdi_dev, list_entry);
    FTDI_SIO_EXIT_CRITICAL();

    return ESP_OK;
}

/**
 * @brief Remove FTDI device
 */
static void ftdi_device_remove(ftdi_dev_t *ftdi_dev)
{
    assert(ftdi_dev);
    ftdi_transfers_free(ftdi_dev);
    usb_host_device_close(p_ftdi_sio_obj->ftdi_client_hdl, ftdi_dev->dev_hdl);
    free(ftdi_dev);
}

/**
 * @brief Find and open USB device
 */
static esp_err_t ftdi_find_and_open_usb_device(uint16_t vid, uint16_t pid, int timeout_ms, ftdi_dev_t **dev)
{
    assert(dev);

    *dev = calloc(1, sizeof(ftdi_dev_t));
    if (*dev == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Check list of already opened devices
    ftdi_dev_t *ftdi_dev;
    SLIST_FOREACH(ftdi_dev, &p_ftdi_sio_obj->ftdi_devices_list, list_entry) {
        const usb_device_desc_t *device_desc;
        ESP_ERROR_CHECK(usb_host_get_device_descriptor(ftdi_dev->dev_hdl, &device_desc));
        if ((vid == FTDI_HOST_ANY_VID || vid == device_desc->idVendor) &&
            (pid == FTDI_HOST_ANY_PID || pid == device_desc->idProduct)) {
            (*dev)->dev_hdl = ftdi_dev->dev_hdl;
            (*dev)->vid = device_desc->idVendor;
            (*dev)->pid = device_desc->idProduct;
            return ESP_OK;
        }
    }

    // Poll for new devices
    TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    TimeOut_t connection_timeout;
    vTaskSetTimeOutState(&connection_timeout);

    do {
        uint8_t dev_addr_list[10];
        int num_of_devices;
        ESP_ERROR_CHECK(usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &num_of_devices));

        for (int i = 0; i < num_of_devices; i++) {
            usb_device_handle_t current_device;
            // ESP-IDF v6.0: Use usb_host_device_open to get device handle
            esp_err_t err = usb_host_device_open(p_ftdi_sio_obj->ftdi_client_hdl, dev_addr_list[i], &current_device);
            if (err != ESP_OK) {
                continue;
            }

            const usb_device_desc_t *device_desc;
            ESP_ERROR_CHECK(usb_host_get_device_descriptor(current_device, &device_desc));

            if ((vid == FTDI_HOST_ANY_VID || vid == device_desc->idVendor) &&
                (pid == FTDI_HOST_ANY_PID || pid == device_desc->idProduct)) {
                // Found matching device
                (*dev)->dev_hdl = current_device;
                (*dev)->vid = device_desc->idVendor;
                (*dev)->pid = device_desc->idProduct;
                ESP_LOGD(TAG, "Found FTDI device: VID=0x%04x, PID=0x%04x", (*dev)->vid, (*dev)->pid);
                return ESP_OK;
            }

            // Not the device we're looking for, close it
            usb_host_device_close(p_ftdi_sio_obj->ftdi_client_hdl, current_device);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    } while (xTaskCheckForTimeOut(&connection_timeout, &timeout_ticks) == pdFALSE);

    free(*dev);
    *dev = NULL;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ftdi_sio_host_install(const ftdi_sio_host_driver_config_t *driver_config)
{
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(!p_ftdi_sio_obj, ESP_ERR_INVALID_STATE, TAG, "Driver already installed");

    if (driver_config == NULL) {
        driver_config = &ftdi_driver_config_default;
    }

    ftdi_sio_obj_t *ftdi_obj = calloc(1, sizeof(ftdi_sio_obj_t));
    ESP_RETURN_ON_FALSE(ftdi_obj, ESP_ERR_NO_MEM, TAG, "Unable to allocate memory");

    ftdi_obj->open_close_mutex = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(ftdi_obj->open_close_mutex, ESP_ERR_NO_MEM, err, TAG, "Unable to create mutex");

    ftdi_obj->event_group = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(ftdi_obj->event_group, ESP_ERR_NO_MEM, err, TAG, "Unable to create event group");

    SLIST_INIT(&ftdi_obj->ftdi_devices_list);
    ftdi_obj->new_dev_cb = driver_config->new_dev_cb;
    ftdi_obj->new_dev_cb_arg = driver_config->user_arg;

    // Register USB host client
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 3,
        .async = {
            .client_event_callback = usb_event_cb,
            .callback_arg = NULL
        }
    };
    ESP_GOTO_ON_ERROR(usb_host_client_register(&client_config, &ftdi_obj->ftdi_client_hdl),
                      err, TAG, "Failed to register USB host client");

    p_ftdi_sio_obj = ftdi_obj;

    // Create driver task
    TaskHandle_t task_created = NULL;
    BaseType_t result = xTaskCreatePinnedToCore(
                                  ftdi_client_task,
                                  "FTDI",
                                  driver_config->driver_task_stack_size,
                                  NULL,
                                  driver_config->driver_task_priority,
                                  &task_created,
                                  driver_config->xCoreID >= 0 ? driver_config->xCoreID : tskNO_AFFINITY);
    ESP_GOTO_ON_FALSE(result == pdPASS, ESP_ERR_NO_MEM, err_client, TAG, "Failed to create task");

    xTaskNotifyGive((TaskHandle_t)task_created);
    ESP_LOGI(TAG, "FTDI SIO driver installed");
    return ESP_OK;

err_client:
    usb_host_client_deregister(ftdi_obj->ftdi_client_hdl);
err:
    if (ftdi_obj->event_group) {
        vEventGroupDelete(ftdi_obj->event_group);
    }
    if (ftdi_obj->open_close_mutex) {
        vSemaphoreDelete(ftdi_obj->open_close_mutex);
    }
    free(ftdi_obj);
    return ret;
}

esp_err_t ftdi_sio_host_uninstall(void)
{
    ESP_RETURN_ON_FALSE(p_ftdi_sio_obj, ESP_ERR_INVALID_STATE, TAG, "Driver not installed");
    ESP_RETURN_ON_FALSE(SLIST_EMPTY(&p_ftdi_sio_obj->ftdi_devices_list), ESP_ERR_INVALID_STATE, TAG,
                        "All devices must be closed before uninstalling driver");

    xEventGroupSetBits(p_ftdi_sio_obj->event_group, FTDI_SIO_TEARDOWN);
    xEventGroupWaitBits(p_ftdi_sio_obj->event_group, FTDI_SIO_TEARDOWN_COMPLETE, pdFALSE, pdFALSE, pdMS_TO_TICKS(100));

    if (p_ftdi_sio_obj->event_group) {
        vEventGroupDelete(p_ftdi_sio_obj->event_group);
    }
    if (p_ftdi_sio_obj->open_close_mutex) {
        vSemaphoreDelete(p_ftdi_sio_obj->open_close_mutex);
    }

    free(p_ftdi_sio_obj);
    p_ftdi_sio_obj = NULL;
    ESP_LOGI(TAG, "FTDI SIO driver uninstalled");
    return ESP_OK;
}

esp_err_t ftdi_sio_host_open(uint16_t vid, uint16_t pid, uint8_t interface_idx,
                              const ftdi_sio_host_device_config_t *dev_config,
                              ftdi_sio_dev_hdl_t *ftdi_hdl_ret)
{
    esp_err_t ret;
    ftdi_dev_t *ftdi_dev = NULL;

    ESP_RETURN_ON_FALSE(p_ftdi_sio_obj, ESP_ERR_INVALID_STATE, TAG, "Driver not installed");
    ESP_RETURN_ON_FALSE(ftdi_hdl_ret, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");

    xSemaphoreTake(p_ftdi_sio_obj->open_close_mutex, portMAX_DELAY);

    // Use defaults if not provided
    ftdi_sio_host_device_config_t dev_cfg_default = FTDI_SIO_HOST_DEVICE_CONFIG_DEFAULT();
    if (dev_config == NULL) {
        dev_config = &dev_cfg_default;
    }

    // Find and open USB device
    ESP_GOTO_ON_ERROR(
        ftdi_find_and_open_usb_device(vid, pid, dev_config->connection_timeout_ms, &ftdi_dev),
        err, TAG, "Failed to find FTDI device");

    // Detect chip type
    ftdi_dev->chip_type = ftdi_parse_chip_type(ftdi_dev->pid);
    ESP_LOGI(TAG, "Detected chip type: %d", ftdi_dev->chip_type);

    // Get configuration descriptor
    const usb_config_desc_t *config_desc;
    ESP_GOTO_ON_ERROR(
        usb_host_get_active_config_descriptor(ftdi_dev->dev_hdl, &config_desc),
        err, TAG, "Failed to get config descriptor");

    // Parse interface descriptor
    ftdi_intf_info_t intf_info;
    ESP_GOTO_ON_ERROR(
        ftdi_parse_interface_descriptor(config_desc, interface_idx, &intf_info),
        err, TAG, "Failed to parse interface descriptor");

    ftdi_dev->data.intf_desc = intf_info.intf_desc;
    ftdi_dev->data.bulk_in_ep = intf_info.bulk_in_ep;
    ftdi_dev->data.bulk_out_ep = intf_info.bulk_out_ep;
    ftdi_dev->data.in_mps = intf_info.bulk_in_mps;

    // Set callbacks
    ftdi_dev->data_cb = dev_config->data_cb;
    ftdi_dev->event_cb = dev_config->event_cb;
    ftdi_dev->cb_arg = dev_config->user_arg;

    // Allocate transfers
    size_t in_buf_size = dev_config->in_buffer_size > 0 ? dev_config->in_buffer_size : FTDI_DEFAULT_IN_BUFFER_SIZE;
    size_t out_buf_size = dev_config->out_buffer_size > 0 ? dev_config->out_buffer_size : FTDI_DEFAULT_OUT_BUFFER_SIZE;
    ESP_GOTO_ON_ERROR(
        ftdi_transfers_allocate(ftdi_dev, in_buf_size, out_buf_size),
        err, TAG, "Failed to allocate transfers");

    // Start device
    ESP_GOTO_ON_ERROR(ftdi_start(ftdi_dev), err, TAG, "Failed to start device");

    // Initialize device with default settings
    ftdi_control_request_t req;

    // Reset device
    ftdi_protocol_build_reset(&req, FTDI_SIO_RESET_SIO);
    ftdi_sio_host_send_custom_request((ftdi_sio_dev_hdl_t)ftdi_dev,
                                      USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
                                      req.request, req.value, req.index, 0, NULL);

    // Set latency timer to 16ms (default)
    ftdi_protocol_build_set_latency_timer(&req, 16);
    ftdi_sio_host_send_custom_request((ftdi_sio_dev_hdl_t)ftdi_dev,
                                      USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
                                      req.request, req.value, req.index, 0, NULL);

    // Clear DTR/RTS
    ftdi_protocol_build_set_modem_ctrl(&req, false, false);
    ftdi_sio_host_send_custom_request((ftdi_sio_dev_hdl_t)ftdi_dev,
                                      USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_RECIP_DEVICE,
                                      req.request, req.value, req.index, 0, NULL);

    *ftdi_hdl_ret = (ftdi_sio_dev_hdl_t)ftdi_dev;
    xSemaphoreGive(p_ftdi_sio_obj->open_close_mutex);
    ESP_LOGI(TAG, "FTDI device opened successfully");
    return ESP_OK;

err:
    if (ftdi_dev) {
        ftdi_device_remove(ftdi_dev);
    }
    xSemaphoreGive(p_ftdi_sio_obj->open_close_mutex);
    return ret;
}

esp_err_t ftdi_sio_host_close(ftdi_sio_dev_hdl_t ftdi_hdl)
{
    ESP_RETURN_ON_FALSE(p_ftdi_sio_obj, ESP_ERR_INVALID_STATE, TAG, "Driver not installed");
    ESP_RETURN_ON_FALSE(ftdi_hdl, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    ftdi_dev_t *ftdi_dev = (ftdi_dev_t *)ftdi_hdl;

    xSemaphoreTake(p_ftdi_sio_obj->open_close_mutex, portMAX_DELAY);

    // Remove from list
    FTDI_SIO_ENTER_CRITICAL();
    SLIST_REMOVE(&p_ftdi_sio_obj->ftdi_devices_list, ftdi_dev, ftdi_dev_s, list_entry);
    FTDI_SIO_EXIT_CRITICAL();

    // Cancel and free transfers
    if (ftdi_dev->data.in_xfer) {
        ftdi_reset_transfer_endpoint(ftdi_dev->dev_hdl, ftdi_dev->data.in_xfer);
    }

    // Release interface
    ESP_ERROR_CHECK(usb_host_interface_release(p_ftdi_sio_obj->ftdi_client_hdl,
                    ftdi_dev->dev_hdl,
                    ftdi_dev->data.intf_desc->bInterfaceNumber));

    ftdi_device_remove(ftdi_dev);

    xSemaphoreGive(p_ftdi_sio_obj->open_close_mutex);
    ESP_LOGI(TAG, "FTDI device closed");
    return ESP_OK;
}

esp_err_t ftdi_sio_host_data_tx_blocking(ftdi_sio_dev_hdl_t ftdi_hdl,
        const uint8_t *data,
        size_t data_len,
        uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(ftdi_hdl && data && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");

    ftdi_dev_t *ftdi_dev = (ftdi_dev_t *)ftdi_hdl;

    xSemaphoreTake(ftdi_dev->data.out_mux, portMAX_DELAY);

    memcpy(ftdi_dev->data.out_xfer->data_buffer, data, data_len);
    ftdi_dev->data.out_xfer->num_bytes = data_len;
    ftdi_dev->data.out_xfer->timeout_ms = timeout_ms;

    esp_err_t err = usb_host_transfer_submit(ftdi_dev->data.out_xfer);

    xSemaphoreGive(ftdi_dev->data.out_mux);
    return err;
}

esp_err_t ftdi_sio_host_send_custom_request(ftdi_sio_dev_hdl_t ftdi_hdl,
        uint8_t bmRequestType,
        uint8_t bRequest,
        uint16_t wValue,
        uint16_t wIndex,
        uint16_t wLength,
        uint8_t *data)
{
    ESP_RETURN_ON_FALSE(ftdi_hdl, ESP_ERR_INVALID_ARG, TAG, "Invalid argument");

    ftdi_dev_t *ftdi_dev = (ftdi_dev_t *)ftdi_hdl;

    xSemaphoreTake(ftdi_dev->ctrl_mux, portMAX_DELAY);

    usb_setup_packet_t *setup = (usb_setup_packet_t *)ftdi_dev->ctrl_transfer->data_buffer;
    setup->bmRequestType = bmRequestType;
    setup->bRequest = bRequest;
    setup->wValue = wValue;
    setup->wIndex = wIndex;
    setup->wLength = wLength;

    if (data && wLength > 0) {
        memcpy(ftdi_dev->ctrl_transfer->data_buffer + sizeof(usb_setup_packet_t), data, wLength);
    }

    ftdi_dev->ctrl_transfer->num_bytes = sizeof(usb_setup_packet_t) + wLength;

    esp_err_t err = usb_host_transfer_submit_control(p_ftdi_sio_obj->ftdi_client_hdl, ftdi_dev->ctrl_transfer);

    xSemaphoreGive(ftdi_dev->ctrl_mux);
    return err;
}
