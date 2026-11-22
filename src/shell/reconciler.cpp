#include "shell/reconciler.hpp"

#include "core/planner.hpp"

#include <spa/utils/result.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <format>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace pwcap::shell {

namespace {

[[nodiscard]] std::optional<std::string_view> lookup(const spa_dict& dict, const char* key) {
    const char* value = spa_dict_lookup(&dict, key);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string_view{value};
}

[[nodiscard]] std::string text_or_empty(const spa_dict& dict, const char* key) {
    const auto value = lookup(dict, key);
    return value ? std::string{*value} : std::string{};
}

// The whole string must be a number of type T: no sign for unsigned types, no spaces, no
// trailing characters, no overflow.
template <std::integral T> [[nodiscard]] std::optional<T> parse_number(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    T value{};
    const auto [end, error] =
        std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), value);
    if (error != std::errc{} || end != std::to_address(text.end())) {
        return std::nullopt;
    }
    return value;
}

template <std::integral T>
[[nodiscard]] std::optional<T> number_at(const spa_dict& dict, const char* key) {
    const auto text = lookup(dict, key);
    if (!text) {
        return std::nullopt;
    }
    return parse_number<T>(*text);
}

} // namespace

std::optional<core::NodeInfo> node_from_props(std::uint32_t id, const spa_dict& props) {
    const auto serial = number_at<core::Serial>(props, PW_KEY_OBJECT_SERIAL);
    if (!serial) {
        return std::nullopt;
    }
    core::NodeInfo node;
    node.id = id;
    node.serial = *serial;
    node.node_name = text_or_empty(props, PW_KEY_NODE_NAME);
    node.app_name = text_or_empty(props, PW_KEY_APP_NAME);
    node.binary = text_or_empty(props, PW_KEY_APP_PROCESS_BINARY);
    node.media_class = text_or_empty(props, PW_KEY_MEDIA_CLASS);
    node.media_name = text_or_empty(props, PW_KEY_MEDIA_NAME);
    node.pid = number_at<std::int32_t>(props, PW_KEY_APP_PROCESS_ID);
    return node;
}

std::optional<core::PortInfo> port_from_props(std::uint32_t id, const spa_dict& props) {
    const auto node_id = number_at<std::uint32_t>(props, PW_KEY_NODE_ID);
    const auto direction_text = lookup(props, PW_KEY_PORT_DIRECTION);
    if (!node_id || !direction_text) {
        return std::nullopt;
    }
    core::Direction direction{};
    if (*direction_text == "in") {
        direction = core::Direction::Input;
    } else if (*direction_text == "out") {
        direction = core::Direction::Output;
    } else {
        return std::nullopt;
    }
    core::PortInfo port;
    port.node_id = *node_id;
    port.port_id = id;
    port.direction = direction;
    port.channel = text_or_empty(props, PW_KEY_AUDIO_CHANNEL);
    port.monitor = lookup(props, PW_KEY_PORT_MONITOR) == std::string_view{"true"};
    return port;
}

std::optional<core::LinkInfo> link_from_props(const spa_dict& props) {
    const auto output = number_at<std::uint32_t>(props, PW_KEY_LINK_OUTPUT_PORT);
    const auto input = number_at<std::uint32_t>(props, PW_KEY_LINK_INPUT_PORT);
    if (!output || !input) {
        return std::nullopt;
    }
    return core::LinkInfo{.output_port_id = *output, .input_port_id = *input};
}

// ---------------------------------------------------------------------------------------------
// Entries. Each is heap-allocated and never moves: PipeWire keeps the address of its spa_hook and
// of the callback data. Handles come before the hooks that listen to them.

struct Reconciler::BoundNode {
    ProxyPtr proxy;
    HookGuard hook;
};

struct Reconciler::NodeEntry {
    Reconciler* owner{};
    std::uint32_t id{};
    core::NodeInfo info;
    // In the snapshot. A bound node becomes ready only with its first info that carries props.
    bool ready{false};
    // The first bound info arrived. True from the start for nodes that are not bound.
    bool info_seen{false};
    BoundNode bound;
};

