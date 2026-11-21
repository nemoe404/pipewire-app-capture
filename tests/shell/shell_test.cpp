// Tests for the shell that need no PipeWire session: the open() error mapping, the RAII handles,
// the property parsers, and the subprocess silence checks on the error paths (API-02, D-05).
//
// Silence is always judged in a child process: stdio buffering inside the gtest process could
// otherwise hide a write. The child is tests/shell/capture_probe; a control run that writes one
// byte to each standard stream proves the redirection really captures.

#include "pwcap/capture.hpp"
#include "shell/pw_handles.hpp"
#include "shell/reconciler.hpp"

#include <gtest/gtest.h>

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

extern char** environ; // NOLINT(readability-redundant-declaration,misc-use-internal-linkage)

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

// ---- subprocess helper -----------------------------------------------------------------------

// A scratch directory removed on scope exit.
class TempDir {
public:
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "pwcap-shell-XXXXXX").string();
        if (mkdtemp(pattern.data()) != nullptr) {
            path_ = pattern;
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    [[nodiscard]] const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

struct ProbeRun {
    bool spawned{false};
    bool timed_out{false};
    int exit_status{-1};
    std::uintmax_t out_bytes{0};
    std::uintmax_t err_bytes{0};
    std::chrono::milliseconds elapsed{0};
};

// The argv/envp shape posix_spawn wants: pointers into `storage`, then a null.
[[nodiscard]] std::vector<char*> null_terminated_pointers(std::vector<std::string>& storage) {
    std::vector<char*> pointers;
    pointers.reserve(storage.size() + 1);
    std::ranges::transform(storage, std::back_inserter(pointers),
                           [](std::string& entry) { return entry.data(); });
    pointers.push_back(nullptr);
    return pointers;
}

[[nodiscard]] std::uintmax_t size_of(const fs::path& file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    return error ? 0 : size;
}

// Runs the capture probe with fds 1 and 2 redirected to two fresh files. The environment is a
// copy of the caller's, with PIPEWIRE_REMOTE replaced when given. Waits at most 10 s.
[[nodiscard]] ProbeRun run_probe(const std::vector<std::string>& args,
                                 const std::optional<std::string>& pipewire_remote) {
    ProbeRun run;
    const TempDir dir;
    const fs::path out_file = dir.path() / "stdout.bin";
    const fs::path err_file = dir.path() / "stderr.bin";

    std::vector<std::string> env_storage;
    // The environment is a null-terminated array of C strings; reading it needs pointer steps.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    for (char* const* entry = environ; *entry != nullptr; ++entry) {
        const std::string_view text{*entry};
        if (!pipewire_remote || !text.starts_with("PIPEWIRE_REMOTE=")) {
            env_storage.emplace_back(text);
        }
    }
    if (pipewire_remote) {
        env_storage.push_back("PIPEWIRE_REMOTE=" + *pipewire_remote);
    }
    std::vector<char*> envp = null_terminated_pointers(env_storage);

    std::vector<std::string> argv_storage{PWCAP_CAPTURE_PROBE};
    argv_storage.insert(argv_storage.end(), args.begin(), args.end());
    std::vector<char*> argv = null_terminated_pointers(argv_storage);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, out_file.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err_file.c_str(),
                                     O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    pid_t pid = 0;
    const auto start = std::chrono::steady_clock::now();
    const int spawn_result =
        posix_spawn(&pid, PWCAP_CAPTURE_PROBE, &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_result != 0) {
        return run;
    }
    run.spawned = true;

    int status = 0;
    const auto deadline = start + 10s;
    // A raw loop: waitpid has no timeout, so the wait polls against a deadline.
    for (;;) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            run.timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(5ms);
    }
    run.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    run.exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    run.out_bytes = size_of(out_file);
    run.err_bytes = size_of(err_file);
    return run;
}

// ---- spa_dict helper -------------------------------------------------------------------------

using Prop = std::pair<const char*, const char*>;

// Owns the items a spa_dict points at. Neither copyable nor movable: the dict holds an address.
class Props {
public:
    Props(std::initializer_list<Prop> props) {
        std::ranges::transform(props, std::back_inserter(items_), [](const Prop& prop) {
            return spa_dict_item{.key = prop.first, .value = prop.second};
        });
        dict_ = spa_dict{.flags = 0,
                         .n_items = static_cast<std::uint32_t>(items_.size()),
                         .items = items_.data()};
    }
    Props(const Props&) = delete;
    Props& operator=(const Props&) = delete;
    Props(Props&&) = delete;
    Props& operator=(Props&&) = delete;
    ~Props() = default;
    [[nodiscard]] const spa_dict& dict() const { return dict_; }

private:
    std::vector<spa_dict_item> items_;
    spa_dict dict_{};
};

