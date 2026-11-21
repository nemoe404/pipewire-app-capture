#include "cli/status.hpp"

#include "pwcap/capture.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

namespace pwcap::cli {

namespace {

// Every Error enumerator. The names come from to_string, never from string literals here.
constexpr std::array<Error, 6> kAllErrors{Error::NoPipeWire,       Error::InvalidMatch,
                                          Error::NoMatchingSource, Error::LinkFailed,
                                          Error::Stopped,          Error::InvalidOptions};

} // namespace

std::optional<Error> error_prefix(std::string_view message) noexcept {
    const auto* const hit = std::ranges::find_if(kAllErrors, [message](Error error) {
        const std::string_view name = to_string(error);
        return message.size() > name.size() && message.starts_with(name) &&
               message[name.size()] == ':';
    });
    if (hit == kAllErrors.end()) {
        return std::nullopt;
    }
    return *hit;
}

} // namespace pwcap::cli
