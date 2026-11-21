#!/usr/bin/env bash
# Full gate, in order: clang-format -> ctest legs asan-ubsan, tsan, debug -> cppcheck -> clang-tidy.
#
# Takes no arguments. A leg name would silently narrow the sweep, so any argument is refused.
# The first failing stage stops the run with its own exit code (see the stage scripts).
# Integration tests (label pipewire) are skipped unless PWCAP_TEST_PIPEWIRE=1; the skip count
# is printed for every leg, so a skip is never counted as a pass.
set -euo pipefail

if (($# != 0)); then
    echo "check.sh takes no arguments: run it bare so every stage runs" >&2
    exit 2
fi

root=$(git rev-parse --show-toplevel)
cd "$root"

id=${PWCAP_BUILD_ID:-gate}
export PWCAP_BUILD_ID="$id"
logs="build/$id/logs"
mkdir -p "$logs"
clang_format=${CLANG_FORMAT:-clang-format-21}

stage() { printf '\n== %s\n' "$1"; }

# 1. Format first: cheapest, and needs no configure.
stage "clang-format"
mapfile -t sources < <(git ls-files '*.cpp' '*.hpp' '*.h')
if ((${#sources[@]} == 0)); then
    echo "check: git ls-files lists no C++ sources; refusing to pass an empty sweep" >&2
    exit 2
fi
unformatted=()
for file in "${sources[@]}"; do
    "$clang_format" --dry-run -Werror "$file" >/dev/null 2>&1 || unformatted+=("$file")
done
if ((${#unformatted[@]} > 0)); then
    echo "check: ${#unformatted[@]} file(s) not clang-formatted. Fix with:" >&2
    printf '  %s -i %s\n' "$clang_format" "${unformatted[*]}" >&2
    exit 1
fi
echo "FORMAT-OK files=${#sources[@]}"

# 2. Compile legs. One Debug conan install serves all of them.
stage "dependencies"
conan install . --output-folder="$root/build/$id/conan" --profile:all=conan/profiles/clang21 \
    -s "&:build_type=Debug" --build=missing >"$logs/conan.log" 2>&1 || {
    echo "check: conan install failed; see $logs/conan.log" >&2
    exit 1
}

for leg in asan-ubsan tsan debug; do
    stage "leg $leg"
    cmake --preset "$leg" >"$logs/$leg.configure.log" 2>&1 || {
        echo "check: configure failed for $leg; see $logs/$leg.configure.log" >&2
        exit 1
    }
    cmake --build --preset "$leg" >"$logs/$leg.build.log" 2>&1 || {
        echo "check: build failed for $leg; see $logs/$leg.build.log" >&2
        grep -E ' error' "$logs/$leg.build.log" | head -n 10 >&2
        exit 1
    }
    status=0
    ctest --preset "$leg" >"$logs/$leg.ctest.log" 2>&1 || status=$?
    summary=$(grep -E 'tests (passed|failed)' "$logs/$leg.ctest.log" || true)
    skipped=$(grep -c '\*\*\*Skipped' "$logs/$leg.ctest.log" || true)
    echo "leg $leg: ${summary:-no ctest summary} skipped=$skipped"
    if ((status != 0)); then
        echo "check: tests failed in leg $leg; see $logs/$leg.ctest.log" >&2
        exit 1
    fi
done

# 3. Static analysis over the debug database that the legs just built.
stage "cppcheck"
scripts/cppcheck.sh

# 4. clang-tidy over every translation unit in the same database.
stage "clang-tidy"
scripts/tidy.sh --all

printf '\nCHECK-OK stages=format,legs(asan-ubsan,tsan,debug),cppcheck,clang-tidy\n'
