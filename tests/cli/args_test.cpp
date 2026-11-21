#include "cli/args.hpp"

#include "pwcap/capture.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <string_view>
#include <variant>
#include <vector>

namespace {

using pwcap::cli::CaptureCommand;
using pwcap::cli::Command;
using pwcap::cli::HelpCommand;
using pwcap::cli::ListCommand;
using pwcap::cli::UsageError;
using pwcap::cli::UsageFailure;

using Result = std::expected<Command, UsageFailure>;

[[nodiscard]] Result parse(std::initializer_list<std::string_view> tokens) {
    const std::vector<std::string_view> args{tokens};
    return pwcap::cli::parse_command_line(args);
}

// Every UsageError value. Used by the description test.
constexpr std::array<UsageError, 10> kAllUsageErrors{
    UsageError::MissingCommand,  UsageError::UnknownCommand, UsageError::UnknownOption,
    UsageError::MissingValue,    UsageError::EmptyValue,     UsageError::DuplicateOption,
    UsageError::NoInclude,       UsageError::BadNumber,      UsageError::BadFormat,
    UsageError::BadExcludeField,
};

} // namespace

TEST(Args, CaptureAppDefaults) {
    const Result result = parse({"capture", "--app", "Zoom"});
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<CaptureCommand>(*result));
    const auto& command = std::get<CaptureCommand>(*result);

    EXPECT_EQ(command.options.include.app_name, "Zoom");
    EXPECT_TRUE(command.options.include.binary.empty());
    EXPECT_TRUE(command.options.include.node_name.empty());
    EXPECT_TRUE(command.options.include.media_name.empty());
    EXPECT_TRUE(command.options.exclude.empty());
    EXPECT_EQ(command.options.sample_rate, std::uint32_t{16000});
    EXPECT_EQ(command.options.channels, std::uint16_t{1});
    EXPECT_EQ(command.options.format, pwcap::SampleFormat::S16LE);
    EXPECT_TRUE(command.options.reconnect);
    EXPECT_FALSE(command.quiet);
}

TEST(Args, EqualsFormIsTheSame) {
    const Result spaced = parse({"capture", "--app", "Zoom"});
    const Result equals = parse({"capture", "--app=Zoom"});
    ASSERT_TRUE(spaced.has_value());
    ASSERT_TRUE(equals.has_value());
    ASSERT_TRUE(std::holds_alternative<CaptureCommand>(*equals));

    const auto& left = std::get<CaptureCommand>(*spaced);
    const auto& right = std::get<CaptureCommand>(*equals);
    EXPECT_EQ(right.options.include.app_name, left.options.include.app_name);
    EXPECT_EQ(right.options.sample_rate, left.options.sample_rate);
    EXPECT_EQ(right.options.channels, left.options.channels);
    EXPECT_EQ(right.options.format, left.options.format);
    EXPECT_EQ(right.options.reconnect, left.options.reconnect);
    EXPECT_EQ(right.quiet, left.quiet);
}

TEST(Args, UsageFailuresCarryTheIndex) {
    // An index of args.size() means a token is missing; otherwise it is the offending token.
    const Result no_args = parse({});
    ASSERT_FALSE(no_args.has_value());
    EXPECT_EQ(no_args.error().error, UsageError::MissingCommand);
    EXPECT_EQ(no_args.error().index, std::size_t{0});

    const Result unknown_command = parse({"frobnicate"});
    ASSERT_FALSE(unknown_command.has_value());
    EXPECT_EQ(unknown_command.error().error, UsageError::UnknownCommand);
    EXPECT_EQ(unknown_command.error().index, std::size_t{0});

    const Result no_include = parse({"capture"});
    ASSERT_FALSE(no_include.has_value());
    EXPECT_EQ(no_include.error().error, UsageError::NoInclude);
    EXPECT_EQ(no_include.error().index, std::size_t{1});

    const Result missing_value = parse({"capture", "--app"});
    ASSERT_FALSE(missing_value.has_value());
    EXPECT_EQ(missing_value.error().error, UsageError::MissingValue);
    EXPECT_EQ(missing_value.error().index, std::size_t{2});

    const Result empty_value = parse({"capture", "--app", ""});
    ASSERT_FALSE(empty_value.has_value());
    EXPECT_EQ(empty_value.error().error, UsageError::EmptyValue);
    EXPECT_EQ(empty_value.error().index, std::size_t{2});

    const Result duplicate = parse({"capture", "--app", "A", "--app", "B"});
    ASSERT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().error, UsageError::DuplicateOption);
    EXPECT_EQ(duplicate.error().index, std::size_t{3});

    const Result unknown_option = parse({"capture", "--bogus"});
    ASSERT_FALSE(unknown_option.has_value());
    EXPECT_EQ(unknown_option.error().error, UsageError::UnknownOption);
    EXPECT_EQ(unknown_option.error().index, std::size_t{1});
}