struct Reconciler::OwnedLink {
    Reconciler* owner{};
    core::LinkInfo key;
    // Requested by us and not yet announced by the registry.
    bool pending{true};
    // PipeWire rejected the request. The entry waits in Reconciler::failed_links_ and every later
    // event of its proxy is ignored.
    bool failed{false};
    ProxyPtr proxy;
    HookGuard hook;
};

// Static trampolines from PipeWire's C callbacks to the Reconciler. A friend of Reconciler.
struct Callbacks {
    static void registry_global(void* data, std::uint32_t id, std::uint32_t /*permissions*/,
                                const char* type, std::uint32_t /*version*/,
                                const spa_dict* props) {
        static_cast<Reconciler*>(data)->on_global(id, type, props);
    }
    static void registry_global_remove(void* data, std::uint32_t id) {
        static_cast<Reconciler*>(data)->on_global_remove(id);
    }
    static void core_done(void* data, std::uint32_t id, int seq) {
        static_cast<Reconciler*>(data)->on_core_done(id, seq);
    }
    static void core_error(void* data, std::uint32_t id, int /*seq*/, int res,
                           const char* message) {
        static_cast<Reconciler*>(data)->on_core_error(id, res, message);
    }
    static void node_info(void* data, const pw_node_info* info) {
        auto* entry = static_cast<Reconciler::NodeEntry*>(data);
        entry->owner->on_node_info(entry->id, info);
    }
    static void link_removed(void* data) {
        auto* entry = static_cast<Reconciler::OwnedLink*>(data);
        if (entry->failed) {
            return;
        }
        // Copy first: the handler erases the entry that owns the key.
        const core::LinkInfo key = entry->key;
        entry->owner->on_link_removed(key, entry);
    }
    static void link_error(void* data, int /*seq*/, int res, const char* message) {
        auto* entry = static_cast<Reconciler::OwnedLink*>(data);
        if (entry->failed) {
            return;
        }
        const core::LinkInfo key = entry->key;
        entry->owner->on_link_error(key, entry, res, message);
    }
};

namespace {

// The event tables are built by assignment: a partial designated initialiser of these C structs
// would trip -Wmissing-designated-field-initializers.
constexpr pw_registry_events make_registry_events() noexcept {
    pw_registry_events events{};
    events.version = PW_VERSION_REGISTRY_EVENTS;
    events.global = &Callbacks::registry_global;
    events.global_remove = &Callbacks::registry_global_remove;
    return events;
}

constexpr pw_core_events make_core_events() noexcept {
    pw_core_events events{};
    events.version = PW_VERSION_CORE_EVENTS;
    events.done = &Callbacks::core_done;
    events.error = &Callbacks::core_error;
    return events;
}

constexpr pw_node_events make_node_events() noexcept {
    pw_node_events events{};
    events.version = PW_VERSION_NODE_EVENTS;
    events.info = &Callbacks::node_info;
    return events;
}

constexpr pw_proxy_events make_link_events() noexcept {
    pw_proxy_events events{};
    events.version = PW_VERSION_PROXY_EVENTS;
    events.removed = &Callbacks::link_removed;
    events.error = &Callbacks::link_error;
    return events;
}

constexpr pw_registry_events kRegistryEvents = make_registry_events();
constexpr pw_core_events kCoreEvents = make_core_events();
constexpr pw_node_events kNodeEvents = make_node_events();
constexpr pw_proxy_events kLinkEvents = make_link_events();

[[nodiscard]] std::string link_name(const core::LinkInfo& link) {
    return std::format("{}->{}", link.output_port_id, link.input_port_id);
}

} // namespace

// ---------------------------------------------------------------------------------------------

std::expected<std::unique_ptr<Reconciler>, pwcap::Error>
Reconciler::create(pw_thread_loop* loop, pw_core* core, ReconcilerConfig config) {
    auto reconciler = std::make_unique<Reconciler>(PrivateTag{}, loop, core, std::move(config));
    pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (registry == nullptr) {
        return std::unexpected{pwcap::Error::NoPipeWire};
    }
    reconciler->registry_ = make_registry_handle(registry);
    pw_registry_add_listener(registry, reconciler->registry_hook_.get(), &kRegistryEvents,
                             reconciler.get());
    pw_core_add_listener(core, reconciler->core_hook_.get(), &kCoreEvents, reconciler.get());
    // The initial round trip: its done event means every existing global has been announced.
    reconciler->schedule_reconcile();
    return reconciler;
}

