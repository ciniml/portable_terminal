#pragma once

#include <expected>

namespace term {

enum class Error {
    OutOfRange,
    NotInitialized,
    BackendError,
    ParseOverflow,
};

template <class T>
using Result = std::expected<T, Error>;

}  // namespace term
