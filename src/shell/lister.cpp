#include "pwcap/list.hpp"

#include "core/graph.hpp"
#include "shell/lister.hpp"
#include "shell/pw_handles.hpp"
#include "shell/pw_init.hpp"
#include "shell/reconciler.hpp"

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace pwcap {

namespace {

// Every call is bounded by this timer on the caller's main loop (T-05-07).
constexpr int kDeadlineSeconds = 2;

class Lister;
struct ListerCallbacks;

// One bound Stream/Output/Audio node. The proxy is declared before its hook, so the hook goes
// first.
struct Entry {
    Lister* owner{};
    std::uint32_t id{};
    shell::ProxyPtr proxy;
    shell::HookGuard hook;
    // Set only from the node's own info props. The registry props lack media.name and the binary.
    std::optional<core::NodeInfo> info;
    bool info_seen{false};
};

// Runs one registry round trip on the caller's thread with a pw_main_loop. No thread loop is
// started, so there is no loop thread to stop (architecture section 4).
class Lister {
public:
    Lister() = default;
    Lister(const Lister&) = delete;
    Lister& operator=(const Lister&) = delete;
    Lister(Lister&&) = delete;
    Lister& operator=(Lister&&) = delete;
    // Destroys the timer while the loop still exists. The members then go in reverse declaration
    // order: entries, the registry hook and registry, the core hook, the core, the context, the
    // loop.
    ~Lister();

    // NoPipeWire when the connection cannot be made, the registry cannot be obtained, the
    // connection is lost, or the deadline passes before the first registry round trip finishes.
    [[nodiscard]] std::expected<void, Error> run();

    // The info of every entry whose first info parsed. A node still without info is left out.
    [[nodiscard]] std::vector<core::NodeInfo> parsed_nodes() const;

private:
    friend struct ListerCallbacks;

    void on_global(std::uint32_t id, const char* type, const spa_dict* props);
    void on_global_remove(std::uint32_t id);
    void on_core_done(std::uint32_t id, int seq);
    void on_core_error(std::uint32_t id, int res);
    void on_node_info(std::uint32_t id, const pw_node_info* info);
    void on_deadline();
    // Issues a core sync and records its seq. False when PipeWire refuses it.
    [[nodiscard]] bool sync();
    [[nodiscard]] bool all_infos_seen() const;
    void quit() const;

    // Declaration order is the teardown order (see the destructor). Each hook follows the handle
    // it listens to.
    shell::MainLoopPtr loop_;
    shell::ContextPtr context_;
    shell::CorePtr connection_;
    shell::HookGuard core_hook_;
    shell::ProxyPtr registry_;
    shell::HookGuard registry_hook_;
    // Node-based: PipeWire keeps the addresses of the hooks and the callback data stored here.
    std::map<std::uint32_t, std::unique_ptr<Entry>> entries_;
    spa_source* timer_{nullptr};
    int sync_seq_{0};
    bool first_done_{false};
    bool failed_{false};
    bool timed_out_{false};
};

// Static trampolines from PipeWire's C callbacks to the Lister. A friend of Lister.
struct ListerCallbacks {
    static void registry_global(void* data, std::uint32_t id, std::uint32_t /*permissions*/,
                                const char* type, std::uint32_t /*version*/,
                                const spa_dict* props) {
        static_cast<Lister*>(data)->on_global(id, type, props);
    }
    static void registry_global_remove(void* data, std::uint32_t id) {
        static_cast<Lister*>(data)->on_global_remove(id);
    }
    static void core_done(void* data, std::uint32_t id, int seq) {
        static_cast<Lister*>(data)->on_core_done(id, seq);
    }
    static void core_error(void* data, std::uint32_t id, int /*seq*/, int res,
                           const char* /*message*/) {
        static_cast<Lister*>(data)->on_core_error(id, res);
    }
    static void node_info(void* data, const pw_node_info* info) {
        auto* entry = static_cast<Entry*>(data);
        entry->owner->on_node_info(entry->id, info);
    }
    static void deadline(void* data, std::uint64_t /*expirations*/) {
        static_cast<Lister*>(data)->on_deadline();
    }
};

// The event tables are built by assignment: a partial designated initialiser of these C structs
// would trip -Wmissing-designated-field-initializers.
constexpr pw_registry_events make_registry_events() noexcept {
    pw_registry_events events{};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = &ListerCallbacks::registry_global;
    events.global_remove = &ListerCallbacks::registry_global_remove;
    return events;
}

constexpr pw_core_events make_core_events() noexcept {
    pw_core_events events{};
    events.version = PW_VERSION_CORE_EVENTS;
    events.done = &ListerCallbacks::core_done;
    events.error = &ListerCallbacks::core_error;
    return events;
}

constexpr pw_node_events make_node_events() noexcept {
    pw_node_events events{};
    events.version = PW_VERSION_NODE_EVENTS;
    events.info = &ListerCallbacks::node_info;
    return events;
}

constexpr pw_registry_events kRegistryEvents = make_registry_events();
constexpr pw_core_events kCoreEvents = make_core_events();
constexpr pw_node_events kNodeEvents = make_node_events();

// ---------------------------------------------------------------------------------------------

Lister::~Lister() {
    if (timer_ != nullptr && loop_) {
        pw_loop_destroy_source(pw_main_loop_get_loop(loop_.get()), timer_);
    }
}

std::expected<void, Error> Lister::run() {
    loop_.reset(pw_main_loop_new(nullptr));
    if (!loop_) {
        return std::unexpected{Error::NoPipeWire};
    }
    pw_loop* loop = pw_main_loop_get_loop(loop_.get());
    context_.reset(pw_context_new(loop, nullptr, 0));
    if (!context_) {
        return std::unexpected{Error::NoPipeWire};
    }
    connection_.reset(pw_context_connect(context_.get(), nullptr, 0));
    if (!connection_) {
        return std::unexpected{Error::NoPipeWire};
    }
    pw_registry* registry = pw_core_get_registry(connection_.get(), PW_VERSION_REGISTRY, 0);
    if (registry == nullptr) {
        return std::unexpected{Error::NoPipeWire};
    }
    registry_ = shell::make_registry_handle(registry);
    pw_registry_add_listener(registry, registry_hook_.get(), &kRegistryEvents, this);
    pw_core_add_listener(connection_.get(), core_hook_.get(), &kCoreEvents, this);

    timer_ = pw_loop_add_timer(loop, &ListerCallbacks::deadline, this);
    if (timer_ == nullptr) {
        return std::unexpected{Error::NoPipeWire};
    }
    timespec deadline{};
    deadline.tv_sec = kDeadlineSeconds;
    if (pw_loop_update_timer(loop, timer_, &deadline, nullptr, false) < 0) {
        return std::unexpected{Error::NoPipeWire};
    }
    if (!sync()) {
        return std::unexpected{Error::NoPipeWire};
    }
    // Blocks the caller until quit() is called from a done, error or deadline event.
    static_cast<void>(pw_main_loop_run(loop_.get()));

    if (failed_ || (timed_out_ && !first_done_)) {
        return std::unexpected{Error::NoPipeWire};
    }
    return {};
}

std::vector<core::NodeInfo> Lister::parsed_nodes() const {
    return entries_ | std::views::values |
           std::views::filter(
               [](const std::unique_ptr<Entry>& entry) { return entry->info.has_value(); }) |
           std::views::transform([](const std::unique_ptr<Entry>& entry) { return *entry->info; }) |
           std::ranges::to<std::vector>();
}

void Lister::on_global(std::uint32_t id, const char* type, const spa_dict* props) {
    if (type == nullptr || props == nullptr || std::string_view{type} != PW_TYPE_INTERFACE_Node) {
        return;
    }
    // Ids are recycled, so any entry under this id belongs to an earlier object.
    entries_.erase(id);
    const auto info = shell::node_from_props(id, *props);
    if (!info || info->media_class != core::kStreamOutputAudio) {
        return;
    }
    // Bind and wait for the first info before the node is listed. A node that never answers is
    // left out at the deadline (fail closed).
    void* bound = pw_registry_bind(shell::registry_of(registry_), id, PW_TYPE_INTERFACE_Node,
                                   PW_VERSION_NODE, 0);
    if (bound == nullptr) {
        return;
    }
    auto entry = std::make_unique<Entry>();
    entry->owner = this;
    entry->id = id;
    entry->proxy.reset(static_cast<pw_proxy*>(bound));
    pw_proxy_add_object_listener(entry->proxy.get(), entry->hook.get(), &kNodeEvents, entry.get());
    entries_.emplace(id, std::move(entry));
}

void Lister::on_global_remove(std::uint32_t id) {
    // Each erase destroys an entry's hook before its proxy (member order in Entry).
    entries_.erase(id);
}

void Lister::on_core_done(std::uint32_t id, int seq) {
    if (id != PW_ID_CORE || seq != sync_seq_) {
        return;
    }
    first_done_ = true;
    if (!all_infos_seen()) {
        // A bound node has not delivered its first info yet: another round trip gives it time.
        if (!sync()) {
            failed_ = true;
            quit();
        }
        return;
    }
    quit();
}

void Lister::on_core_error(std::uint32_t id, int res) {
    if (id == PW_ID_CORE && res == -EPIPE) {
        failed_ = true;
        quit();
    }
}

void Lister::on_node_info(std::uint32_t id, const pw_node_info* info) {
    const auto entry = entries_.find(id);
    if (entry == entries_.end() || info == nullptr) {
        return;
    }
    Entry& node = *entry->second;
    node.info_seen = true;
    if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0 && info->props != nullptr) {
        // An unreadable info leaves the node out: node_from_props fails closed.
        node.info = shell::node_from_props(id, *info->props);
    }
}

void Lister::on_deadline() {
    timed_out_ = true;
    quit();
}

bool Lister::sync() {
    const int seq = pw_core_sync(connection_.get(), PW_ID_CORE, 0);
    if (seq < 0) {
        return false;
    }
    // The done event carries this exact value (it includes PipeWire's async marker bit).
    sync_seq_ = seq;
    return true;
}

bool Lister::all_infos_seen() const {
    return std::ranges::all_of(
        entries_ | std::views::values,
        [](const std::unique_ptr<Entry>& entry) { return entry->info_seen; });
}

void Lister::quit() const { static_cast<void>(pw_main_loop_quit(loop_.get())); }

} // namespace

namespace shell {

std::vector<AppStream> to_app_streams(std::vector<core::NodeInfo> nodes) {
    auto streams = nodes | std::views::filter([](const core::NodeInfo& node) {
                       return node.media_class == core::kStreamOutputAudio;
                   }) |
                   std::views::transform([](const core::NodeInfo& node) {
                       return AppStream{.app_name = node.app_name,
                                        .binary = node.binary,
                                        .node_name = node.node_name,
                                        .media_name = node.media_name,
                                        .serial = node.serial,
                                        .pid = node.pid};
                   }) |
                   std::ranges::to<std::vector>();
    std::ranges::sort(streams, std::ranges::less{}, [](const AppStream& stream) {
        return std::tie(stream.app_name, stream.serial);
    });
    return streams;
}

} // namespace shell

// ---------------------------------------------------------------------------------------------

std::expected<std::vector<AppStream>, Error> list_applications() {
    shell::init_pipewire_once();
    auto lister = std::make_unique<Lister>();
    if (auto ran = lister->run(); !ran) {
        return std::unexpected{ran.error()};
    }
    return shell::to_app_streams(lister->parsed_nodes());
}

} // namespace pwcap