Reconciler::Reconciler(PrivateTag /*key*/, pw_thread_loop* loop, pw_core* core,
                       ReconcilerConfig config)
    : loop_{loop}, core_{core}, selection_{std::move(config.selection)},
      reconnect_{config.reconnect}, diagnostics_{config.diagnostics} {}

Reconciler::~Reconciler() {
    // Our links first, so they disappear from the graph. Then the bound nodes; each entry
    // removes its hook before it destroys its proxy. The members after that are destroyed in
    // reverse declaration order: the hooks, then the registry.
    owned_links_.clear();
    nodes_.clear();
}

std::expected<void, pwcap::Error> Reconciler::start(std::uint32_t capture_node_id,
                                                    std::chrono::milliseconds timeout) {
    capture_node_id_ = capture_node_id;
    timespec deadline{};
    const auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout).count();
    if (pw_thread_loop_get_time(loop_, &deadline, timeout_ns) < 0) {
        return std::unexpected{pwcap::Error::NoPipeWire};
    }
    // The condition is re-checked on every wake-up; every registry change is followed by a core
    // done event, and that handler signals.
    // A raw loop: the wait primitive releases the lock, and no algorithm expresses that.
    while (!ready_to_start()) {
        if (lost_connection_ || pw_thread_loop_timed_wait_full(loop_, &deadline) != 0) {
            return std::unexpected{pwcap::Error::NoPipeWire};
        }
    }
    started_ = true;
    if (reconnect_) {
        // Waiting for an application is a normal state: reconcile() reports it and open succeeds.
        reconcile();
        return {};
    }

    // reconnect=false: open needs a source, and the streams selected now are the only ones this
    // capture will ever follow.
    const std::vector<core::NodeId> sources = core::select_sources(build_snapshot(), selection_);
    if (sources.empty()) {
        return std::unexpected{pwcap::Error::NoMatchingSource};
    }
    pinned_ = nodes_ | std::views::values |
              std::views::filter([&](const std::unique_ptr<NodeEntry>& entry) {
                  return entry->ready && std::ranges::contains(sources, entry->id);
              }) |
              std::views::transform(
                  [](const std::unique_ptr<NodeEntry>& entry) { return entry->info.serial; }) |
              std::ranges::to<std::vector>();
    reconcile();
    // Every requested link must be announced by the registry, or have failed.
    // A raw loop: same wait primitive as above.
    while (!first_links_settled()) {
        if (lost_connection_ || pw_thread_loop_timed_wait_full(loop_, &deadline) != 0) {
            return std::unexpected{pwcap::Error::LinkFailed};
        }
    }
    if (link_failures_ != 0) {
        return std::unexpected{pwcap::Error::LinkFailed};
    }
    return {};
}

bool Reconciler::first_links_settled() const {
    return link_failures_ != 0 || std::ranges::none_of(owned_links_ | std::views::values,
                                                       [](const std::unique_ptr<OwnedLink>& owned) {
                                                           return owned->pending;
                                                       });
}

bool Reconciler::ready_to_start() const {
    if (!initial_done_ || !nodes_.contains(capture_node_id_)) {
        return false;
    }
    const bool all_info_seen = std::ranges::all_of(
        nodes_ | std::views::values,
        [](const std::unique_ptr<NodeEntry>& entry) { return entry->info_seen; });
    return all_info_seen && capture_inputs().size() >= kExpectedCaptureInputs;
}

void Reconciler::post(std::string message) const {
    if (diagnostics_ != nullptr) {
        diagnostics_->post(std::move(message));
    }
}

// ---- registry --------------------------------------------------------------------------------

