#ifndef PWCAP_SHELL_DIAGNOSTICS_HPP
#define PWCAP_SHELL_DIAGNOSTICS_HPP

// A mutex-guarded batch queue of diagnostic messages. The PipeWire loop thread posts; the library
// consumer thread takes the pending batch and calls the user's Diagnostic callback outside the
// lock. Standard headers only.
//
// post() allocates and locks, so it must never be called from the real-time process callback.

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pwcap::shell {

class DiagnosticQueue {
public:
    void post(std::string message) {
        const std::scoped_lock lock{mutex_};
        pending_.push_back(std::move(message));
    }

    // Swaps the pending messages out, so the lock is held only for the swap.
    [[nodiscard]] std::vector<std::string> take() {
        std::vector<std::string> taken;
        {
            const std::scoped_lock lock{mutex_};
            taken.swap(pending_);
        }
        return taken;
    }

private:
    std::mutex mutex_;
    // NOLINTNEXTLINE(readability-redundant-member-init)
    std::vector<std::string> pending_{};
};

} // namespace pwcap::shell

#endif // PWCAP_SHELL_DIAGNOSTICS_HPP