// The value of `value`, or a test-failing exception. Keeps the optional checks in one place.
template <typename T> [[nodiscard]] const T& require(const std::optional<T>& value) {
    if (!value.has_value()) {
        throw std::logic_error("expected the optional to hold a value");
    }
    return *value;
}

[[nodiscard]] pwcap::Sink discarding_sink() {
    return [](std::span<const std::byte> /*pcm*/) {};
}

// ---- compile-time facts ----------------------------------------------------------------------

static_assert(pwcap::to_string(pwcap::Error::NoPipeWire) == "NoPipeWire");
static_assert(std::to_underlying(pwcap::Error::InvalidOptions) ==
              std::to_underlying(pwcap::Error::Stopped) + 1);
static_assert(std::is_same_v<std::underlying_type_t<pwcap::Error>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<pwcap::SampleFormat>, std::uint8_t>);
static_assert(!std::is_copy_constructible_v<pwcap::Capture>);
static_assert(std::is_nothrow_move_constructible_v<pwcap::Capture>);

} // namespace

// ---- OpenErrors ------------------------------------------------------------------------------

TEST(OpenErrors, ProbeControlBytesAreCaptured) {
    const ProbeRun run = run_probe({"control"}, std::nullopt);
    ASSERT_TRUE(run.spawned);
    EXPECT_FALSE(run.timed_out);
    EXPECT_EQ(run.exit_status, 0);
    EXPECT_EQ(run.out_bytes, 1U);
    EXPECT_EQ(run.err_bytes, 1U);
}

TEST(OpenErrors, NoDaemonSubprocessIsNoPipeWireAndSilent) {
    const TempDir dir;
    ASSERT_FALSE(dir.path().empty());
    // A path that cannot exist: a fresh directory with nothing in it. The host daemon is untouched.
    const std::string missing_socket = (dir.path() / "no-such-socket").string();
    ASSERT_FALSE(fs::exists(missing_socket));
    const ProbeRun run = run_probe({"no-daemon"}, missing_socket);
    ASSERT_TRUE(run.spawned);
    EXPECT_FALSE(run.timed_out);
    EXPECT_EQ(run.exit_status, 0) << "open() did not return Error::NoPipeWire";
    EXPECT_EQ(run.out_bytes, 0U);
    EXPECT_EQ(run.err_bytes, 0U);
    EXPECT_LT(run.elapsed, 5s);
}

TEST(OpenErrors, ListNoDaemonSubprocessIsNoPipeWireAndSilent) {
    const TempDir dir;
    ASSERT_FALSE(dir.path().empty());
    // A path that cannot exist: a fresh directory with nothing in it. The host daemon is untouched.
    const std::string missing_socket = (dir.path() / "no-such-socket").string();
    ASSERT_FALSE(fs::exists(missing_socket));
    const ProbeRun run = run_probe({"list-no-daemon"}, missing_socket);
    ASSERT_TRUE(run.spawned);
    EXPECT_FALSE(run.timed_out);
    EXPECT_EQ(run.exit_status, 0) << "list_applications() did not return Error::NoPipeWire";
    EXPECT_EQ(run.out_bytes, 0U);
    EXPECT_EQ(run.err_bytes, 0U);
    EXPECT_LT(run.elapsed, 5s);
}

TEST(OpenErrors, InvalidMatchSubprocessIsSilent) {
    const ProbeRun run = run_probe({"invalid-match"}, std::nullopt);
    ASSERT_TRUE(run.spawned);
    EXPECT_EQ(run.exit_status, 0);
    EXPECT_EQ(run.out_bytes, 0U);
    EXPECT_EQ(run.err_bytes, 0U);
}

TEST(OpenErrors, EmptySinkSubprocessIsSilent) {
    const ProbeRun run = run_probe({"empty-sink"}, std::nullopt);
    ASSERT_TRUE(run.spawned);
    EXPECT_EQ(run.exit_status, 0);
    EXPECT_EQ(run.out_bytes, 0U);
    EXPECT_EQ(run.err_bytes, 0U);
}

TEST(OpenErrors, WildcardIncludeIsInvalidMatch) {
    const auto result = pwcap::Capture::open(pwcap::Options{}, discarding_sink());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), pwcap::Error::InvalidMatch);
}

TEST(OpenErrors, WildcardExcludeIsInvalidMatch) {
    pwcap::Options options;
    options.include.node_name = "x";
    options.exclude.push_back(pwcap::Match{});
    const auto result = pwcap::Capture::open(std::move(options), discarding_sink());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), pwcap::Error::InvalidMatch);
}

TEST(OpenErrors, EmptySinkIsInvalidOptions) {
    pwcap::Options options;
    options.include.node_name = "x";
    const auto result = pwcap::Capture::open(std::move(options), pwcap::Sink{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), pwcap::Error::InvalidOptions);
}

