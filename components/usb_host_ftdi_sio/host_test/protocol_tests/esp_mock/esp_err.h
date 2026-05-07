/*
 * SPDX-FileCopyrightText: 2026 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mock header for Linux testing of FTDI protocol layer
 *
 * This is a minimal mock of ESP-IDF's esp_err.h for cross-platform compilation.
 * The original esp_err.h is:
 * SPDX-FileCopyrightText: 2015-2021 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ESP-IDF error type
typedef int esp_err_t;

// Error codes used by FTDI protocol layer
#define ESP_OK              0       /*!< Success (no error) */
#define ESP_ERR_INVALID_ARG 0x102   /*!< Invalid argument */

#ifdef __cplusplus
}
#endif
