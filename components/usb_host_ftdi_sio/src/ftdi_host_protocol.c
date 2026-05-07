/*
 * SPDX-FileCopyrightText: 2024 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "ftdi_host_protocol.h"

esp_err_t ftdi_protocol_build_reset(ftdi_control_request_t *req_out, uint16_t reset_type)
{
    if (req_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    req_out->request = FTDI_SIO_RESET;
    req_out->value = reset_type;
    req_out->index = 0;  // Interface index (0 for single port)

    return ESP_OK;
}

esp_err_t ftdi_protocol_build_set_baudrate(ftdi_control_request_t *req_out,
                                            uint32_t baudrate,
                                            ftdi_chip_type_t chip_type)
{
    if (req_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t value, index;
    esp_err_t ret = ftdi_calculate_baudrate_divisor(baudrate, chip_type, &value, &index);
    if (ret != ESP_OK) {
        return ret;
    }

    req_out->request = FTDI_SIO_SET_BAUDRATE;
    req_out->value = value;
    req_out->index = index;

    return ESP_OK;
}

esp_err_t ftdi_protocol_build_set_line_property(ftdi_control_request_t *req_out,
                                                 ftdi_data_bits_t bits,
                                                 ftdi_stop_bits_t stype,
                                                 ftdi_parity_t parity)
{
    if (req_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Validate parameters
    if (bits != FTDI_DATA_BITS_7 && bits != FTDI_DATA_BITS_8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (stype > FTDI_STOP_BITS_2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (parity > FTDI_PARITY_SPACE) {
        return ESP_ERR_INVALID_ARG;
    }

    // Build value field: bits[7:0] = data bits, bits[10:8] = parity, bits[13:11] = stop bits
    uint16_t value = bits | (parity << 8) | (stype << 11);

    req_out->request = FTDI_SIO_SET_DATA;
    req_out->value = value;
    req_out->index = 0;

    return ESP_OK;
}

esp_err_t ftdi_protocol_build_set_modem_ctrl(ftdi_control_request_t *req_out,
                                              bool dtr,
                                              bool rts)
{
    if (req_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // FTDI modem control encoding:
    // High byte: control mask (1 = control this signal)
    // Low byte: signal value (1 = high, 0 = low)
    // DTR is bit 0, RTS is bit 1
    uint16_t value = 0;

    // Set DTR
    value |= FTDI_SIO_SET_DTR_MASK << 8;  // Enable DTR control
    if (dtr) {
        value |= FTDI_SIO_SET_DTR_MASK;  // Set DTR high
    }

    // Set RTS
    value |= FTDI_SIO_SET_RTS_MASK << 8;  // Enable RTS control
    if (rts) {
        value |= FTDI_SIO_SET_RTS_MASK;  // Set RTS high
    }

    req_out->request = FTDI_SIO_SET_MODEM_CTRL;
    req_out->value = value;
    req_out->index = 0;

    return ESP_OK;
}

esp_err_t ftdi_protocol_build_set_latency_timer(ftdi_control_request_t *req_out,
                                                 uint8_t latency_ms)
{
    if (req_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (latency_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    req_out->request = FTDI_SIO_SET_LATENCY_TIMER;
    req_out->value = latency_ms;
    req_out->index = 0;

    return ESP_OK;
}

esp_err_t ftdi_protocol_parse_modem_status(const uint8_t data[2],
                                            ftdi_modem_status_t *status_out)
{
    if (data == NULL || status_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Clear status structure
    memset(status_out, 0, sizeof(ftdi_modem_status_t));

    // Byte 0: B0
    status_out->data_pending = (data[0] >> 0) & 0x01;
    status_out->overrun = (data[0] >> 1) & 0x01;
    status_out->parity_error = (data[0] >> 2) & 0x01;
    status_out->framing_error = (data[0] >> 3) & 0x01;
    status_out->break_received = (data[0] >> 4) & 0x01;
    status_out->tx_holding_empty = (data[0] >> 5) & 0x01;
    status_out->tx_empty = (data[0] >> 6) & 0x01;

    // Byte 1: B1
    status_out->cts = (data[1] >> 4) & 0x01;
    status_out->dsr = (data[1] >> 5) & 0x01;
    status_out->ri = (data[1] >> 6) & 0x01;
    status_out->rlsd = (data[1] >> 7) & 0x01;

    return ESP_OK;
}

esp_err_t ftdi_calculate_baudrate_divisor(uint32_t baudrate,
                                          ftdi_chip_type_t chip_type,
                                          uint16_t *value_out,
                                          uint16_t *index_out)
{
    if (value_out == NULL || index_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Base clock frequency for different chip types
    uint32_t base_clock;
    switch (chip_type) {
    case FTDI_CHIP_TYPE_232R:
    case FTDI_CHIP_TYPE_230X:
        base_clock = 3000000;  // 3 MHz
        break;
    case FTDI_CHIP_TYPE_232H:
        base_clock = 12000000; // 12 MHz (can also be 30 MHz in high speed mode)
        break;
    case FTDI_CHIP_TYPE_2232D:
    case FTDI_CHIP_TYPE_4232H:
        base_clock = 6000000;  // 6 MHz
        break;
    default:
        base_clock = 3000000;  // Default to 3 MHz
        break;
    }

    // Validate baud rate range
    if (baudrate < 300 || baudrate > base_clock / 2) {
        return ESP_ERR_INVALID_ARG;
    }

    // Calculate divisor with fractional support
    // The divisor is encoded as: (integer_part << 3) | fractional_index
    // Fractional values: 0 (.000), 1 (.125), 2 (.25), 3 (.375), 4 (.5), 5 (.625), 6 (.75), 7 (.875)

    // Multiply by 8 to preserve fractional part
    uint32_t divisor = (base_clock * 8) / baudrate;

    // Special case: divisor of 1 means base_clock / 1 (not base_clock / 8)
    if (divisor == 8) {
        divisor = 1;
    } else if (divisor < 8) {
        divisor = 0;  // Maximum baud rate
    } else if (divisor > 0x1FFFF8) {
        // Maximum divisor value (for very low baud rates)
        divisor = 0x1FFFF8;
    }

    uint32_t integral_part = divisor >> 3;
    uint32_t fractional_part = divisor & 0x07;  
    uint32_t divisor_encoded_value = integral_part;

    switch(fractional_part) {
        case 0b000: divisor_encoded_value |= (0b000 << 14); break; // .000
        case 0b001: divisor_encoded_value |= (0b011 << 14); break; // .125
        case 0b010: divisor_encoded_value |= (0b010 << 14); break; // .250
        case 0b011: divisor_encoded_value |= (0b100 << 14); break; // .375
        case 0b100: divisor_encoded_value |= (0b001 << 14); break; // .500
        case 0b101: divisor_encoded_value |= (0b101 << 14); break; // .625
        case 0b110: divisor_encoded_value |= (0b110 << 14); break; // .750
        case 0b111: divisor_encoded_value |= (0b111 << 14); break; // .875
        default: break;
    }

    // Extract value and index
    *value_out = divisor_encoded_value & 0xFFFF;
    *index_out = (divisor_encoded_value >> 16) & 0xFFFF;

    // Special handling for sub-integer divisors
    if (divisor == 0) {
        *value_out = 0;
        *index_out = 0;
    } else if (divisor == 1) {
        *value_out = 1;
        *index_out = 0;
    }

    return ESP_OK;
}