TEST(Args, HelpAnywhere) {
    for (const Result& result : {parse({"--help"}), parse({"-h"}), parse({"capture", "--help"})}) {
        ASSERT_TRUE(result.has_value());
        EXPECT_TRUE(std::holds_alternative<HelpCommand>(*result));
    }
}

TEST(Args, DescribeIsNonEmptyForEveryError) {
    for (const UsageError error : kAllUsageErrors) {
        EXPECT_FALSE(pwcap::cli::describe(error).empty());
    }
}

namespace {

// The number of non-empty fields of a Match. A valid exclude rule sets exactly one.
[[nodiscard]] std::ptrdiff_t fields_set(const pwcap::Match& match) {
    const std::array<std::string, 4> fields{match.app_name, match.binary, match.node_name,
                                            match.media_name};
    return std::ranges::count_if(fields, [](const std::string& field) { return !field.empty(); });
}

} // namespace

TEST(Args, AllIncludeFlagsCombine) {
    const Result result =
        parse({"capture", "--app", "A", "--binary", "B", "--node", "N", "--media", "M"});
    ASSERT_TRUE(result.has_value());
    const auto& command = std::get<CaptureCommand>(*result);
    EXPECT_EQ(command.options.include.app_name, "A");
    EXPECT_EQ(command.options.include.binary, "B");
    EXPECT_EQ(command.options.include.node_name, "N");
    EXPECT_EQ(command.options.include.media_name, "M");
}

TEST(Args, ExcludeIsRepeatable) {
    const Result result = parse({"capture", "--app", "A", "--exclude", "app=X", "--exclude",
                                 "media=Some Tab", "--exclude=node=N"});
    ASSERT_TRUE(result.has_value());
    const auto& exclude = std::get<CaptureCommand>(*result).options.exclude;
    ASSERT_EQ(exclude.size(), std::size_t{3});
    EXPECT_EQ(exclude[0].app_name, "X");
    EXPECT_EQ(exclude[1].media_name, "Some Tab");
    EXPECT_EQ(exclude[2].node_name, "N");
    std::ranges::for_each(exclude,
                          [](const pwcap::Match& match) { EXPECT_EQ(fields_set(match), 1); });
}

TEST(Args, ExcludeRejectsBadField) {
    const Result bad_name = parse({"capture", "--app", "A", "--exclude", "foo=x"});
    ASSERT_FALSE(bad_name.has_value());
    EXPECT_EQ(bad_name.error().error, UsageError::BadExcludeField);
    EXPECT_EQ(bad_name.error().index, std::size_t{4});

    const Result no_equals = parse({"capture", "--app", "A", "--exclude", "app"});
    ASSERT_FALSE(no_equals.has_value());
    EXPECT_EQ(no_equals.error().error, UsageError::BadExcludeField);
    EXPECT_EQ(no_equals.error().index, std::size_t{4});

    const Result empty_text = parse({"capture", "--app", "A", "--exclude", "app="});
    ASSERT_FALSE(empty_text.has_value());
    EXPECT_EQ(empty_text.error().error, UsageError::EmptyValue);
}

