#ifndef PWCAP_TESTS_PIPEWIRE_PW_SESSION_HPP
#define PWCAP_TESTS_PIPEWIRE_PW_SESSION_HPP

// Test-only harness for the integration tests under tests/pipewire: the opt-in session gate, a temp
// dir, a tone WAV writer, child processes, a command runner and an independent graph oracle built
// from `pw-dump` and nlohmann_json. The oracle never uses the library's own registry code, so the
// tests do not judge the library with the library.
//
// Later plans extend this header (04-05 adds a link guard); the signatures below stay as they are.

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace pwcap::test {

using namespace std::chrono_literals;

// ---- small POSIX wrappers ---------------------------------------------------------------------

// Owns one file descriptor.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    ~UniqueFd() { reset(); }

    [[nodiscard]] int get() const noexcept { return fd_; }
    void reset() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_{-1};
};

// The argv shape posix_spawnp wants: pointers into `storage`, then a null.
[[nodiscard]] inline std::vector<char*>
null_terminated_pointers(std::vector<std::string>& storage) {
    std::vector<char*> pointers;
    pointers.reserve(storage.size() + 1);
    std::ranges::transform(storage, std::back_inserter(pointers),
                           [](std::string& entry) { return entry.data(); });
    pointers.push_back(nullptr);
    return pointers;
}

// A child process started with posix_spawnp, its standard output and error discarded. The
// destructor stops it if it is still running.
class Child {
public:
    Child() = default;

    explicit Child(const std::vector<std::string>& argv) {
        if (argv.empty()) {
            throw std::invalid_argument("Child needs a program name");
        }
        std::vector<std::string> storage = argv;
        std::vector<char*> pointers = null_terminated_pointers(storage);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        pid_t pid = 0;
        const int result =
            ::posix_spawnp(&pid, pointers.front(), &actions, nullptr, pointers.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (result != 0) {
            throw std::runtime_error("posix_spawnp failed for " + argv.front());
        }
        pid_ = pid;
    }

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept : pid_(std::exchange(other.pid_, -1)) {}
    Child& operator=(Child&& other) noexcept {
        if (this != &other) {
            terminate_and_wait();
            pid_ = std::exchange(other.pid_, -1);
        }
        return *this;
    }
    ~Child() { terminate_and_wait(); }

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    // SIGTERM, then waitpid. A child that ignores SIGTERM for 3 s gets SIGKILL.
    void terminate_and_wait() noexcept {
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, SIGTERM);
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        bool reaped = false;
        // A raw loop: waitpid has no timeout, so the wait polls against a deadline.
        while (!reaped && std::chrono::steady_clock::now() < deadline) {
            reaped = ::waitpid(pid_, &status, WNOHANG) == pid_;
            if (!reaped) {
                std::this_thread::sleep_for(5ms);
            }
        }
        if (!reaped) {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, &status, 0);
        }
        pid_ = -1;
    }

private:
    pid_t pid_{-1};
};

struct CommandResult {
    std::string out;
    int status{-1}; // exit code, or -1 when the program did not start or did not exit normally
};

