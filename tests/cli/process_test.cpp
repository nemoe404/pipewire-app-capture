// Session-free tests of the pwcap binary: exit codes, the usage and help text, and the rule that
// stdout stays empty on every failure. No PipeWire session is needed, so these run in every leg,
// including tsan. Each wait is bounded.

#include "spawn.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

#ifndef PWCAP_CLI_EXE
#error "PWCAP_CLI_EXE must name the built pwcap executable"
#endif

namespace {

namespace test = pwcap::test;

// The exit statuses the CLI promises (see src/cli/main.cpp and cli/args).
constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

constexpr std::chrono::seconds kProcessWait{10};

// A fresh directory under the system temp dir for one test, removed with everything in it.
class ProcessScratch {
public:
    ProcessScratch() {
        std::string pattern =
            (std::filesystem::temp_directory_path() / "pwcap-cli-XXXXXX").string();
        if (::mkdtemp(pattern.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed for the CLI process tests");
        }
        path_ = pattern;
    }
    ProcessScratch(const ProcessScratch&) = delete;
    ProcessScratch& operator=(const ProcessScratch&) = delete;
    ProcessScratch(ProcessScratch&&) = delete;
    ProcessScratch& operator=(ProcessScratch&&) = delete;
    ~ProcessScratch() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

struct RunResult {
    std::optional<int> status; // nullopt when the process was still running at the deadline
    std::string out;
    std::string err;
};

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// Runs pwcap with `args`, stdout and stderr to files in `dir`. `pipewire_remote`, when set, is the
// PIPEWIRE_REMOTE the child sees.
[[nodiscard]] RunResult run_cli(const std::vector<std::string>& args, const ProcessScratch& dir,
                                const std::optional<std::string>& pipewire_remote = std::nullopt) {
    std::vector<std::string> argv{std::string{PWCAP_CLI_EXE}};
    argv.insert(argv.end(), args.begin(), args.end());
    const std::filesystem::path out = dir.path() / "stdout.txt";
    const std::filesystem::path err = dir.path() / "stderr.txt";
    test::Spawned cli{argv, out, err, pipewire_remote};
    RunResult result;
    result.status = cli.wait_for(kProcessWait);
    result.out = read_text(out);
    result.err = read_text(err);
    return result;
}

// The last line of `text` that is not empty, or "" when there is none.
[[nodiscard]] std::string last_line(const std::string& text) {
    std::string last;
    std::ranges::for_each(std::views::split(text, '\n'), [&](auto&& line) {
        if (!std::ranges::empty(line)) {
            last.assign(line.begin(), line.end());
        }
    });
    return last;
}

// True when the child exited with `code`.
[[nodiscard]] bool exited_with(const RunResult& result, int code) {
    return result.status.has_value() && *result.status == code;
}

// A missing PIPEWIRE_REMOTE socket in `dir`: connecting to it fails at once with NoPipeWire.
[[nodiscard]] std::string missing_socket(const ProcessScratch& dir) {
    return (dir.path() / "no-daemon.sock").string();
}

} // namespace

TEST(CliProcess, UsageErrorsExitTwoWithEmptyStdout) {
    const ProcessScratch dir;
    const std::vector<std::vector<std::string>> cases{
        {},
        {"capture"},
        {"capture", "--app"},
        {"frobnicate"},
        {"capture", "--app", "x", "--bogus"},
        {"list", "--app", "x"},
        {"capture", "--exclude", "app=X"},
    };
    std::ranges::for_each(cases, [&](const std::vector<std::string>& args) {
        const RunResult result = run_cli(args, dir);
        EXPECT_TRUE(exited_with(result, kExitUsage)) << "usage error did not exit 2";
        EXPECT_TRUE(result.out.empty()) << "usage error wrote to stdout";
        EXPECT_FALSE(result.err.empty()) << "usage error printed nothing on stderr";
    });
}

TEST(CliProcess, HelpGoesToStdoutWithExitZero) {
    const ProcessScratch dir;
    const RunResult result = run_cli({"--help"}, dir);
    EXPECT_TRUE(exited_with(result, kExitOk));
    EXPECT_NE(result.out.find("capture"), std::string::npos);
    EXPECT_NE(result.out.find("list"), std::string::npos);
    EXPECT_TRUE(result.err.empty());
}

TEST(CliProcess, NoDaemonCaptureIsErrorAndSilentOnStdout) {
    const ProcessScratch dir;
    const RunResult result = run_cli({"capture", "--app", "x"}, dir, missing_socket(dir));
    EXPECT_TRUE(exited_with(result, kExitError));
    EXPECT_TRUE(result.out.empty()) << "a failed capture wrote to stdout";
    EXPECT_EQ(last_line(result.err), "pwcap: NoPipeWire");
}

TEST(CliProcess, NoDaemonListIsErrorAndSilentOnStdout) {
    const ProcessScratch dir;
    std::ranges::for_each(std::vector<std::vector<std::string>>{{"list"}, {"list", "--json"}},
                          [&](const std::vector<std::string>& args) {
                              const RunResult result = run_cli(args, dir, missing_socket(dir));
                              EXPECT_TRUE(exited_with(result, kExitError));
                              EXPECT_TRUE(result.out.empty()) << "a failed list wrote to stdout";
                              EXPECT_EQ(last_line(result.err), "pwcap: NoPipeWire");
                          });
}

TEST(CliProcess, QuietNoDaemonPrintsOneLine) {
    const ProcessScratch dir;
    const RunResult result =
        run_cli({"capture", "--app", "x", "--quiet"}, dir, missing_socket(dir));
    EXPECT_TRUE(exited_with(result, kExitError));
    EXPECT_EQ(result.err, "pwcap: NoPipeWire\n");
}

TEST(CliProcess, InvalidOptionsComesBeforePipeWire) {
    const ProcessScratch dir;
    const RunResult result =
        run_cli({"capture", "--app", "x", "--rate", "44100"}, dir, missing_socket(dir));
    EXPECT_TRUE(exited_with(result, kExitError));
    EXPECT_EQ(last_line(result.err), "pwcap: InvalidOptions");
}
