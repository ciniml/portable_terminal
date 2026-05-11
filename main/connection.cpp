#include "connection.hpp"

#include <atomic>

namespace tab5 {

namespace {
std::atomic<IConnection*> g_active{nullptr};
}

IConnection* active_connection() { return g_active.load(); }

void set_active_connection(IConnection* c) { g_active.store(c); }

}  // namespace tab5
