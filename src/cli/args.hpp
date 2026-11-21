#ifndef PWCAP_CLI_ARGS_HPP
#define PWCAP_CLI_ARGS_HPP

// Pure command-line parser. No I/O and no PipeWire: main.cpp is the only CLI file that touches the
// OS.

#include "pwcap/capture.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <variant>

namespace pwcap::cli {

enum class UsageError : std::uint8_t {
    MissingCommand,
    UnknownCommand,
    UnknownOption,
    MissingValue,
    EmptyValue,
    DuplicateOption,
    NoInclude,
};

// index is the position of the offending token in args, or args.size() when a token is missing.
struct UsageFailure {
    UsageError error;
    std::size_t index;
};

[[nodiscard]] std::string_view describe(UsageError error) noexcept;

struct HelpCommand {};

struct ListCommand {
    bool json{false};
};

struct CaptureCommand {
    pwcap::Options options{};
    bool quiet{false};
};

using Command = std::variant<HelpCommand, ListCommand, CaptureCommand>;

// args excludes argv[0]. Accepts `capture --app NAME`, `capture --app=NAME`, `list [--json]`, and
// --help or -h alone or after a command. There is no default target: `capture` without --app is
// NoInclude.
[[nodiscard]] std::expected<Command, UsageFailure>
parse_command_line(std::span<const std::string_view> args);

} // namespace pwcap::cli

#endif // PWCAP_CLI_ARGS_HPP
