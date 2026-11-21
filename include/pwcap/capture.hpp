#ifndef PWCAP_CAPTURE_HPP
#define PWCAP_CAPTURE_HPP

// Public API of pwcap: capture the audio of one application from PipeWire and hand it to a
// callback as PCM. This header includes only standard headers; PipeWire stays behind the library.
//
// What the library does not do, by design:
//   - It never moves the application's stream away from the speakers. It adds an extra input and
//     links it to the application's output ports.
//   - It never falls back to the microphone or the system mix. With no matching application it
//     waits or reports Error::NoMatchingSource.
//   - It writes nothing to the process's standard streams and uses no logging framework.
//     Diagnostics reach the consumer only through the Diagnostic callback.
//
// Recording other people's calls needs their consent. See the README.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pwcap {

enum class SampleFormat : std::uint8_t { S16LE, F32LE };

// Selects application streams by their properties. Every comparison is exact and case-sensitive.
// An empty field matches any value. In Options::include at least one field must be non-empty.
struct Match {
    // The empty initialisers are kept so a designated initialiser may leave a field out.
    // NOLINTBEGIN(readability-redundant-member-init)
    std::string app_name{};   // application.name
    std::string binary{};     // application.process.binary
    std::string node_name{};  // node.name
    std::string media_name{}; // media.name, for example a browser tab title
    // NOLINTEND(readability-redundant-member-init)
};

// Output format. The supported (sample_rate, channels) pairs are 16000/1, 48000/2 and 48000/1.
// The capture stream itself always runs as F32, 48 kHz, FL/FR; the library converts.
struct Options {
    // NOLINTBEGIN(readability-redundant-member-init)
    Match include{};
    std::vector<Match> exclude{};
    std::uint32_t sample_rate{16000};
    std::uint16_t channels{1};
    SampleFormat format{SampleFormat::S16LE};
    bool reconnect{true};
    // NOLINTEND(readability-redundant-member-init)
};

// InvalidOptions is last: it was added after the first public sketch, and the existing values keep
// their numbers.
// clang-format off
enum class Error : std::uint8_t { NoPipeWire, InvalidMatch, NoMatchingSource, LinkFailed, Stopped,
                                  InvalidOptions };
// clang-format on

[[nodiscard]] constexpr std::string_view to_string(Error error) noexcept {
    switch (error) {
    case Error::NoPipeWire:
        return "NoPipeWire";
    case Error::InvalidMatch:
        return "InvalidMatch";
    case Error::NoMatchingSource:
        return "NoMatchingSource";
    case Error::LinkFailed:
        return "LinkFailed";
    case Error::Stopped:
        return "Stopped";
    case Error::InvalidOptions:
        return "InvalidOptions";
    }
    return {};
}

// Receives PCM in the format given by Options. Runs on one library thread, never on a PipeWire
// thread. Never destroy a Capture from inside this callback.
using Sink = std::move_only_function<void(std::span<const std::byte> pcm)>;

// Receives human-readable status and error messages, on the same library thread as the Sink. A
// message that reports an Error starts with the Error name and a colon, for example
// "LinkFailed: 102->47: ...". Never destroy a Capture from inside this callback.
using Diagnostic = std::move_only_function<void(std::string_view message)>;

class Capture {
public:
    // Opens a capture of the application selected by options.include.
    [[nodiscard]] static std::expected<Capture, Error> open(Options options, Sink sink,
                                                            Diagnostic diagnostic = {});

    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    Capture(Capture&&) noexcept;
    Capture& operator=(Capture&&) noexcept;
    ~Capture();

    // True while at least one selected application stream is linked into the capture. False on a
    // moved-from object.
    [[nodiscard]] bool linked() const noexcept;

private:
    struct Impl;
    explicit Capture(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace pwcap

#endif // PWCAP_CAPTURE_HPP
