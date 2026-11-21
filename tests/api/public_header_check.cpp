// Compile-isolation check for the public API. This translation unit includes only the public header
// and is built with include/ as its sole include path (see the pwcap_public_header_check target),
// so a public header that needs a third-party or PipeWire header fails to build here. The
// static_asserts pin the shape of the API that the accepted decisions D-01 and D-03 changed. There
// is no main and no test framework: building the object file is the check.

#include <pwcap/capture.hpp>
#include <pwcap/list.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using pwcap::Capture;
using pwcap::Error;
using pwcap::SampleFormat;

static_assert(!std::is_copy_constructible_v<Capture>, "Capture owns a stream and is move-only");
static_assert(!std::is_copy_assignable_v<Capture>, "Capture owns a stream and is move-only");
static_assert(std::is_nothrow_move_constructible_v<Capture>, "Capture moves must not throw");
static_assert(std::is_nothrow_move_assignable_v<Capture>, "Capture moves must not throw");

static_assert(std::is_same_v<std::underlying_type_t<Error>, std::uint8_t>,
              "Error is a one-byte enum");
static_assert(std::is_same_v<std::underlying_type_t<SampleFormat>, std::uint8_t>,
              "SampleFormat is a one-byte enum");

static_assert(
    std::is_same_v<pwcap::Sink, std::move_only_function<void(std::span<const std::byte>)>>,
    "Sink is a move-only callback over a byte span");
static_assert(std::is_same_v<pwcap::Diagnostic, std::move_only_function<void(std::string_view)>>,
              "Diagnostic is a move-only callback over a string_view");

// D-03: to_string(Error) is usable in a constant expression.
static_assert(pwcap::to_string(Error::InvalidOptions) == "InvalidOptions",
              "to_string names the enumerator");
static_assert(noexcept(pwcap::to_string(Error::NoPipeWire)), "to_string is noexcept");

// D-01: InvalidOptions is appended after Stopped, so the earlier values keep their numbers.
static_assert(std::to_underlying(Error::InvalidOptions) == std::to_underlying(Error::Stopped) + 1,
              "InvalidOptions is the last enumerator, directly after Stopped");
static_assert(std::to_underlying(Error::NoPipeWire) == 0, "the first enumerator keeps value 0");

// list.hpp: the listing signature and the AppStream fields are fixed by D-09.
static_assert(std::is_same_v<decltype(pwcap::list_applications()),
                             std::expected<std::vector<pwcap::AppStream>, pwcap::Error>>,
              "list_applications returns the expected list of AppStream or an Error");
static_assert(std::is_aggregate_v<pwcap::AppStream>, "AppStream is an aggregate");
static_assert(std::is_same_v<decltype(pwcap::AppStream::serial), std::uint64_t>,
              "AppStream::serial is the 64-bit object.serial");
static_assert(std::is_same_v<decltype(pwcap::AppStream::pid), std::optional<std::int32_t>>,
              "AppStream::pid is optional, because some streams carry no process id");

} // namespace
