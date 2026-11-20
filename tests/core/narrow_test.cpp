#include "core/narrow.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace pwcap::core {
namespace {

static_assert(checked_narrow<std::int32_t>(std::size_t{2147483647}) == 2147483647);
static_assert(!checked_narrow<std::int32_t>(std::size_t{2147483648}).has_value());

TEST(CheckedNarrow, GuardsTheInt32Boundary) {
    EXPECT_EQ(checked_narrow<std::int32_t>(std::size_t{2147483647}),
              std::optional<std::int32_t>{2147483647});
    EXPECT_EQ(checked_narrow<std::int32_t>(std::size_t{2147483648}), std::nullopt);
    EXPECT_EQ(checked_narrow<std::int32_t>(SIZE_MAX), std::nullopt);
    EXPECT_EQ(checked_narrow<std::uint32_t>(std::int64_t{-1}), std::nullopt);
    EXPECT_EQ(checked_narrow<std::uint32_t>(std::int64_t{0}), std::optional<std::uint32_t>{0});
}

} // namespace
} // namespace pwcap::core