TEST(OpenErrors, ErrorNamesAreStable) {
    EXPECT_EQ(pwcap::to_string(pwcap::Error::NoPipeWire), "NoPipeWire");
    EXPECT_EQ(pwcap::to_string(pwcap::Error::InvalidMatch), "InvalidMatch");
    EXPECT_EQ(pwcap::to_string(pwcap::Error::NoMatchingSource), "NoMatchingSource");
    EXPECT_EQ(pwcap::to_string(pwcap::Error::LinkFailed), "LinkFailed");
    EXPECT_EQ(pwcap::to_string(pwcap::Error::Stopped), "Stopped");
    EXPECT_EQ(pwcap::to_string(pwcap::Error::InvalidOptions), "InvalidOptions");
}

// ---- Handles ---------------------------------------------------------------------------------

TEST(Handles, AliasesAreMoveOnly) {
    using namespace pwcap::shell;
    static_assert(!std::is_copy_constructible_v<ThreadLoopPtr>);
    static_assert(!std::is_copy_constructible_v<ContextPtr>);
    static_assert(!std::is_copy_constructible_v<CorePtr>);
    static_assert(!std::is_copy_constructible_v<StreamPtr>);
    static_assert(!std::is_copy_constructible_v<ProxyPtr>);
    static_assert(!std::is_copy_constructible_v<PropertiesPtr>);
    static_assert(std::is_nothrow_move_constructible_v<ThreadLoopPtr>);
    static_assert(std::is_nothrow_move_constructible_v<ContextPtr>);
    static_assert(std::is_nothrow_move_constructible_v<CorePtr>);
    static_assert(std::is_nothrow_move_constructible_v<StreamPtr>);
    static_assert(std::is_nothrow_move_constructible_v<ProxyPtr>);
    static_assert(std::is_nothrow_move_constructible_v<PropertiesPtr>);
    static_assert(!std::is_copy_constructible_v<HookGuard>);
    static_assert(!std::is_move_constructible_v<HookGuard>);
    static_assert(!std::is_copy_assignable_v<HookGuard>);
    static_assert(!std::is_move_assignable_v<HookGuard>);
    SUCCEED();
}

TEST(Handles, LoopAndContextLifecycleWithoutDaemon) {
    using namespace pwcap::shell;
    pw_init(nullptr, nullptr);
    ThreadLoopPtr loop{pw_thread_loop_new("pwcap-test", nullptr)};
    ASSERT_TRUE(loop);
    ContextPtr context{pw_context_new(pw_thread_loop_get_loop(loop.get()), nullptr, 0)};
    ASSERT_TRUE(context);

    // Moved into new handles: the moved-from ones must not destroy anything.
    ThreadLoopPtr moved_loop{std::move(loop)};
    ContextPtr moved_context{std::move(context)};
    EXPECT_FALSE(loop);
    EXPECT_FALSE(context);

    ASSERT_GE(pw_thread_loop_start(moved_loop.get()), 0);
    {
        const LoopLock lock{moved_loop.get()};
        HookGuard unused_hook; // never added: reset() must be a no-op
        unused_hook.reset();
    }
    pw_thread_loop_stop(moved_loop.get());

    // Reverse order of creation.
    moved_context.reset();
    moved_loop.reset();
    EXPECT_FALSE(moved_context);
    EXPECT_FALSE(moved_loop);
}

// ---- RegistryProps ---------------------------------------------------------------------------

TEST(RegistryProps, NodeFromBoundProps) {
    // The shape of a Firefox node's bound props (s0-item4).
    const Props props{{"object.serial", "2201"},
                      {"node.name", "Firefox"},
                      {"application.name", "Firefox"},
                      {"application.process.binary", "firefox"},
                      {"application.process.id", "528076"},
                      {"media.class", "Stream/Output/Audio"},
                      {"media.name", "pwspike-tab-440"}};
    const auto node_opt = pwcap::shell::node_from_props(77, props.dict());
    const auto& node = require(node_opt);
    EXPECT_EQ(node.id, 77U);
    EXPECT_EQ(node.serial, 2201U);
    EXPECT_EQ(node.node_name, "Firefox");
    EXPECT_EQ(node.app_name, "Firefox");
    EXPECT_EQ(node.binary, "firefox");
    EXPECT_EQ(node.media_class, "Stream/Output/Audio");
    EXPECT_EQ(node.media_name, "pwspike-tab-440");
    ASSERT_TRUE(node.pid.has_value());
    EXPECT_EQ(require(node.pid), 528076);
}

