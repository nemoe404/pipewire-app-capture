#include "cli/args.hpp"

#include "pwcap/capture.hpp"

#include <gtest/gtest.h>

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
using pwcap::cli::UsageError;
using pwcap::cli::UsageFailure;

using Result = std::expected<Command, UsageFailure>;

[[nodiscard]] Result parse(std::initializer_list<std::string_view> tokens) {
    const std::vector<std::string_view> args{tokens};
    return pwcap::cli::parse_command_line(args);
}

// Every UsageError value. Used by the description test.
constexpr std::array<UsageError, 7> kAllUsageErrors{
    UsageError::MissingCommand, UsageError::UnknownCommand, UsageError::UnknownOption,
    UsageError::MissingValue,   UsageError::EmptyValue,     UsageError::DuplicateOption,
    UsageError::NoInclude,
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
