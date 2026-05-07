/*
 * SPDX-FileCopyrightText: 2026 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mock header for Linux testing of FTDI protocol layer
 *
 * This is a minimal mock of ESP-IDF's sdkconfig.h for cross-platform compilation.
 */

#pragma once

// For Linux testing, always enable FTDI driver
#define CONFIG_USB_SERIAL_DRIVER_FTDI 1
