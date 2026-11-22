#!/usr/bin/env bash
# cppcheck gate over every translation unit in the debug compile database.
#
# Exit: 0 clean, 1 findings, 2 usage or no compile database, 3 not every translation unit was
# analysed (syntax or internal error, or a unit missing from cppcheck's "Checking" output).
set -euo pipefail

root=$(git rev-parse --show-toplevel)
cd "$root"

id=${PWCAP_BUILD_ID:-gate}
db="build/$id/debug/compile_commands.json"
logs="build/$id/logs"
[[ $# -eq 0 ]] || { echo "usage: scripts/cppcheck.sh" >&2; exit 2; }
[[ -f $db ]] || {
    echo "cppcheck: no $db; configure and build the debug preset first (PWCAP_BUILD_ID=$id)" >&2
    exit 2
}

mkdir -p "$logs"
log="$logs/cppcheck.log"
cppcheck --project="$db" --std=c++23 \
    --enable=warning,style,performance,portability \
    --inline-suppr --library=googletest --suppress=missingIncludeSystem \
    --template='{file}:{line}: {severity}: {message} [{id}]' >"$log" 2>&1 || true

python3 - "$db" "$root" "$log" <<'PY'
import json, os, re, sys
db, root, log_path = sys.argv[1], sys.argv[2], sys.argv[3]
expected = {os.path.relpath(e["file"], root) for e in json.load(open(db, encoding="utf-8"))}
lines = open(log_path, encoding="utf-8", errors="replace").read().splitlines()

checked = set()
for line in lines:
    if line.startswith("Checking "):
        checked.add(re.split(r"(?: \.\.\.|: |\s)", line[len("Checking "):], maxsplit=1)[0])

broken = [l for l in lines if re.search(r"syntaxError|internalAstError|cppcheckError", l)]
missing = sorted(expected - checked)
if broken or missing:
    for l in broken[:20]:
        print(l, file=sys.stderr)
    if missing:
        print("cppcheck: not analysed: " + ", ".join(missing[:20]), file=sys.stderr)
    print(f"cppcheck: {len(broken)} internal error(s), {len(missing)} unit(s) not analysed", file=sys.stderr)
    sys.exit(3)

findings = [l for l in lines if re.search(r"\[[A-Za-z]+\]$", l)]
for l in findings:
    print(l)
if findings:
    print(f"cppcheck: {len(findings)} finding(s) in {len(expected)} unit(s)", file=sys.stderr)
    sys.exit(1)
print(f"CPPCHECK-OK units={len(expected)} checked={len(checked)} findings=0")
PY
