#include "cli/args.hpp"

#include "pwcap/capture.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace pwcap::cli {

namespace {

constexpr std::string_view kCapture = "capture";
constexpr std::string_view kList = "list";
constexpr std::string_view kJsonFlag = "--json";
constexpr std::string_view kExcludeFlag = "--exclude";
constexpr std::string_view kRateFlag = "--rate";
constexpr std::string_view kChannelsFlag = "--channels";
constexpr std::string_view kFormatFlag = "--format";
constexpr std::string_view kNoReconnectFlag = "--no-reconnect";
constexpr std::string_view kQuietFlag = "--quiet";
constexpr std::string_view kS16le = "s16le";
constexpr std::string_view kF32le = "f32le";

// One Match field. name is the FIELD of --exclude FIELD=VALUE, flag is the include option, and
// member is the Match member that both of them set.
struct FieldSpec {
    std::string_view name;
    std::string_view flag;
    std::string Match::* member;
};

constexpr std::array<FieldSpec, 4> kFields{{
    {.name = "app", .flag = "--app", .member = &Match::app_name},
    {.name = "binary", .flag = "--binary", .member = &Match::binary},
    {.name = "node", .flag = "--node", .member = &Match::node_name},
    {.name = "media", .flag = "--media", .member = &Match::media_name},
}};

// The options that may appear once. Repeating one is DuplicateOption.
enum class Once : std::uint8_t { Rate, Channels, Format, NoReconnect, Quiet, Json, Count };
constexpr std::size_t kOnceCount = static_cast<std::size_t>(Once::Count);

// What has been seen so far. field[i] is kFields[i] given as an include flag.
struct Seen {
    std::bitset<kFields.size()> field;
    std::bitset<kOnceCount> once;
};

// A token split at the first '=' when it is an option: --name=value. A bare token is all name.
struct Token {
    std::string_view name;
    std::optional<std::string_view> value;
};

// The text of an option's value, where it was found, and the first token after the option.
struct Value {
    std::string_view text;
    std::size_t index;
    std::size_t next;
};

using Next = std::expected<std::size_t, UsageFailure>;

[[nodiscard]] bool is_help(std::string_view token) noexcept {
    return token == "--help" || token == "-h";
}

[[nodiscard]] std::unexpected<UsageFailure> fail(UsageError error, std::size_t index) {
    return std::unexpected{UsageFailure{.error = error, .index = index}};
}

[[nodiscard]] Token split_token(std::string_view token) noexcept {
    if (token.starts_with("--")) {
        if (const std::size_t equals = token.find('='); equals != std::string_view::npos) {
            return Token{.name = token.substr(0, equals), .value = token.substr(equals + 1)};
        }
    }
    return Token{.name = token, .value = std::nullopt};
}

// The value is the text after '=' when the token has one, otherwise the next token. A missing next
// token is MissingValue at args.size(), as the other missing-token errors are.
[[nodiscard]] std::expected<Value, UsageFailure> take_value(std::span<const std::string_view> args,
                                                            std::size_t index, const Token& token) {
    if (token.value) {
        return Value{.text = *token.value, .index = index, .next = index + 1};
    }
    if (index + 1 >= args.size()) {
        return fail(UsageError::MissingValue, args.size());
    }
    return Value{.text = args[index + 1], .index = index + 1, .next = index + 2};
}

[[nodiscard]] std::expected<Value, UsageFailure> take_text(std::span<const std::string_view> args,
                                                           std::size_t index, const Token& token) {
    std::expected<Value, UsageFailure> value = take_value(args, index, token);
    if (value && value->text.empty()) {
        return fail(UsageError::EmptyValue, value->index);
    }
    return value;
}

// Strict decimal parsing: only the digits 0 to 9, with no sign and no space, and the value must fit
// T. The fold saturates one past the limit, so an overflow stays out of range and never wraps.
template <std::unsigned_integral T>
[[nodiscard]] std::optional<T> parse_number(std::string_view text) noexcept {
    const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    if (text.empty() || !std::ranges::all_of(text, is_digit)) {
        return std::nullopt;
    }
    const auto limit = static_cast<std::uint64_t>(std::numeric_limits<T>::max());
    const std::uint64_t value =
        std::ranges::fold_left(text, std::uint64_t{0}, [](std::uint64_t acc, char c) {
            if (acc > limit) {
                return acc;
            }
            return (acc * 10U) + static_cast<std::uint64_t>(c - '0');
        });
    if (value > limit) {
        return std::nullopt;
    }
    return static_cast<T>(value);
}

// Marks a once-only option as seen. A repeat is DuplicateOption at the token's own index.
[[nodiscard]] std::expected<void, UsageFailure> claim(Seen& seen, Once option, std::size_t index) {
    const auto slot = static_cast<std::size_t>(option);
    if (seen.once.test(slot)) {
        return fail(UsageError::DuplicateOption, index);
    }
    seen.once.set(slot);
    return {};
}

// --exclude FIELD=VALUE. The split is at the first '=', so the value may itself contain '='.
[[nodiscard]] std::expected<void, UsageFailure> add_exclude(CaptureCommand& command,
                                                            const Value& value) {
    const std::size_t equals = value.text.find('=');
    if (equals == std::string_view::npos) {
        return fail(UsageError::BadExcludeField, value.index);
    }
    const std::string_view name = value.text.substr(0, equals);
    const std::string_view text = value.text.substr(equals + 1);
    const auto* const field = std::ranges::find(kFields, name, &FieldSpec::name);
    if (field == kFields.end()) {
        return fail(UsageError::BadExcludeField, value.index);
    }
    if (text.empty()) {
        return fail(UsageError::EmptyValue, value.index);
    }
    Match match;
    match.*(field->member) = std::string{text};
    command.options.exclude.push_back(std::move(match));
    return {};
}

[[nodiscard]] Next apply_exclude(CaptureCommand& command, std::span<const std::string_view> args,
                                 std::size_t index, const Token& token) {
    const std::expected<Value, UsageFailure> value = take_text(args, index, token);
    if (!value) {
        return std::unexpected{value.error()};
    }
    if (const auto added = add_exclude(command, *value); !added) {
        return std::unexpected{added.error()};
    }
    return value->next;
}

// An include flag, given once per field. It sets that field of Options::include.
[[nodiscard]] Next apply_include(CaptureCommand& command, Seen& seen,
                                 std::span<const std::string_view> args, std::size_t index,
                                 const Token& token, const FieldSpec& field, std::size_t slot) {
    if (seen.field.test(slot)) {
        return fail(UsageError::DuplicateOption, index);
    }
    const std::expected<Value, UsageFailure> value = take_text(args, index, token);
    if (!value) {
        return std::unexpected{value.error()};
    }
    seen.field.set(slot);
    command.options.include.*(field.member) = std::string{value->text};
    return value->next;
}

// The shape shared by --rate, --channels and --format: claim the once-only slot, read the value,
// then apply it. apply returns the UsageError to report, or nullopt when the value was accepted.
template <typename Apply>
[[nodiscard]] Next apply_once_value(Seen& seen, Once option, std::span<const std::string_view> args,
                                    std::size_t index, const Token& token, const Apply& apply) {
    if (const auto claimed = claim(seen, option, index); !claimed) {
        return std::unexpected{claimed.error()};
    }
    const std::expected<Value, UsageFailure> value = take_text(args, index, token);
    if (!value) {
        return std::unexpected{value.error()};
    }
    if (const std::optional<UsageError> error = apply(*value); error) {
        return fail(*error, value->index);
    }
    return value->next;
}

[[nodiscard]] std::optional<UsageError> set_rate(CaptureCommand& command, const Value& value) {
    const std::optional<std::uint32_t> rate = parse_number<std::uint32_t>(value.text);
    if (!rate) {
        return UsageError::BadNumber;
    }
    command.options.sample_rate = *rate;
    return std::nullopt;
}

[[nodiscard]] std::optional<UsageError> set_channels(CaptureCommand& command, const Value& value) {
    const std::optional<std::uint16_t> channels = parse_number<std::uint16_t>(value.text);
    if (!channels) {
        return UsageError::BadNumber;
    }
    command.options.channels = *channels;
    return std::nullopt;
}

[[nodiscard]] std::optional<UsageError> set_format(CaptureCommand& command, const Value& value) {
    if (value.text == kS16le) {
        command.options.format = SampleFormat::S16LE;
        return std::nullopt;
    }
    if (value.text == kF32le) {
        command.options.format = SampleFormat::F32LE;
        return std::nullopt;
    }
    return UsageError::BadFormat;
}

// The two flags that take no value. An inline value on either is an unknown option.
[[nodiscard]] Next apply_bare_flag(CaptureCommand& command, Seen& seen, std::size_t index,
                                   const Token& token) {
    if (token.value) {
        return fail(UsageError::UnknownOption, index);
    }
    if (token.name == kNoReconnectFlag) {
        if (const auto claimed = claim(seen, Once::NoReconnect, index); !claimed) {
            return std::unexpected{claimed.error()};
        }
        command.options.reconnect = false;
        return index + 1;
    }
    if (token.name == kQuietFlag) {
        if (const auto claimed = claim(seen, Once::Quiet, index); !claimed) {
            return std::unexpected{claimed.error()};
        }
        command.quiet = true;
        return index + 1;
    }
    return fail(UsageError::UnknownOption, index);
}

[[nodiscard]] Next apply_capture_option(CaptureCommand& command, Seen& seen,
                                        std::span<const std::string_view> args, std::size_t index,
                                        const Token& token) {
    if (token.name == kExcludeFlag) {
        return apply_exclude(command, args, index, token);
    }
    if (const auto* const field = std::ranges::find(kFields, token.name, &FieldSpec::flag);
        field != kFields.end()) {
        const auto slot = static_cast<std::size_t>(std::ranges::distance(kFields.begin(), field));
        return apply_include(command, seen, args, index, token, *field, slot);
    }
    if (token.name == kRateFlag) {
        return apply_once_value(
            seen, Once::Rate, args, index, token,
            [&command](const Value& value) { return set_rate(command, value); });
    }
    if (token.name == kChannelsFlag) {
        return apply_once_value(
            seen, Once::Channels, args, index, token,
            [&command](const Value& value) { return set_channels(command, value); });
    }
    if (token.name == kFormatFlag) {
        return apply_once_value(
            seen, Once::Format, args, index, token,
            [&command](const Value& value) { return set_format(command, value); });
    }
    return apply_bare_flag(command, seen, index, token);
}

[[nodiscard]] std::expected<Command, UsageFailure>
parse_capture(std::span<const std::string_view> args) {
    CaptureCommand command;
    Seen seen{};
    std::size_t index = 1;
    // A raw loop: each option consumes one or two tokens (the value may be the next token), and
    // every error reports the index it was found at. No range algorithm walks that way.
    while (index < args.size()) {
        const std::string_view raw = args[index];
        if (is_help(raw)) {
            return Command{HelpCommand{}};
        }
        const Next next = apply_capture_option(command, seen, args, index, split_token(raw));
        if (!next) {
            return std::unexpected{next.error()};
        }
        index = *next;
    }
    // At least one include flag is needed. Exclusions alone never select an application.
    if (seen.field.none()) {
        return fail(UsageError::NoInclude, args.size());
    }
    return Command{std::move(command)};
}

// list takes only --json and --help. Anything else is an unknown option, and a repeat of --json is
// a duplicate.
[[nodiscard]] std::expected<Command, UsageFailure>
parse_list(std::span<const std::string_view> args) {
    ListCommand command;
    Seen seen{};
    std::size_t index = 1;
    // A raw loop, for the same reason as parse_capture: each token is one option, and the index in
    // each error is the token's own.
    while (index < args.size()) {
        const std::string_view token = args[index];
        if (is_help(token)) {
            return Command{HelpCommand{}};
        }
        if (token != kJsonFlag) {
            return fail(UsageError::UnknownOption, index);
        }
        if (const auto claimed = claim(seen, Once::Json, index); !claimed) {
            return std::unexpected{claimed.error()};
        }
        command.json = true;
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
        return "option needs a value";
    case UsageError::EmptyValue:
        return "the value must not be empty";
    case UsageError::DuplicateOption:
        return "option may be given only once";
    case UsageError::NoInclude:
        return "capture needs at least one of --app, --binary, --node or --media";
    case UsageError::BadNumber:
        return "the value must be a whole number within the option's range";
    case UsageError::BadFormat:
        return "--format must be s16le or f32le";
    case UsageError::BadExcludeField:
        return "--exclude must be FIELD=VALUE, with FIELD app, binary, node or media";
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
    return parse_capture(args);
}

} // namespace pwcap::cli
