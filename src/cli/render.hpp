#ifndef PWCAP_CLI_RENDER_HPP
#define PWCAP_CLI_RENDER_HPP

// Pure rendering of the application list for pwcap list. No I/O and no PipeWire. Every string taken
// from a PipeWire property passes through here before it reaches stdout, so untrusted text (a page
// title, for example) cannot inject JSON syntax or terminal control sequences.

#include "pwcap/list.hpp"

#include <span>
#include <string>
#include <string_view>

namespace pwcap::cli {

// Escapes text for a JSON string body (RFC 8259): quote, backslash and every C0 control are
// escaped, invalid UTF-8 becomes U+FFFD, and all other code points are copied unchanged.
[[nodiscard]] std::string json_escape(std::string_view text);

// Makes text safe for one terminal row: every C0 control (TAB and ESC included), DEL and C1 control
// becomes '?', and invalid UTF-8 becomes U+FFFD.
[[nodiscard]] std::string display_safe(std::string_view text);

// The D-14 text listing: a header line, then one TAB-separated row per stream. An empty or absent
// value is '-'.
[[nodiscard]] std::string render_text(std::span<const pwcap::AppStream> streams);

// The D-14 JSON listing: one line holding an array of objects with the keys serial, pid, app_name,
// binary, node_name and media_name, in that order. pid is null when absent. An empty list is "[]".
[[nodiscard]] std::string render_json(std::span<const pwcap::AppStream> streams);

} // namespace pwcap::cli

#endif // PWCAP_CLI_RENDER_HPP
