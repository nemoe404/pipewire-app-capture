#!/usr/bin/env bash
# Runs the pipewire-labelled tests against the running session. Fails unless every selected test
# passed and none was skipped: a dead session must not turn into a green run.
set -euo pipefail
pw-cli info 0 >/dev/null || { echo "live: no PipeWire session" >&2; exit 1; }

id=${PWCAP_BUILD_ID:-ci}
export PWCAP_BUILD_ID="$id"
conan install . --output-folder="build/$id/conan" --profile:all=conan/profiles/clang21 \
    -s "&:build_type=Debug" --build=missing >/dev/null
cmake --preset debug >/dev/null
cmake --build --preset debug >/dev/null

selected=$(ctest --preset debug -L pipewire -N | grep -cE 'Test +#')
((selected > 0)) || { echo "live: no pipewire-labelled tests were selected" >&2; exit 1; }

status=0
PWCAP_TEST_PIPEWIRE=1 ctest --preset debug -L pipewire --output-on-failure >"build/$id/live.log" 2>&1 || status=$?
skipped=$(grep -c '\*\*\*Skipped' "build/$id/live.log" || true)
grep -E 'tests (passed|failed)' "build/$id/live.log" || true
echo "live: selected=$selected skipped=$skipped"
if ((status != 0 || skipped != 0)); then
    echo "live: FAILED (ctest exit $status, skipped $skipped)" >&2
    exit 1
fi
echo "LIVE-OK selected=$selected"
