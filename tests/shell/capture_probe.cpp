// A silent helper executable for subprocess checks. It writes nothing to either standard stream
// except in `control` mode, so a caller that redirects fds 1 and 2 to files can tell "the library
// wrote nothing" from "the redirect did not work".
//
// Modes and exit codes:
//   control                  writes one byte to each C standard stream, returns 0
//   no-daemon                open() must return NoPipeWire (the caller points PIPEWIRE_REMOTE at a
//                            missing socket); returns 0 on that result, 1 otherwise
//   invalid-match            open(Options{}) must return InvalidMatch; 0 on that, 1 otherwise
//   empty-sink               a valid include with an empty Sink must return InvalidOptions
//   list-no-daemon           list_applications() must return NoPipeWire (the caller points
//                            PIPEWIRE_REMOTE at a missing socket); returns 0 on that result, 1
//                            otherwise
//   hold <node_name> <ms>    capture the node, wait up to 3000 ms for linked(), hold until <ms>
//                            have passed since open, then close. 0 if linked was seen, 3 if not,
//                            1 if open failed
//   anything else            2

#include "pwcap/capture.hpp"
#include "pwcap/list.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;

constexpr int kOk = 0;
constexpr int kFail = 1;
constexpr int kUsage = 2;
constexpr int kNotLinked = 3;
constexpr auto kLinkWait = 3000ms;
constexpr auto kPoll = 10ms;

[[nodiscard]] pwcap::Sink discarding_sink() {
    return [](std::span<const std::byte> /*pcm*/) {};
}

[[nodiscard]] pwcap::Options include_node(std::string_view node_name) {
    pwcap::Options options;
    options.include.node_name = std::string{node_name};
    return options;
}

[[nodiscard]] int expect_error(const std::expected<pwcap::Capture, pwcap::Error>& result,
                               pwcap::Error wanted) {
    return !result && result.error() == wanted ? kOk : kFail;
}

[[nodiscard]] int hold(std::string_view node_name, std::chrono::milliseconds hold_for) {
    const auto start = std::chrono::steady_clock::now();
    auto capture = pwcap::Capture::open(include_node(node_name), discarding_sink());
    if (!capture) {
        return kFail;
    }
    bool linked = false;
    const auto link_deadline = start + kLinkWait;
    // A raw loop: polls a state flag against a deadline; no algorithm expresses that.
    while (!linked && std::chrono::steady_clock::now() < link_deadline) {
        linked = capture->linked();
        if (!linked) {
            std::this_thread::sleep_for(kPoll);
        }
    }
    std::this_thread::sleep_until(start + hold_for);
    return linked ? kOk : kNotLinked;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> args{argv, static_cast<std::size_t>(argc)};
    if (args.size() < 2) {
        return kUsage;
    }
    const std::string_view mode{args[1]};
    if (mode == "control") {
        static_cast<void>(std::fputc('o', stdout));
        static_cast<void>(std::fputc('e', stderr));
        return kOk;
    }
    if (mode == "no-daemon") {
        return expect_error(
            pwcap::Capture::open(include_node("pwcap-probe-none"), discarding_sink()),
            pwcap::Error::NoPipeWire);
    }
    if (mode == "invalid-match") {
        return expect_error(pwcap::Capture::open(pwcap::Options{}, discarding_sink()),
                            pwcap::Error::InvalidMatch);
    }
    if (mode == "empty-sink") {
        return expect_error(pwcap::Capture::open(include_node("pwcap-probe-none"), pwcap::Sink{}),
                            pwcap::Error::InvalidOptions);
    }
    if (mode == "list-no-daemon") {
        const auto listed = pwcap::list_applications();
        return !listed && listed.error() == pwcap::Error::NoPipeWire ? kOk : kFail;
    }
    if (mode == "hold" && args.size() == 4) {
        const std::string_view text{args[3]};
        std::int64_t ms = 0;
        const auto [end, error] =
            std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), ms);
        if (error != std::errc{} || end != std::to_address(text.end()) || ms < 0) {
            return kUsage;
        }
        return hold(std::string_view{args[2]}, std::chrono::milliseconds{ms});
    }
    return kUsage;
}
