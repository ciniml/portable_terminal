/*
 * SPDX-FileCopyrightText: 2024 Kenta Ida
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

// Include FTDI protocol header (esp_err.h is mocked in esp_mock/)
extern "C" {
#include "ftdi_host_protocol.h"
}

TEST_CASE("FTDI Protocol - Build Reset Request", "[ftdi_protocol]")
{
    ftdi_control_request_t req;

    SECTION("Build SIO reset") {
        REQUIRE(ftdi_protocol_build_reset(&req, FTDI_SIO_RESET_SIO) == ESP_OK);
        REQUIRE(req.request == FTDI_SIO_RESET);
        REQUIRE(req.value == FTDI_SIO_RESET_SIO);
        REQUIRE(req.index == 0);
    }

    SECTION("Build purge RX") {
        REQUIRE(ftdi_protocol_build_reset(&req, FTDI_SIO_RESET_PURGE_RX) == ESP_OK);
        REQUIRE(req.request == FTDI_SIO_RESET);
        REQUIRE(req.value == FTDI_SIO_RESET_PURGE_RX);
    }

    SECTION("Build purge TX") {
        REQUIRE(ftdi_protocol_build_reset(&req, FTDI_SIO_RESET_PURGE_TX) == ESP_OK);
        REQUIRE(req.request == FTDI_SIO_RESET);
        REQUIRE(req.value == FTDI_SIO_RESET_PURGE_TX);
    }

    SECTION("NULL pointer") {
        REQUIRE(ftdi_protocol_build_reset(nullptr, FTDI_SIO_RESET_SIO) == ESP_ERR_INVALID_ARG);
    }
}

TEST_CASE("FTDI Protocol - Build Set Modem Control", "[ftdi_protocol]")
{
    ftdi_control_request_t req;

    SECTION("Set DTR=1, RTS=1") {
        REQUIRE(ftdi_protocol_build_set_modem_ctrl(&req, true, true) == ESP_OK);
        REQUIRE(req.request == FTDI_SIO_SET_MODEM_CTRL);
        // DTR: mask=0x0100, value=0x0001 -> 0x0101
        // RTS: mask=0x0200, value=0x0002 -> 0x0202
        REQUIRE(req.value == 0x0303);
    }

    SECTION("Set DTR=0, RTS=0") {
        REQUIRE(ftdi_protocol_build_set_modem_ctrl(&req, false, false) == ESP_OK);
        REQUIRE(req.request == FTDI_SIO_SET_MODEM_CTRL);
        // DTR: mask=0x0100, value=0x0000 -> 0x0100
        // RTS: mask=0x0200, value=0x0000 -> 0x0200
        REQUIRE(req.value == 0x0300);
    }

    SECTION("Set DTR=1, RTS=0") {
        REQUIRE(ftdi_protocol_build_set_modem_ctrl(&req, true, false) == ESP_OK);
        // DTR: mask=0x0100, value=0x0001 -> 0x0101
        // RTS: mask=0x0200, value=0x0000 -> 0x0200
        REQUIRE(req.value == 0x0301);
    }

    SECTION("Set DTR=0, RTS=1") {
        REQUIRE(ftdi_protocol_build_set_modem_ctrl(&req, false, true) == ESP_OK);
        // DTR: mask=0x0100, value=0x0000 -> 0x0100
        // RTS: mask=0x0200, value=0x0002 -> 0x0202
        REQUIRE(req.value == 0x0302);
    }
}

TEST_CASE("FTDI Protocol - Build Set Line Property", "[ftdi_protocol]")
{
    ftdi_control_request_t req;

    SECTION("8N1 (8 data bits, no parity, 1 stop bit)") {
        REQUIRE(ftdi_protocol_build_set_line_property(&req,
                FTDI_DATA_BITS_8,
                FTDI_STOP_BITS_1,
                FTDI_PARITY_NONE) == ESP_OK);
        REQUIRE(req.request == FTDI_SIO_SET_DATA);
        // Bits[7:0]=8, Bits[10:8]=0 (none), Bits[13:11]=0 (1 stop)
        REQUIRE(req.value == 0x0008);
    }

    SECTION("7E1 (7 data bits, even parity, 1 stop bit)") {
        REQUIRE(ftdi_protocol_build_set_line_property(&req,
                FTDI_DATA_BITS_7,
                FTDI_STOP_BITS_1,
                FTDI_PARITY_EVEN) == ESP_OK);
        // Bits[7:0]=7, Bits[10:8]=2 (even), Bits[13:11]=0 (1 stop)
        REQUIRE(req.value == 0x0207);
    }

    SECTION("8N2 (8 data bits, no parity, 2 stop bits)") {
        REQUIRE(ftdi_protocol_build_set_line_property(&req,
                FTDI_DATA_BITS_8,
                FTDI_STOP_BITS_2,
                FTDI_PARITY_NONE) == ESP_OK);
        // Bits[7:0]=8, Bits[10:8]=0 (none), Bits[13:11]=2 (2 stop)
        REQUIRE(req.value == 0x1008);
    }
}

TEST_CASE("FTDI Protocol - Build Set Latency Timer", "[ftdi_protocol]")
{
    ftdi_control_request_t req;

    SECTION("Set latency to 16ms") {
        REQUIRE(ftdi_protocol_build_set_latency_timer(&req, 16) == ESP_OK);
        REQUIRE(req.request == FTDI_SIO_SET_LATENCY_TIMER);
        REQUIRE(req.value == 16);
    }

    SECTION("Invalid latency (0)") {
        REQUIRE(ftdi_protocol_build_set_latency_timer(&req, 0) == ESP_ERR_INVALID_ARG);
    }
}

TEST_CASE("FTDI Protocol - Baud Rate Calculation (FT232R)", "[ftdi_protocol]")
{
    uint16_t value, index;

    SECTION("9600 baud") {
        REQUIRE(ftdi_calculate_baudrate_divisor(9600, FTDI_CHIP_TYPE_232R, &value, &index) == ESP_OK);
        // 3000000 / 9600 = 312.5 -> divisor_raw = 2500 (312.5 * 8)
        // integral = 312, fractional = 4 (.500) -> encoded as 0b001 << 14
        // divisor_encoded = 312 | (1 << 14) = 16696
        REQUIRE(value == 16696);
        REQUIRE(index == 0);
    }

    SECTION("115200 baud") {
        REQUIRE(ftdi_calculate_baudrate_divisor(115200, FTDI_CHIP_TYPE_232R, &value, &index) == ESP_OK);
        // 3000000 / 115200 = 26.04... -> divisor_raw = 208
        // integral = 26, fractional = 0 (.000)
        // divisor_encoded = 26 | (0 << 14) = 26
        REQUIRE(value == 26);
        REQUIRE(index == 0);
    }

    SECTION("19200 baud") {
        REQUIRE(ftdi_calculate_baudrate_divisor(19200, FTDI_CHIP_TYPE_232R, &value, &index) == ESP_OK);
        // 3000000 / 19200 = 156.25 -> divisor_raw = 1250
        // integral = 156, fractional = 2 (.250) -> encoded as 0b010 << 14
        // divisor_encoded = 156 | (2 << 14) = 32924
        REQUIRE(value == 32924);
        REQUIRE(index == 0);
    }

    SECTION("300 baud (minimum)") {
        REQUIRE(ftdi_calculate_baudrate_divisor(300, FTDI_CHIP_TYPE_232R, &value, &index) == ESP_OK);
        // Should calculate large divisor
        REQUIRE(value > 0);
    }

    SECTION("921600 baud (high speed)") {
        REQUIRE(ftdi_calculate_baudrate_divisor(921600, FTDI_CHIP_TYPE_232R, &value, &index) == ESP_OK);
        // 3000000 / 921600 = 3.255... -> divisor_raw = 26
        // integral = 3, fractional = 2 (.250) -> encoded as 0b010 << 14
        // divisor_encoded = 3 | (2 << 14) = 32771
        REQUIRE(value == 32771);
        REQUIRE(index == 0);
    }

    SECTION("Invalid baud rate (too low)") {
        REQUIRE(ftdi_calculate_baudrate_divisor(100, FTDI_CHIP_TYPE_232R, &value, &index) == ESP_ERR_INVALID_ARG);
    }

    SECTION("Invalid baud rate (too high)") {
        REQUIRE(ftdi_calculate_baudrate_divisor(10000000, FTDI_CHIP_TYPE_232R, &value, &index) == ESP_ERR_INVALID_ARG);
    }

    SECTION("NULL pointers") {
        REQUIRE(ftdi_calculate_baudrate_divisor(9600, FTDI_CHIP_TYPE_232R, nullptr, &index) == ESP_ERR_INVALID_ARG);
        REQUIRE(ftdi_calculate_baudrate_divisor(9600, FTDI_CHIP_TYPE_232R, &value, nullptr) == ESP_ERR_INVALID_ARG);
    }
}

TEST_CASE("FTDI Protocol - Parse Modem Status", "[ftdi_protocol]")
{
    ftdi_modem_status_t status;

    SECTION("All bits clear") {
        uint8_t data[2] = {0x00, 0x00};
        REQUIRE(ftdi_protocol_parse_modem_status(data, &status) == ESP_OK);
        REQUIRE(status.cts == 0);
        REQUIRE(status.dsr == 0);
        REQUIRE(status.ri == 0);
        REQUIRE(status.rlsd == 0);
        REQUIRE(status.overrun == 0);
        REQUIRE(status.parity_error == 0);
        REQUIRE(status.framing_error == 0);
    }

    SECTION("CTS set") {
        uint8_t data[2] = {0x00, 0x10};  // Bit 4 of byte 1
        REQUIRE(ftdi_protocol_parse_modem_status(data, &status) == ESP_OK);
        REQUIRE(status.cts == 1);
        REQUIRE(status.dsr == 0);
    }

    SECTION("DSR set") {
        uint8_t data[2] = {0x00, 0x20};  // Bit 5 of byte 1
        REQUIRE(ftdi_protocol_parse_modem_status(data, &status) == ESP_OK);
        REQUIRE(status.dsr == 1);
        REQUIRE(status.cts == 0);
    }

    SECTION("RI set") {
        uint8_t data[2] = {0x00, 0x40};  // Bit 6 of byte 1
        REQUIRE(ftdi_protocol_parse_modem_status(data, &status) == ESP_OK);
        REQUIRE(status.ri == 1);
    }

    SECTION("RLSD (CD) set") {
        uint8_t data[2] = {0x00, 0x80};  // Bit 7 of byte 1
        REQUIRE(ftdi_protocol_parse_modem_status(data, &status) == ESP_OK);
        REQUIRE(status.rlsd == 1);
    }

    SECTION("Overrun error") {
        uint8_t data[2] = {0x02, 0x00};  // Bit 1 of byte 0
        REQUIRE(ftdi_protocol_parse_modem_status(data, &status) == ESP_OK);
        REQUIRE(status.overrun == 1);
    }

    SECTION("Parity error") {
        uint8_t data[2] = {0x04, 0x00};  // Bit 2 of byte 0
        REQUIRE(ftdi_protocol_parse_modem_status(data, &status) == ESP_OK);
        REQUIRE(status.parity_error == 1);
    }

    SECTION("Framing error") {
        uint8_t data[2] = {0x08, 0x00};  // Bit 3 of byte 0
        REQUIRE(ftdi_protocol_parse_modem_status(data, &status) == ESP_OK);
        REQUIRE(status.framing_error == 1);
    }

    SECTION("Multiple status bits set") {
        uint8_t data[2] = {0x00, 0xF0};  // All modem status bits
        REQUIRE(ftdi_protocol_parse_modem_status(data, &status) == ESP_OK);
        REQUIRE(status.cts == 1);
        REQUIRE(status.dsr == 1);
        REQUIRE(status.ri == 1);
        REQUIRE(status.rlsd == 1);
    }

    SECTION("NULL pointers") {
        uint8_t data[2] = {0x00, 0x00};
        REQUIRE(ftdi_protocol_parse_modem_status(nullptr, &status) == ESP_ERR_INVALID_ARG);
        REQUIRE(ftdi_protocol_parse_modem_status(data, nullptr) == ESP_ERR_INVALID_ARG);
    }
}

TEST_CASE("FTDI Protocol - Build Set Baudrate", "[ftdi_protocol]")
{
    ftdi_control_request_t req;

    SECTION("Valid baudrate") {
        REQUIRE(ftdi_protocol_build_set_baudrate(&req, 115200, FTDI_CHIP_TYPE_232R) == ESP_OK);
        REQUIRE(req.request == FTDI_SIO_SET_BAUDRATE);
        // Value should be calculated divisor
        REQUIRE(req.value > 0);
    }

    SECTION("Invalid baudrate") {
        REQUIRE(ftdi_protocol_build_set_baudrate(&req, 100, FTDI_CHIP_TYPE_232R) == ESP_ERR_INVALID_ARG);
    }

    SECTION("NULL pointer") {
        REQUIRE(ftdi_protocol_build_set_baudrate(nullptr, 115200, FTDI_CHIP_TYPE_232R) == ESP_ERR_INVALID_ARG);
    }
}