void Reconciler::on_global(std::uint32_t id, const char* type, const spa_dict* props) {
    if (type == nullptr || props == nullptr) {
        return;
    }
    const std::string_view type_name{type};
    if (type_name == PW_TYPE_INTERFACE_Node) {
        auto info = node_from_props(id, *props);
        if (!info) {
            return;
        }
        nodes_.erase(id);
        auto entry = std::make_unique<NodeEntry>();
        entry->owner = this;
        entry->id = id;
        const bool bind = info->media_class == core::kStreamOutputAudio;
        entry->info = std::move(*info);
        entry->ready = !bind;
        entry->info_seen = !bind;
        if (bind) {
            // A stream's registry props lack media.name and the binary: bind it and wait for its
            // first info before it may be matched.
            void* bound = pw_registry_bind(registry_of(registry_), id, PW_TYPE_INTERFACE_Node,
                                           PW_VERSION_NODE, 0);
            if (bound == nullptr) {
                return; // not mirrored: fails closed
            }
            entry->bound.proxy.reset(static_cast<pw_proxy*>(bound));
            pw_proxy_add_object_listener(entry->bound.proxy.get(), entry->bound.hook.get(),
                                         &kNodeEvents, entry.get());
        }
        nodes_.emplace(id, std::move(entry));
    } else if (type_name == PW_TYPE_INTERFACE_Port) {
        const auto port = port_from_props(id, *props);
        if (!port) {
            return;
        }
        ports_.insert_or_assign(id, *port);
    } else if (type_name == PW_TYPE_INTERFACE_Link) {
        const auto link = link_from_props(*props);
        if (!link) {
            return;
        }
        links_.insert_or_assign(id, *link);
        if (const auto owned = owned_links_.find(*link); owned != owned_links_.end()) {
            owned->second->pending = false;
        }
    } else {
        return;
    }
    registry_changed();
    schedule_reconcile();
}

void Reconciler::on_global_remove(std::uint32_t id) {
    // A registry link that goes away takes our bookkeeping for it along: the server removes the
    // link when one of its ports dies, and the proxy's own removed event may come later.
    if (const auto link = links_.find(id); link != links_.end()) {
        if (const auto owned = owned_links_.find(link->second);
            owned != owned_links_.end() && !owned->second->pending) {
            owned_links_.erase(owned);
        }
    }
    // Ids are recycled: a link we asked for between a port's old and new life must not
    // be taken for a link between the new ports.
    if (ports_.contains(id)) {
        std::erase_if(owned_links_, [id](const auto& entry) {
            return entry.second->pending &&
                   (entry.first.output_port_id == id || entry.first.input_port_id == id);
        });
    }
    // Each erase destroys an entry's hook before its proxy (member order).
    const std::size_t removed = nodes_.erase(id) + ports_.erase(id) + links_.erase(id);
    if (removed != 0) {
        registry_changed();
        schedule_reconcile();
    }
}

void Reconciler::registry_changed() { failed_keys_.clear(); }

// ---- core ------------------------------------------------------------------------------------

void Reconciler::schedule_reconcile() {
    if (sync_outstanding_) {
        dirty_ = true;
        return;
    }
    const int res = pw_core_sync(core_, PW_ID_CORE, 0);
    if (res < 0) {
        post(std::format("NoPipeWire: pw_core_sync failed: {}", spa_strerror(res)));
        return;
    }
    // The done event carries this exact value (it includes PipeWire's async marker bit).
    sync_seq_ = res;
    sync_outstanding_ = true;
    dirty_ = false;
}

void Reconciler::on_core_done(std::uint32_t id, int seq) {
    if (id == PW_ID_CORE && sync_outstanding_ && seq == sync_seq_) {
        sync_outstanding_ = false;
        initial_done_ = true;
        reconcile();
        if (dirty_) {
            schedule_reconcile();
        }
    }
    pw_thread_loop_signal(loop_, false);
}

void Reconciler::on_core_error(std::uint32_t id, int res, const char* message) {
    // The text varies with the PipeWire version; the diagnostic is fixed.
    static_cast<void>(message);
    if (id == PW_ID_CORE && res == -EPIPE && !lost_connection_) {
        lost_connection_ = true;
        linked_.store(false);
        post("Stopped: lost the connection to PipeWire");
        pw_thread_loop_signal(loop_, false);
    }
}

// ---- node ------------------------------------------------------------------------------------

void Reconciler::on_node_info(std::uint32_t id, const pw_node_info* info) {
    const auto entry = nodes_.find(id);
    if (entry == nodes_.end() || info == nullptr) {
        return;
    }
    NodeEntry& node = *entry->second;
    node.info_seen = true;
    if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0 && info->props != nullptr) {
        if (auto parsed = node_from_props(id, *info->props)) {
            node.info = std::move(*parsed);
            node.ready = true;
        } else {
            node.ready = false; // unreadable props: fail closed
        }
        schedule_reconcile();
    }
    pw_thread_loop_signal(loop_, false);
}