TEST(RegistryProps, NodeRejectsBadSerial) {
    for (const char* bad : {"", "12x", "-1", "+5", " 5", "18446744073709551616", "0x10"}) {
        const Props props{{"object.serial", bad}, {"node.name", "n"}};
        EXPECT_FALSE(pwcap::shell::node_from_props(1, props.dict()).has_value())
            << "serial \"" << bad << "\" must be rejected";
    }
    const Props missing{{"node.name", "n"}};
    EXPECT_FALSE(pwcap::shell::node_from_props(1, missing.dict()).has_value());
    // The largest valid serial is accepted.
    const Props largest{{"object.serial", "18446744073709551615"}};
    const auto node_opt = pwcap::shell::node_from_props(1, largest.dict());
    const auto& node = require(node_opt);
    EXPECT_EQ(node.serial, UINT64_MAX);
}

TEST(RegistryProps, NodePidIsOptional) {
    const Props absent{{"object.serial", "5"}};
    const auto without_opt = pwcap::shell::node_from_props(1, absent.dict());
    const auto& without = require(without_opt);
    EXPECT_FALSE(without.pid.has_value());

    const Props garbage{{"object.serial", "5"}, {"application.process.id", "abc"}};
    const auto with_garbage_opt = pwcap::shell::node_from_props(1, garbage.dict());
    const auto& with_garbage = require(with_garbage_opt);
    EXPECT_FALSE(with_garbage.pid.has_value());

    const Props overflow{{"object.serial", "5"}, {"application.process.id", "99999999999"}};
    const auto with_overflow_opt = pwcap::shell::node_from_props(1, overflow.dict());
    const auto& with_overflow = require(with_overflow_opt);
    EXPECT_FALSE(with_overflow.pid.has_value());
}

TEST(RegistryProps, PortParsesDirectionChannelMonitor) {
    const Props monitor{{"node.id", "59"},
                        {"port.direction", "out"},
                        {"audio.channel", "FR"},
                        {"port.monitor", "true"}};
    const auto out_opt = pwcap::shell::port_from_props(45, monitor.dict());
    const auto& out = require(out_opt);
    EXPECT_EQ(out.node_id, 59U);
    EXPECT_EQ(out.port_id, 45U);
    EXPECT_EQ(out.direction, pwcap::core::Direction::Output);
    EXPECT_EQ(out.channel, "FR");
    EXPECT_TRUE(out.monitor);

    const Props plain{{"node.id", "60"}, {"port.direction", "in"}};
    const auto in_opt = pwcap::shell::port_from_props(46, plain.dict());
    const auto& in = require(in_opt);
    EXPECT_EQ(in.direction, pwcap::core::Direction::Input);
    EXPECT_TRUE(in.channel.empty());
    EXPECT_FALSE(in.monitor);
}

TEST(RegistryProps, PortRejectsBadNodeIdOrDirection) {
    const Props bad_node{{"node.id", "x"}, {"port.direction", "in"}};
    EXPECT_FALSE(pwcap::shell::port_from_props(1, bad_node.dict()).has_value());
    const Props no_node{{"port.direction", "in"}};
    EXPECT_FALSE(pwcap::shell::port_from_props(1, no_node.dict()).has_value());
    const Props bad_direction{{"node.id", "5"}, {"port.direction", "sideways"}};
    EXPECT_FALSE(pwcap::shell::port_from_props(1, bad_direction.dict()).has_value());
    const Props no_direction{{"node.id", "5"}};
    EXPECT_FALSE(pwcap::shell::port_from_props(1, no_direction.dict()).has_value());
    const Props negative_node{{"node.id", "-3"}, {"port.direction", "in"}};
    EXPECT_FALSE(pwcap::shell::port_from_props(1, negative_node.dict()).has_value());
}

TEST(RegistryProps, LinkRejectsMissingOrBadPort) {
    const Props good{{"link.output.port", "102"}, {"link.input.port", "47"}};
    const auto link_opt = pwcap::shell::link_from_props(good.dict());
    const auto& link = require(link_opt);
    EXPECT_EQ(link.output_port_id, 102U);
    EXPECT_EQ(link.input_port_id, 47U);

    const Props no_input{{"link.output.port", "102"}};
    EXPECT_FALSE(pwcap::shell::link_from_props(no_input.dict()).has_value());
    const Props no_output{{"link.input.port", "47"}};
    EXPECT_FALSE(pwcap::shell::link_from_props(no_output.dict()).has_value());
    const Props garbage{{"link.output.port", "102"}, {"link.input.port", "4x7"}};
    EXPECT_FALSE(pwcap::shell::link_from_props(garbage.dict()).has_value());
    const Props overflow{{"link.output.port", "4294967296"}, {"link.input.port", "47"}};
    EXPECT_FALSE(pwcap::shell::link_from_props(overflow.dict()).has_value());
}
