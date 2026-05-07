/*
 * SPDX-FileCopyrightText: 2024 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"

#ifdef CONFIG_USB_HOST_ENABLE_FTDI_SIO_DRIVER

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FTDI device handle
 */
typedef struct ftdi_dev_s *ftdi_sio_dev_hdl_t;

/**
 * @brief FTDI Vendor ID
 */
#define FTDI_VID (0x0403)

/**
 * @brief Common FTDI Product IDs
 */
#define FTDI_PID_FT232R   (0x6001)  // FT232R and FT245R
#define FTDI_PID_FT232H   (0x6014)  // FT232H
#define FTDI_PID_FT2232D  (0x6010)  // FT2232D/H/C
#define FTDI_PID_FT4232H  (0x6011)  // FT4232H
#define FTDI_PID_FT230X   (0x6015)  // FT230X

/**
 * @brief Wildcard VID/PID for matching any device
 */
#define FTDI_HOST_ANY_VID (0)
#define FTDI_HOST_ANY_PID (0)

/**
 * @brief FTDI chip types
 */
typedef enum {
    FTDI_CHIP_TYPE_UNKNOWN = 0,
    FTDI_CHIP_TYPE_232R,    // FT232R
    FTDI_CHIP_TYPE_232H,    // FT232H (high speed)
    FTDI_CHIP_TYPE_2232D,   // FT2232D (dual port)
    FTDI_CHIP_TYPE_4232H,   // FT4232H (quad port)
    FTDI_CHIP_TYPE_230X,    // FT230X
} ftdi_chip_type_t;

/**
 * @brief Data bits configuration
 */
typedef enum {
    FTDI_DATA_BITS_7 = 7,
    FTDI_DATA_BITS_8 = 8,
} ftdi_data_bits_t;

/**
 * @brief Stop bits configuration
 */
typedef enum {
    FTDI_STOP_BITS_1 = 0,   // 1 stop bit
    FTDI_STOP_BITS_15 = 1,  // 1.5 stop bits
    FTDI_STOP_BITS_2 = 2,   // 2 stop bits
} ftdi_stop_bits_t;

/**
 * @brief Parity configuration
 */
typedef enum {
    FTDI_PARITY_NONE = 0,
    FTDI_PARITY_ODD = 1,
    FTDI_PARITY_EVEN = 2,
    FTDI_PARITY_MARK = 3,
    FTDI_PARITY_SPACE = 4,
} ftdi_parity_t;

/**
 * @brief Modem status structure
 *
 * This status is included in the first 2 bytes of every Bulk IN packet
 * from FTDI devices.
 */
typedef struct {
    // Byte 0 (B0)
    uint8_t data_pending : 1;    // More data in device buffer
    uint8_t overrun : 1;         // Data overrun error
    uint8_t parity_error : 1;    // Parity error
    uint8_t framing_error : 1;   // Framing error
    uint8_t break_received : 1;  // Break signal received
    uint8_t tx_holding_empty : 1; // Transmit holding register empty
    uint8_t tx_empty : 1;        // Transmit shift register empty
    uint8_t reserved_b0 : 1;     // Reserved

    // Byte 1 (B1)
    uint8_t cts : 1;             // Clear To Send
    uint8_t dsr : 1;             // Data Set Ready
    uint8_t ri : 1;              // Ring Indicator
    uint8_t rlsd : 1;            // Carrier Detect (RLSD/DCD)
    uint8_t reserved_b1 : 4;     // Reserved
} ftdi_modem_status_t;

/**
 * @brief Device events
 */
typedef enum {
    FTDI_SIO_HOST_ERROR,                // Error occurred
    FTDI_SIO_HOST_MODEM_STATUS,         // Modem status changed
    FTDI_SIO_HOST_DEVICE_DISCONNECTED,  // Device disconnected
} ftdi_sio_host_dev_event_t;

/**
 * @brief Data receive callback
 *
 * @param[in] data Pointer to received data (modem status bytes already stripped)
 * @param[in] data_len Length of received data
 * @param[in] user_arg User argument provided during device open
 */
typedef void (*ftdi_sio_data_callback_t)(const uint8_t *data, size_t data_len, void *user_arg);

/**
 * @brief Device event callback
 *
 * @param[in] event Event type
 * @param[in] user_arg User argument provided during device open
 */
typedef void (*ftdi_sio_host_dev_callback_t)(ftdi_sio_host_dev_event_t event, void *user_arg);

/**
 * @brief New device callback
 *
 * Called when a new FTDI device is connected to the bus.
 *
 * @param[in] vid USB Vendor ID
 * @param[in] pid USB Product ID
 * @param[in] user_arg User argument provided during driver installation
 */
typedef void (*ftdi_sio_new_dev_callback_t)(uint16_t vid, uint16_t pid, void *user_arg);

/**
 * @brief Driver configuration
 */
typedef struct {
    size_t driver_task_stack_size;     // Stack size for driver task (0 = default)
    unsigned driver_task_priority;     // Priority for driver task (0 = default)
    int xCoreID;                       // Core ID for driver task (-1 = no affinity)
    ftdi_sio_new_dev_callback_t new_dev_cb; // New device callback (can be NULL)
    void *user_arg;                    // User argument for new device callback
} ftdi_sio_host_driver_config_t;

/**
 * @brief Device configuration
 */
typedef struct {
    uint32_t connection_timeout_ms;    // Timeout for device connection (0 = default)
    size_t out_buffer_size;            // Bulk OUT buffer size (0 = default)
    size_t in_buffer_size;             // Bulk IN buffer size (0 = default)
    ftdi_sio_host_dev_callback_t event_cb; // Event callback (can be NULL)
    ftdi_sio_data_callback_t data_cb;  // Data receive callback (can be NULL)
    void *user_arg;                    // User argument for callbacks
} ftdi_sio_host_device_config_t;

/**
 * @brief Default driver configuration
 */
#define FTDI_SIO_HOST_DRIVER_CONFIG_DEFAULT() { \
    .driver_task_stack_size = 4096, \
    .driver_task_priority = 5, \
    .xCoreID = -1, \
    .new_dev_cb = NULL, \
    .user_arg = NULL, \
}

/**
 * @brief Default device configuration
 */
#define FTDI_SIO_HOST_DEVICE_CONFIG_DEFAULT() { \
    .connection_timeout_ms = 5000, \
    .out_buffer_size = 512, \
    .in_buffer_size = 512, \
    .event_cb = NULL, \
    .data_cb = NULL, \
    .user_arg = NULL, \
}

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_USB_HOST_ENABLE_FTDI_SIO_DRIVER */
