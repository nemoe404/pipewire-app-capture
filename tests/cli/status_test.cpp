#include "cli/status.hpp"

#include "pwcap/capture.hpp"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace {

using pwcap::Error;
using pwcap::cli::error_prefix;
using pwcap::cli::is_fatal;

// The six Error values. The names come from to_string, never from string literals in this file.
constexpr std::array<Error, 6> kAllErrors{Error::NoPipeWire,       Error::InvalidMatch,
                                          Error::NoMatchingSource, Error::LinkFailed,
                                          Error::Stopped,          Error::InvalidOptions};

} // namespace

TEST(Status, EveryErrorNameWithColonIsRecognised) {
    for (const Error error : kAllErrors) {
        const std::string message = std::string{pwcap::to_string(error)} + ": x";
        const std::optional<Error> found = error_prefix(message);
        EXPECT_EQ(found, std::optional<Error>{error}) << message;
    }
}

TEST(Status, NonErrorPrefixesAreNotErrors) {
    EXPECT_FALSE(error_prefix("Overrun: dropped 10 frames").has_value());
    EXPECT_FALSE(error_prefix("pwcap: matching stream found; linking").has_value());
    EXPECT_FALSE(error_prefix("Stopped").has_value());
    EXPECT_FALSE(error_prefix("StoppedX: y").has_value());
    EXPECT_FALSE(error_prefix("").has_value());
}

TEST(Status, OnlyStoppedAndNoPipeWireAreFatal) {
    EXPECT_TRUE(is_fatal(Error::Stopped));
    EXPECT_TRUE(is_fatal(Error::NoPipeWire));
    EXPECT_FALSE(is_fatal(Error::InvalidMatch));
    EXPECT_FALSE(is_fatal(Error::NoMatchingSource));
    EXPECT_FALSE(is_fatal(Error::LinkFailed));
    EXPECT_FALSE(is_fatal(Error::InvalidOptions));
}
