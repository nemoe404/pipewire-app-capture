#ifndef PWCAP_SHELL_PW_INIT_HPP
#define PWCAP_SHELL_PW_INIT_HPP

// The one place that calls pw_init. pw_init runs once per process and is never de-initialised: it
// ran clean under ASan and TSan that way. Every entry point that creates a PipeWire object calls
// init_pipewire_once() first.

#include <pipewire/pipewire.h>

#include <mutex>

namespace pwcap::shell {

// Inline with external linkage, so every translation unit shares the one once_flag.
inline void init_pipewire_once() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

} // namespace pwcap::shell

#endif // PWCAP_SHELL_PW_INIT_HPP
