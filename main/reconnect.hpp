// Auto-reconnect supervisor. Watches an IConnection's is_connected()
// state; when it drops to false, waits with exponential backoff and
// calls start() again. After a successful reconnect, replays the
// current pty size via resize() so the remote starts at the right
// geometry. Survives transient Wi-Fi outages — dial() inside start()
// fails fast while Wi-Fi is down and the supervisor just keeps
// retrying.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "byte_input.hpp"
#include "connection.hpp"

namespace tab5 {

struct ReconnectConfig {
    uint32_t initial_backoff_ms = 1000;
    uint32_t max_backoff_ms     = 30000;
    uint32_t backoff_factor     = 2;
    uint32_t poll_period_ms     = 500;
    uint32_t task_stack_bytes   = 4096;
};

using GridSizeGetter = std::function<std::pair<uint16_t, uint16_t>()>;

// Spawn the supervisor. `conn` must outlive the program (it does, in
// app_main). `rx_sink` is replayed to start() on every reconnect;
// `get_size` returns the current terminal grid (cols, rows) so the
// remote can be resize()d immediately after a successful reconnect.
void start_reconnect_supervisor(IConnection* conn,
                                ByteSink rx_sink,
                                GridSizeGetter get_size,
                                const ReconnectConfig& cfg = {});

}  // namespace tab5