// ---- links -----------------------------------------------------------------------------------

void Reconciler::on_link_removed(const core::LinkInfo& key, const OwnedLink* entry) {
    // Only the entry that raised the event: a newer request for the same ports must stay.
    if (const auto owned = owned_links_.find(key);
        owned != owned_links_.end() && owned->second.get() == entry) {
        owned_links_.erase(owned);
    }
    schedule_reconcile();
}

void Reconciler::on_link_error(const core::LinkInfo& key, const OwnedLink* entry, int res,
                               const char* message) {
    post(std::format("LinkFailed: {}: {}", link_name(key),
                     message != nullptr ? message : spa_strerror(res)));
    ++link_failures_;
    if (const auto owned = owned_links_.find(key);
        owned != owned_links_.end() && owned->second.get() == entry && owned->second->pending) {
        // No retry here and no new reconcile: a rejected request would be rejected again. The next
        // registry change plans again. The entry is parked, not destroyed: this runs inside its
        // own proxy's event.
        owned->second->failed = true;
        failed_keys_.insert(key);
        failed_links_.push_back(std::move(owned->second));
        owned_links_.erase(owned);
    }
    pw_thread_loop_signal(loop_, false);
}

// ---- reconcile -------------------------------------------------------------------------------

core::GraphSnapshot Reconciler::build_snapshot() const {
    core::GraphSnapshot snapshot;
    snapshot.nodes =
        nodes_ | std::views::values |
        std::views::filter([](const std::unique_ptr<NodeEntry>& entry) { return entry->ready; }) |
        std::views::transform([](const std::unique_ptr<NodeEntry>& entry) { return entry->info; }) |
        std::ranges::to<std::vector>();
    snapshot.ports = ports_ | std::views::values | std::ranges::to<std::vector>();
    snapshot.links = links_ | std::views::values | std::ranges::to<std::vector>();
    return snapshot;
}

std::vector<core::PortId> Reconciler::capture_inputs() const {
    return ports_ | std::views::values | std::views::filter([this](const core::PortInfo& port) {
               return port.node_id == capture_node_id_ &&
                      port.direction == core::Direction::Input && !port.monitor;
           }) |
           std::views::transform([](const core::PortInfo& port) { return port.port_id; }) |
           std::ranges::to<std::vector>();
}

core::GraphSnapshot Reconciler::pinned_view(core::GraphSnapshot snapshot) const {
    // reconnect=false: an application stream that was not selected at open does not exist for the
    // planner, so a restarted stream (new object.serial) is never linked. Its ports and links stay,
    // and a link from it into our capture is removed like any foreign link.
    std::erase_if(snapshot.nodes, [this](const core::NodeInfo& node) {
        return node.media_class == core::kStreamOutputAudio &&
               !std::ranges::contains(pinned_, node.serial);
    });
    return snapshot;
}

bool Reconciler::pinned_stream_present() const {
    return std::ranges::any_of(nodes_ | std::views::values,
                               [this](const std::unique_ptr<NodeEntry>& entry) {
                                   return std::ranges::contains(pinned_, entry->info.serial);
                               });
}

void Reconciler::track_sources(const std::vector<core::NodeId>& sources) {
    if (!reconnect_) {
        if (!stopped_posted_ && !pinned_stream_present()) {
            stopped_posted_ = true;
            post("Stopped: the selected stream ended and reconnect is off");
        }
        return;
    }
    // One message per transition, never one per reconcile.
    const SourceState now = sources.empty() ? SourceState::Absent : SourceState::Present;
    const SourceState before = std::exchange(source_state_, now);
    if (now == before) {
        return;
    }
    if (now == SourceState::Absent) {
        post("NoMatchingSource: no application stream matches the include rule; waiting");
    } else if (before == SourceState::Absent) {
        post("pwcap: matching stream found; linking");
    }
}

