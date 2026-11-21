#!/usr/bin/env bash
# clang-tidy gate for translation units from the debug compile database.
#
#   scripts/tidy.sh <file>...        check the given files (cheap, run after every edit)
#   scripts/tidy.sh --all            check every translation unit in the database (the gate)
#   scripts/tidy.sh --fix <file>...  apply clang-tidy's automatic fixes first (serial)
#
# Exit: 0 clean, 1 findings, 2 usage or no compile database, 3 file not in the database,
# 4 clang-tidy did not analyse a file. Exit 4 is not a pass: it means nothing was checked.
set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

id=${PWCAP_BUILD_ID:-gate}
build_dir="build/$id/debug"
db="$build_dir/compile_commands.json"
tidy=${CLANG_TIDY:-clang-tidy-21}
logs="build/$id/logs"

usage() {
    echo "usage: scripts/tidy.sh [--fix] (--all | <file>...)" >&2
    exit 2
}

fix=()
if [[ ${1:-} == --fix ]]; then
    fix=(--fix)
    shift
fi
[[ $# -ge 1 ]] || usage

[[ -f $db ]] || {
    echo "tidy: no $db; configure and build the debug preset first (PWCAP_BUILD_ID=$id)" >&2
    exit 2
}

# The translation units the database holds, as paths relative to the repository root.
db_files() {
    python3 - "$db" "$root" <<'PY'
import json, os, sys
db, root = sys.argv[1], sys.argv[2]
seen = set()
for entry in json.load(open(db, encoding="utf-8")):
    rel = os.path.relpath(entry["file"], root)
    if rel not in seen:
        seen.add(rel)
        print(rel)
PY
}

mapfile -t known < <(db_files)
targets=()
if [[ $1 == --all ]]; then
    targets=("${known[@]}")
else
    for arg in "$@"; do
        norm=$(realpath --relative-to="$root" "$arg")
        if ! printf '%s\n' "${known[@]}" | grep -qxF -- "$norm"; then
            echo "tidy: $arg is not a translation unit in $db; it would not be analysed" >&2
            exit 3
        fi
        targets+=("$norm")
    done
fi

mkdir -p "$logs"
findings=0
not_analysed=0
for file in "${targets[@]}"; do
    log="$logs/tidy-$(tr / _ <<<"$file").log"
    status=0
    "$tidy" -p "$build_dir" --quiet "${fix[@]}" "$file" >"$log" 2>&1 || status=$?
    if grep -qE 'Error while processing|Error reading configuration|unable to handle compilation' "$log"; then
        echo "tidy: NOT ANALYSED $file" >&2
        grep -E 'Error' "$log" | head -n 3 >&2
        not_analysed=$((not_analysed + 1))
    elif ((status != 0)); then
        grep -E '^[^ ].*:[0-9]+:[0-9]+: (warning|error): ' "$log" | head -n 30
        findings=$((findings + 1))
    fi
done

if ((not_analysed > 0)); then
    echo "tidy: $not_analysed of ${#targets[@]} file(s) not analysed" >&2
    exit 4
fi
if ((findings > 0)); then
    echo "tidy: $findings of ${#targets[@]} file(s) with findings" >&2
    exit 1
fi
echo "TIDY-OK files=${#targets[@]} findings=0"
