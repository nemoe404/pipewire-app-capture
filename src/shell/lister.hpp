#ifndef PWCAP_SHELL_LISTER_HPP
#define PWCAP_SHELL_LISTER_HPP

// The pure part of list_applications: the conversion from the parsed node descriptions to the
// public AppStream entries. It has no PipeWire in it, so it is unit-tested without a daemon.

#include "core/graph.hpp"
#include "pwcap/list.hpp"

#include <vector>

namespace pwcap::shell {

// Keeps only the Stream/Output/Audio nodes (defence in depth: the lister binds only those), copies
// every field one to one, and sorts by app_name, then by serial.
[[nodiscard]] std::vector<AppStream> to_app_streams(std::vector<core::NodeInfo> nodes);

} // namespace pwcap::shell

#endif // PWCAP_SHELL_LISTER_HPP
