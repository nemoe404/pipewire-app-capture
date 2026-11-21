#ifndef PWCAP_LIST_HPP
#define PWCAP_LIST_HPP

// Public API of pwcap: enumerate the applications that have an audio output stream, so that a
// caller can choose a Match for Capture::open. This header includes only standard headers and
// pwcap/capture.hpp (for Error); PipeWire stays behind the library.
//
// list_applications() returns one AppStream per PipeWire node of class Stream/Output/Audio, which
// is the set the include rule is matched against. A browser that plays from several tabs shows one
// entry per tab. The result is sorted by app_name, then by serial.
//
// The call blocks the calling thread for up to 2 s while it waits for the PipeWire registry. It
// must never be called from a Sink or a Diagnostic callback. The library writes nothing to the
// process's standard streams while it runs. With no PipeWire daemon, or when the registry does not
// answer within the deadline, it returns Error::NoPipeWire.

#include <pwcap/capture.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace pwcap {

// One application output stream. The four string fields are the four Match fields, so a caller
// can pass them straight back into Options::include. serial is the PipeWire object.serial.
struct AppStream {
    // The empty initialisers are kept so a designated initialiser may leave a field out.
    // NOLINTBEGIN(readability-redundant-member-init)
    std::string app_name{};   // application.name
    std::string binary{};     // application.process.binary
    std::string node_name{};  // node.name
    std::string media_name{}; // media.name, for example a browser tab title
    std::uint64_t serial{};   // object.serial
    // application.process.id is absent for some streams.
    std::optional<std::int32_t> pid{};
    // NOLINTEND(readability-redundant-member-init)
};

// Lists the application output streams that PipeWire currently exposes. An entry that disappears
// from the graph is gone from the next call.
[[nodiscard]] std::expected<std::vector<AppStream>, Error> list_applications();

} // namespace pwcap

#endif // PWCAP_LIST_HPP
