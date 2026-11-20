#pragma once

// The one guarded path for narrowing an untrusted integer, for example a size_t that is about to
// cross into a C API taking a 32-bit integer.

#include <concepts>
#include <optional>
#include <utility>

namespace pwcap::core {

// The value converted to To, or std::nullopt when it does not fit.
template <std::integral To, std::integral From>
[[nodiscard]] constexpr std::optional<To> checked_narrow(From value) noexcept {
    if (std::in_range<To>(value)) {
        return static_cast<To>(value);
    }
    return std::nullopt;
}

}  // namespace pwcap::core
