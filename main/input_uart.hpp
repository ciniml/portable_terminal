#pragma once

#include "byte_input.hpp"
#include "term_core/error.hpp"

namespace tab5 {

struct UartInputConfig {
    int port{1};         // UART_NUM_x
    int tx_gpio{-1};     // -1 = unused
    int rx_gpio{-1};
    int baud{115200};
};

// Install a UART driver and spawn a reader task that forwards every
// received byte to `sink` on its own task. Multiple sources can share
// a sink target; the sink is responsible for any cross-source
// synchronisation.
term::Result<void> start_uart_input(const UartInputConfig& cfg,
                                    ByteSink sink);

}  // namespace tab5