TEST(Args, RateChannelsFormat) {
    const Result float_result =
        parse({"capture", "--app", "A", "--rate", "48000", "--channels", "2", "--format", "f32le"});
    ASSERT_TRUE(float_result.has_value());
    const auto& floats = std::get<CaptureCommand>(*float_result).options;
    EXPECT_EQ(floats.sample_rate, std::uint32_t{48000});
    EXPECT_EQ(floats.channels, std::uint16_t{2});
    EXPECT_EQ(floats.format, pwcap::SampleFormat::F32LE);

    const Result short_result = parse({"capture", "--app", "A", "--format", "s16le"});
    ASSERT_TRUE(short_result.has_value());
    EXPECT_EQ(std::get<CaptureCommand>(*short_result).options.format, pwcap::SampleFormat::S16LE);
}

TEST(Args, NumbersAreStrict) {
    const Result fraction = parse({"capture", "--app", "A", "--rate", "48k"});
    ASSERT_FALSE(fraction.has_value());
    EXPECT_EQ(fraction.error().error, UsageError::BadNumber);

    const Result negative = parse({"capture", "--app", "A", "--rate", "-1"});
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().error, UsageError::BadNumber);

    const Result too_big_rate = parse({"capture", "--app", "A", "--rate", "4294967296"});
    ASSERT_FALSE(too_big_rate.has_value());
    EXPECT_EQ(too_big_rate.error().error, UsageError::BadNumber);

    const Result too_many_channels = parse({"capture", "--app", "A", "--channels", "65536"});
    ASSERT_FALSE(too_many_channels.has_value());
    EXPECT_EQ(too_many_channels.error().error, UsageError::BadNumber);

    const Result format = parse({"capture", "--app", "A", "--format", "mp3"});
    ASSERT_FALSE(format.has_value());
    EXPECT_EQ(format.error().error, UsageError::BadFormat);
    EXPECT_EQ(format.error().index, std::size_t{4});
}

TEST(Args, UnsupportedPairIsLeftToTheLibrary) {
    const Result result = parse({"capture", "--app", "A", "--rate", "44100", "--channels", "1"});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(std::get<CaptureCommand>(*result).options.sample_rate, std::uint32_t{44100});
    EXPECT_EQ(std::get<CaptureCommand>(*result).options.channels, std::uint16_t{1});
}

TEST(Args, NoReconnectAndQuiet) {
    const Result result = parse({"capture", "--app", "A", "--no-reconnect", "--quiet"});
    ASSERT_TRUE(result.has_value());
    const auto& command = std::get<CaptureCommand>(*result);
    EXPECT_FALSE(command.options.reconnect);
    EXPECT_TRUE(command.quiet);

    const Result repeat_reconnect =
        parse({"capture", "--app", "A", "--no-reconnect", "--no-reconnect"});
    ASSERT_FALSE(repeat_reconnect.has_value());
    EXPECT_EQ(repeat_reconnect.error().error, UsageError::DuplicateOption);

    const Result repeat_quiet = parse({"capture", "--app", "A", "--quiet", "--quiet"});
    ASSERT_FALSE(repeat_quiet.has_value());
    EXPECT_EQ(repeat_quiet.error().error, UsageError::DuplicateOption);
}

TEST(Args, ExcludeAloneIsNoSelection) {
    const Result result = parse({"capture", "--exclude", "app=X"});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().error, UsageError::NoInclude);
}

TEST(Args, ListFlags) {
    const Result plain = parse({"list"});
    ASSERT_TRUE(plain.has_value());
    ASSERT_TRUE(std::holds_alternative<ListCommand>(*plain));
    EXPECT_FALSE(std::get<ListCommand>(*plain).json);

    const Result json = parse({"list", "--json"});
    ASSERT_TRUE(json.has_value());
    EXPECT_TRUE(std::get<ListCommand>(*json).json);

    const Result unknown = parse({"list", "--app", "x"});
    ASSERT_FALSE(unknown.has_value());
    EXPECT_EQ(unknown.error().error, UsageError::UnknownOption);

    const Result repeat = parse({"list", "--json", "--json"});
    ASSERT_FALSE(repeat.has_value());
    EXPECT_EQ(repeat.error().error, UsageError::DuplicateOption);
}
