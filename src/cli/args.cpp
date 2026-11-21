#include "cli/args.hpp"

#include "pwcap/capture.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace pwcap::cli {

namespace {

constexpr std::string_view kCapture = "capture";
constexpr std::string_view kList = "list";
constexpr std::string_view kJsonFlag = "--json";
constexpr std::string_view kAppFlag = "--app";
constexpr std::string_view kAppPrefix = "--app=";

[[nodiscard]] bool is_help(std::string_view token) noexcept {
    return token == "--help" || token == "-h";
}

[[nodiscard]] std::unexpected<UsageFailure> fail(UsageError error, std::size_t index) {
    return std::unexpected{UsageFailure{.error = error, .index = index}};
}

// `list` takes only --json, once, and --help. Anything else is an unknown option.
[[nodiscard]] std::expected<Command, UsageFailure>
parse_list(std::span<const std::string_view> args) {
    ListCommand command;
    bool have_json = false;
    std::size_t index = 1;
    // A raw loop: each token is one option and every error reports the index it was found at, the
    // same walk as the capture branch. No range algorithm stops at a token index.
    while (index < args.size()) {
        const std::string_view token = args[index];
        if (is_help(token)) {
            return Command{HelpCommand{}};
        }
        if (token != kJsonFlag) {
            return fail(UsageError::UnknownOption, index);
        }
        if (have_json) {
            return fail(UsageError::DuplicateOption, index);
        }
        command.json = true;
        have_json = true;
        ++index;
    }
    return Command{command};
}

} // namespace

std::string_view describe(UsageError error) noexcept {
    switch (error) {
    case UsageError::MissingCommand:
        return "missing command; expected 'capture', 'list' or --help";
    case UsageError::UnknownCommand:
        return "unknown command; expected 'capture', 'list' or --help";
    case UsageError::UnknownOption:
        return "unknown option";
    case UsageError::MissingValue:
        return "--app needs a value";
    case UsageError::EmptyValue:
        return "the value must not be empty";
    case UsageError::DuplicateOption:
        return "--app may be given only once";
    case UsageError::NoInclude:
        return "capture needs --app NAME";
    }
    return "usage error";
}

std::expected<Command, UsageFailure> parse_command_line(std::span<const std::string_view> args) {
    if (args.empty()) {
        return fail(UsageError::MissingCommand, args.size());
    }
    if (is_help(args.front())) {
        return Command{HelpCommand{}};
    }
    if (args.front() == kList) {
        return parse_list(args);
    }
    if (args.front() != kCapture) {
        return fail(UsageError::UnknownCommand, 0);
    }

    CaptureCommand command;
    bool have_app = false;
    std::size_t index = 1;
    // A raw loop: --app consumes a variable number of tokens (its value may be the next token), and
    // every error reports the index it was found at. No range algorithm expresses that walk.
    while (index < args.size()) {
        const std::string_view token = args[index];
        if (is_help(token)) {
            return Command{HelpCommand{}};
        }
        if (token != kAppFlag && !token.starts_with(kAppPrefix)) {
            return fail(UsageError::UnknownOption, index);
        }
        if (have_app) {
            return fail(UsageError::DuplicateOption, index);
        }

        std::string_view value;
        std::size_t value_index = index;
        if (token == kAppFlag) {
            if (index + 1 >= args.size()) {
                return fail(UsageError::MissingValue, args.size());
            }
            value_index = index + 1;
            value = args[value_index];
            index += 2;
        } else {
            value = token.substr(kAppPrefix.size());
            index += 1;
        }
        if (value.empty()) {
            return fail(UsageError::EmptyValue, value_index);
        }
        command.options.include.app_name = std::string{value};
        have_app = true;
    }

    if (!have_app) {
        return fail(UsageError::NoInclude, args.size());
    }
    return Command{std::move(command)};
}

} // namespace pwcap::cli
