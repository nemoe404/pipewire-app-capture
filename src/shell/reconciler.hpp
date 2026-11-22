#ifndef PWCAP_SHELL_RECONCILER_HPP
#define PWCAP_SHELL_RECONCILER_HPP

// The reconciler mirrors the PipeWire registry into a core::GraphSnapshot and applies the links
// that core::plan_links asks for. It is the only code in src/shell that creates or removes links
// (SAFE-03).
//
// Threading. Every member except linked() runs on the PipeWire loop thread (from a registry,
// node, proxy or core callback) or on the thread that holds the thread-loop lock (create, start
// and the destructor). linked() reads an atomic and is safe from any thread.

#include "core/graph.hpp"
#include "core/match.hpp"
#include "pwcap/capture.hpp"
#include "shell/diagnostics.hpp"
#include "shell/pw_handles.hpp"

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pwcap::shell {

// Parsers for properties that come from other processes. Numbers are parsed with
// std::from_chars and the whole string must be consumed. Any failure gives std::nullopt, so an
// object that cannot be understood is ignored (fail closed, CORE-05).
[[nodiscard]] std::optional<core::NodeInfo> node_from_props(std::uint32_t id,
                                                            const spa_dict& props);
[[nodiscard]] std::optional<core::PortInfo> port_from_props(std::uint32_t id,
                                                            const spa_dict& props);
[[nodiscard]] std::optional<core::LinkInfo> link_from_props(const spa_dict& props);

struct ReconcilerConfig {
    core::Selection selection;
    bool reconnect{true};
    // Non-owning; outlives the reconciler.
    DiagnosticQueue* diagnostics{nullptr};
};

// The capture stream is F32 FL/FR (D-04), so two input ports are expected on its node.
inline constexpr std::size_t kExpectedCaptureInputs = 2;

class Reconciler {
    struct PrivateTag {
        explicit PrivateTag() = default;
    };

public:
    // Called with the thread-loop lock held. NoPipeWire when the registry cannot be obtained.
    [[nodiscard]] static std::expected<std::unique_ptr<Reconciler>, pwcap::Error>
    create(pw_thread_loop* loop, pw_core* core, ReconcilerConfig config);

    Reconciler(PrivateTag /*key*/, pw_thread_loop* loop, pw_core* core, ReconcilerConfig config);
    Reconciler(const Reconciler&) = delete;
    Reconciler& operator=(const Reconciler&) = delete;
    Reconciler(Reconciler&&) = delete;
    Reconciler& operator=(Reconciler&&) = delete;
    // The caller holds the thread-loop lock. Destroys our link proxies first (which removes our
    // links), then the bound node proxies, then the registry.
    ~Reconciler();

    // Called with the lock held; waits (releasing it) until the initial registry round trip is
    // done, every bound Stream/Output/Audio node delivered its first info, and the capture node's
    // input ports are mirrored. Then runs the first reconcile. NoPipeWire on timeout.
    //
    // reconnect=true: succeeds even when no node matches; the reconciler then waits and posts
    // "NoMatchingSource:". reconnect=false: NoMatchingSource when nothing matches; otherwise the
    // object.serial of every selected node is pinned, the first links are requested, and the call
    // waits until each is announced (LinkFailed when one is rejected or the wait times out).
    [[nodiscard]] std::expected<void, pwcap::Error> start(std::uint32_t capture_node_id,
                                                          std::chrono::milliseconds timeout);

    [[nodiscard]] bool linked() const noexcept { return linked_.load(); }

private:
    struct BoundNode;
    struct NodeEntry;
    struct OwnedLink;

    // Registry callbacks.
    void on_global(std::uint32_t id, const char* type, const spa_dict* props);
    void on_global_remove(std::uint32_t id);
    // Core callbacks.
    void on_core_done(std::uint32_t id, int seq);
    void on_core_error(std::uint32_t id, int res, const char* message);
    // Node callback: the bound node delivered (or changed) its info.
    void on_node_info(std::uint32_t id, const pw_node_info* info);
    // Link proxy callbacks.
    void on_link_removed(const core::LinkInfo& key, const OwnedLink* entry);
    void on_link_error(const core::LinkInfo& key, const OwnedLink* entry, int res,
                       const char* message);

    void schedule_reconcile();
    void reconcile();
    // The part of reconcile() that depends on the reconnect mode and on what is selected.
    void track_sources(const std::vector<core::NodeId>& sources);
    [[nodiscard]] core::GraphSnapshot pinned_view(core::GraphSnapshot snapshot) const;
    [[nodiscard]] bool pinned_stream_present() const;
    [[nodiscard]] bool first_links_settled() const;
    // The registry changed: a link request that failed may be tried again.
    void registry_changed();
    void apply_remove(const core::LinkInfo& link);
    void apply_add(const core::LinkInfo& link);
    [[nodiscard]] core::GraphSnapshot build_snapshot() const;
    [[nodiscard]] std::vector<core::PortId> capture_inputs() const;
    [[nodiscard]] bool ready_to_start() const;
    void post(std::string message) const;

    friend struct Callbacks;

    pw_thread_loop* loop_;
    pw_core* core_;
    core::Selection selection_;
    bool reconnect_;
    DiagnosticQueue* diagnostics_;

    // Handles first, then the hooks that listen to them (see pw_handles.hpp).
    ProxyPtr registry_;
    HookGuard registry_hook_;
    HookGuard core_hook_;

    // Node-based containers: PipeWire keeps the addresses of the hooks and the callback data
    // stored in the entries, so entries must never move. Keys are global ids, which PipeWire
    // recycles, so nothing is ever indexed by position.
    std::map<std::uint32_t, std::unique_ptr<NodeEntry>> nodes_;
    std::map<std::uint32_t, core::PortInfo> ports_;
    std::map<std::uint32_t, core::LinkInfo> links_;
    std::map<core::LinkInfo, std::unique_ptr<OwnedLink>> owned_links_;

    // Link requests PipeWire rejected, kept alive (not destroyed inside their own callback) and
    // not repeated until the registry changes.
    std::vector<std::unique_ptr<OwnedLink>> failed_links_;
    std::set<core::LinkInfo> failed_keys_;
    // object.serial of the nodes selected at open; used only with reconnect=false.
    std::vector<core::Serial> pinned_;

    enum class SourceState : std::uint8_t { Unknown, Present, Absent };
    SourceState source_state_{SourceState::Unknown};
    // Nothing is planned, posted or linked before start() has seen a complete registry.
    bool started_{false};
    bool stopped_posted_{false};
    bool lost_connection_{false};
    std::size_t link_failures_{0};

    std::uint32_t capture_node_id_{PW_ID_ANY};
    int sync_seq_{0};
    bool sync_outstanding_{false};
    bool dirty_{false};
    bool initial_done_{false};
    std::atomic<bool> linked_{false};
};

} // namespace pwcap::shell

#endif // PWCAP_SHELL_RECONCILER_HPP
