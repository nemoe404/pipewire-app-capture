#ifndef PWCAP_CLI_STATUS_HPP
#define PWCAP_CLI_STATUS_HPP

// Pure classification of diagnostic text and the process exit codes. No I/O, no PipeWire.

#include "pwcap/capture.hpp"

#include <optional>
#include <string_view>

namespace pwcap::cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitUsage = 2;

// Returns the Error when the message starts with that Error's name followed by a colon, for example
// a lost-connection message from the stop path. Returns nullopt for every other message.
[[nodiscard]] std::optional<Error> error_prefix(std::string_view message) noexcept;

// Stopped and NoPipeWire end a capture. The other Errors are reported but the capture goes on.
[[nodiscard]] constexpr bool is_fatal(Error error) noexcept {
    return error == Error::Stopped || error == Error::NoPipeWire;
}

} // namespace pwcap::cli

#endif // PWCAP_CLI_STATUS_HPP