void Reconciler::reconcile() {
    if (!started_ || lost_connection_) {
        return; // the registry is not complete yet, or the daemon is gone
    }
    // Rejected link requests parked by on_link_error. This runs from a core done event or from
    // start(), never inside a proxy's own event, so destroying them is safe.
    failed_links_.clear();

    const std::vector<core::PortId> inputs = capture_inputs();
    core::GraphSnapshot snapshot = build_snapshot();
    if (!reconnect_) {
        snapshot = pinned_view(std::move(snapshot));
    }

    // linked(): a registry link, pending links excluded, from a selected source to a capture input.
    const std::vector<core::NodeId> sources = core::select_sources(snapshot, selection_);
    track_sources(sources);
    const bool now_linked = std::ranges::any_of(snapshot.links, [&](const core::LinkInfo& link) {
        const auto output =
            std::ranges::find(snapshot.ports, link.output_port_id, &core::PortInfo::port_id);
        return output != snapshot.ports.end() && !output->monitor &&
               std::ranges::contains(sources, output->node_id) &&
               std::ranges::contains(inputs, link.input_port_id);
    });
    linked_.store(now_linked);

    // Plan against the registry plus the links we asked for and the registry has not announced.
    // Without them, the done event of a sync sent before a link request would create it twice.
    std::ranges::for_each(owned_links_ | std::views::values, [&](const auto& owned) {
        if (owned->pending && !std::ranges::contains(snapshot.links, owned->key)) {
            snapshot.links.push_back(owned->key);
        }
    });

    const core::LinkPlan plan = core::plan_links(snapshot, selection_, inputs);
    std::ranges::for_each(plan.to_remove,
                          [this](const core::LinkInfo& link) { apply_remove(link); });
    std::ranges::for_each(plan.to_add, [this](const core::LinkInfo& link) { apply_add(link); });
}

void Reconciler::apply_remove(const core::LinkInfo& link) {
    if (owned_links_.erase(link) != 0) {
        return; // destroying our proxy removes the link
    }
    const auto registered = std::ranges::find(links_ | std::views::values, link);
    if (registered.base() == links_.end()) {
        return; // already gone
    }
    const int res = pw_registry_destroy(registry_of(registry_), registered.base()->first);
    if (res < 0) {
        post(std::format("LinkFailed: remove {}: {}", link_name(link), spa_strerror(res)));
    }
}

void Reconciler::apply_add(const core::LinkInfo& link) {
    if (owned_links_.contains(link) || failed_keys_.contains(link)) {
        return; // already requested, or rejected and not retried until the registry changes
    }
    const std::string output = std::to_string(link.output_port_id);
    const std::string input = std::to_string(link.input_port_id);
    // Built from a dict, not pw_properties_new: the varargs form is rejected by the lint policy.
    const std::array<spa_dict_item, 3> items{{
        {.key = PW_KEY_LINK_OUTPUT_PORT, .value = output.c_str()},
        {.key = PW_KEY_LINK_INPUT_PORT, .value = input.c_str()},
        {.key = PW_KEY_OBJECT_LINGER, .value = "false"},
    }};
    const spa_dict dict{
        .flags = 0, .n_items = static_cast<std::uint32_t>(items.size()), .items = items.data()};
    const PropertiesPtr props{pw_properties_new_dict(&dict)};
    if (!props) {
        ++link_failures_;
        post(std::format("LinkFailed: {}: out of memory", link_name(link)));
        return;
    }
    // The only place in src/shell that creates a link (SAFE-03): only plan_links output arrives
    // here. The links die with this client (object.linger=false). create_object borrows the
    // dict; `props` frees it.
    void* created = pw_core_create_object(core_, "link-factory", PW_TYPE_INTERFACE_Link,
                                          PW_VERSION_LINK, &props->dict, 0);
    if (created == nullptr) {
        ++link_failures_;
        failed_keys_.insert(link);
        post(std::format("LinkFailed: {}: {}", link_name(link), spa_strerror(-errno)));
        return;
    }
    auto entry = std::make_unique<OwnedLink>();
    entry->owner = this;
    entry->key = link;
    entry->proxy.reset(static_cast<pw_proxy*>(created));
    pw_proxy_add_listener(entry->proxy.get(), entry->hook.get(), &kLinkEvents, entry.get());
    owned_links_.insert_or_assign(link, std::move(entry));
}

} // namespace pwcap::shell