// Runs a program with a pipe on its standard output (read to the end) and standard error
// discarded, then waits for it.
[[nodiscard]] inline CommandResult run_command(const std::vector<std::string>& argv) {
    CommandResult result;
    if (argv.empty()) {
        return result;
    }
    std::array<int, 2> ends{-1, -1};
    if (::pipe2(ends.data(), O_CLOEXEC) != 0) {
        return result;
    }
    UniqueFd read_end{ends[0]};
    UniqueFd write_end{ends[1]};

    std::vector<std::string> storage = argv;
    std::vector<char*> pointers = null_terminated_pointers(storage);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, write_end.get(), STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int spawned =
        ::posix_spawnp(&pid, pointers.front(), &actions, nullptr, pointers.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    write_end.reset();
    if (spawned != 0) {
        return result;
    }

    std::array<char, 4096> buffer{};
    // A raw loop: read() fills a fixed buffer until the child closes its end.
    for (;;) {
        const ssize_t count = ::read(read_end.get(), buffer.data(), buffer.size());
        if (count > 0) {
            result.out.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0 || errno != EINTR) {
            break;
        }
    }
    read_end.reset();

    int status = 0;
    if (::waitpid(pid, &status, 0) == pid && WIFEXITED(status)) {
        result.status = WEXITSTATUS(status);
    }
    return result;
}

// ---- opt-in gate ------------------------------------------------------------------------------

// Why an integration test must skip, or nothing when it may run: PWCAP_TEST_PIPEWIRE must be
// exactly "1" and `pw-cli info 0` must answer.
[[nodiscard]] inline std::optional<std::string> skip_reason() {
    const char* const flag = std::getenv("PWCAP_TEST_PIPEWIRE");
    if (flag == nullptr || std::string_view{flag} != "1") {
        return "PWCAP_TEST_PIPEWIRE is not set to 1";
    }
    if (run_command({"pw-cli", "info", "0"}).status != 0) {
        return "pw-cli info 0 did not answer";
    }
    return std::nullopt;
}

// ---- temp dir and tone WAV --------------------------------------------------------------------

// A fresh directory under the system temp dir, removed with everything in it.
class TempDir {
public:
    TempDir() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "pwcap-test-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = pattern;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

namespace detail {

template <typename T> void append_le(std::string& out, T value) {
    static_assert(std::endian::native == std::endian::little);
    const auto bytes = std::bit_cast<std::array<char, sizeof(T)>>(value);
    out.append(bytes.data(), bytes.size());
}

} // namespace detail

inline constexpr std::uint32_t kToneRate = 48000;
inline constexpr std::uint32_t kBytesPerSample = sizeof(std::int16_t);

// Writes a 48000 Hz s16le PCM WAV of a sine tone. `channels` is 1 or 2; `amplitude` is 0..1.
inline void write_tone_wav(const std::filesystem::path& path, double freq_hz, double seconds,
                           std::uint16_t channels, double amplitude) {
    if (channels != 1 && channels != 2) {
        throw std::invalid_argument("write_tone_wav: channels must be 1 or 2");
    }
    const auto frames = static_cast<std::uint32_t>(seconds * kToneRate);
    const std::uint32_t data_bytes = frames * channels * kBytesPerSample;

    std::string wav;
    wav.reserve(44 + data_bytes);
    wav.append("RIFF");
    detail::append_le<std::uint32_t>(wav, 36 + data_bytes);
    wav.append("WAVEfmt ");
    detail::append_le<std::uint32_t>(wav, 16);
    detail::append_le<std::uint16_t>(wav, 1); // PCM
    detail::append_le<std::uint16_t>(wav, channels);
    detail::append_le<std::uint32_t>(wav, kToneRate);
    detail::append_le<std::uint32_t>(wav, kToneRate * channels * kBytesPerSample);
    detail::append_le<std::uint16_t>(wav, static_cast<std::uint16_t>(channels * kBytesPerSample));
    detail::append_le<std::uint16_t>(wav, 16);
    wav.append("data");
    detail::append_le<std::uint32_t>(wav, data_bytes);

    const double step = 2.0 * std::numbers::pi * freq_hz / static_cast<double>(kToneRate);
    std::ranges::for_each(std::views::iota(std::uint32_t{0}, frames), [&](std::uint32_t frame) {
        const double value = amplitude * std::sin(step * static_cast<double>(frame));
        const auto sample = static_cast<std::int16_t>(std::lround(value * 32767.0));
        std::ranges::for_each(std::views::iota(std::uint16_t{0}, channels),
                              [&](std::uint16_t) { detail::append_le<std::int16_t>(wav, sample); });
    });

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(wav.data(), static_cast<std::streamsize>(wav.size()));
    if (!file) {
        throw std::runtime_error("write_tone_wav: cannot write " + path.string());
    }
}

// ---- graph oracle -----------------------------------------------------------------------------

struct GNode {
    std::uint32_t id{};
    std::uint64_t serial{};
    std::string name;
    std::string media_class;
    std::optional<bool> autoconnect;
};

struct GPort {
    std::uint32_t id{};
    std::uint32_t node{};
    bool output{};
    std::string channel;
    bool monitor{};
};

struct GLink {
    std::uint32_t id{};
    std::uint32_t out_port{};
    std::uint32_t in_port{};
    std::uint32_t out_node{};
    std::uint32_t in_node{};
};

struct Graph {
    // A plain value the tests fill and read directly; the lookup helpers below are conveniences.
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    std::vector<GNode> nodes;
    std::vector<GPort> ports;
    std::vector<GLink> links;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] const GNode* node_by_name(std::string_view name) const {
        const auto found = std::ranges::find(nodes, name, &GNode::name);
        return found == nodes.end() ? nullptr : &*found;
    }

    [[nodiscard]] const GNode* node_by_id(std::uint32_t id) const {
        const auto found = std::ranges::find(nodes, id, &GNode::id);
        return found == nodes.end() ? nullptr : &*found;
    }

    [[nodiscard]] const GPort* port_by_id(std::uint32_t id) const {
        const auto found = std::ranges::find(ports, id, &GPort::id);
        return found == ports.end() ? nullptr : &*found;
    }

    [[nodiscard]] std::vector<const GNode*> nodes_by_prefix(std::string_view prefix) const {
        return pointers_to(nodes, [&](const GNode& node) { return node.name.starts_with(prefix); });
    }

    [[nodiscard]] std::vector<const GPort*> ports_of(std::uint32_t node_id) const {
        return pointers_to(ports, [&](const GPort& port) { return port.node == node_id; });
    }

    [[nodiscard]] std::vector<const GLink*> links_into(std::uint32_t node_id) const {
        return pointers_to(links, [&](const GLink& link) { return link.in_node == node_id; });
    }

    [[nodiscard]] std::vector<const GLink*> links_out_of(std::uint32_t node_id) const {
        return pointers_to(links, [&](const GLink& link) { return link.out_node == node_id; });
    }

private:
    template <typename T, typename Pred>
    [[nodiscard]] static std::vector<const T*> pointers_to(const std::vector<T>& items, Pred pred) {
        std::vector<const T*> found;
        std::ranges::transform(items | std::views::filter(pred), std::back_inserter(found),
                               [](const T& item) { return &item; });
        return found;
    }
};

namespace detail {

using Json = nlohmann::json;

// The member of a JSON object, or null when the value is not an object or has no such key.
[[nodiscard]] inline const Json* member(const Json& object, const std::string& key) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

// pw-dump writes numbers as numbers; accept their string form too.
[[nodiscard]] inline std::optional<std::uint64_t> as_uint(const Json* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (value->is_number_integer()) {
        const auto number = value->get<std::int64_t>();
        return number < 0 ? std::nullopt
                          : std::optional<std::uint64_t>{static_cast<std::uint64_t>(number)};
    }
    if (value->is_string()) {
        const auto text = value->get<std::string>();
        const char* const first = std::to_address(text.begin());
        const char* const last = std::to_address(text.end());
        std::uint64_t number = 0;
        const auto [end, error] = std::from_chars(first, last, number);
        if (error == std::errc{} && end == last) {
            return number;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<std::uint32_t> as_u32(const Json* value) {
    const auto number = as_uint(value);
    if (!number || *number > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(*number);
}

[[nodiscard]] inline std::optional<bool> as_bool(const Json* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (value->is_boolean()) {
        return value->get<bool>();
    }
    if (value->is_string()) {
        const auto text = value->get<std::string>();
        if (text == "true") {
            return true;
        }
        if (text == "false") {
            return false;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string as_string(const Json* value) {
    return value != nullptr && value->is_string() ? value->get<std::string>() : std::string{};
}

[[nodiscard]] inline const Json& props_of(const Json& object) {
    static const Json empty = Json::object();
    const Json* info = member(object, "info");
    const Json* props = info == nullptr ? nullptr : member(*info, "props");
    return props == nullptr ? empty : *props;
}

inline void add_node(Graph& graph, const Json& object) {
    const auto id = as_u32(member(object, "id"));
    if (!id) {
        return;
    }
    const Json& props = props_of(object);
    GNode node;
    node.id = *id;
    node.serial = as_uint(member(props, "object.serial")).value_or(0);
    node.name = as_string(member(props, "node.name"));
    node.media_class = as_string(member(props, "media.class"));
    node.autoconnect = as_bool(member(props, "node.autoconnect"));
    graph.nodes.push_back(std::move(node));
}

inline void add_port(Graph& graph, const Json& object) {
    const auto id = as_u32(member(object, "id"));
    const Json& props = props_of(object);
    const auto node = as_u32(member(props, "node.id"));
    if (!id || !node) {
        return;
    }
    const Json* info = member(object, "info");
    std::string direction = as_string(info == nullptr ? nullptr : member(*info, "direction"));
    if (direction.empty()) {
        direction = as_string(member(props, "port.direction")) == "out" ? "output" : "input";
    }
    GPort port;
    port.id = *id;
    port.node = *node;
    port.output = direction == "output";
    port.channel = as_string(member(props, "audio.channel"));
    port.monitor = as_bool(member(props, "port.monitor")).value_or(false);
    graph.ports.push_back(std::move(port));
}

inline void add_link(Graph& graph, const Json& object) {
    const auto id = as_u32(member(object, "id"));
    const Json* info = member(object, "info");
    if (!id || info == nullptr) {
        return;
    }
    const auto out_node = as_u32(member(*info, "output-node-id"));
    const auto out_port = as_u32(member(*info, "output-port-id"));
    const auto in_node = as_u32(member(*info, "input-node-id"));
    const auto in_port = as_u32(member(*info, "input-port-id"));
    if (!out_node || !out_port || !in_node || !in_port) {
        return;
    }
    graph.links.push_back(GLink{.id = *id,
                                .out_port = *out_port,
                                .in_port = *in_port,
                                .out_node = *out_node,
                                .in_node = *in_node});
}

} // namespace detail

// Reads the whole graph from `pw-dump`. An empty Graph means pw-dump failed or printed garbage.
[[nodiscard]] inline Graph read_graph() {
    Graph graph;
    const CommandResult dump = run_command({"pw-dump"});
    if (dump.status != 0) {
        return graph;
    }
    const detail::Json parsed = detail::Json::parse(dump.out, nullptr, false);
    if (!parsed.is_array()) {
        return graph;
    }
    std::ranges::for_each(parsed, [&](const detail::Json& object) {
        const std::string type = detail::as_string(detail::member(object, "type"));
        if (type == "PipeWire:Interface:Node") {
            detail::add_node(graph, object);
        } else if (type == "PipeWire:Interface:Port") {
            detail::add_port(graph, object);
        } else if (type == "PipeWire:Interface:Link") {
            detail::add_link(graph, object);
        }
    });
    return graph;
}

// Polls `predicate` every `step` until it returns true or `timeout` passes. The predicate is
// checked once more at the deadline.
template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate, std::chrono::milliseconds timeout,
                              std::chrono::milliseconds step = 20ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // A raw loop: the poll interval and the deadline have no algorithm equivalent.
    for (;;) {
        if (predicate()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(step);
    }
}

} // namespace pwcap::test

#endif // PWCAP_TESTS_PIPEWIRE_PW_SESSION_HPP
