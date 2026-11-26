#!/bin/bash
# Build the hub firmware with ESP-IDF from ../../esp-idf, next to the repository (or $IDF_PATH),
# and print only diagnostics.
# Usage: hub/tools/build.sh [idf.py args...]   default: build
set -o pipefail
cd "$(dirname "$0")/.." || exit 1
IDF="${IDF_PATH:-$(cd ../../esp-idf && pwd)}"
. "$IDF/export.sh" >/dev/null 2>&1 || { echo "cannot source $IDF/export.sh"; exit 1; }
log=build/last-build.log
mkdir -p build
idf.py "${@:-build}" >"$log" 2>&1
rc=$?
grep -E "error:|warning:|undefined reference|FAILED:|CMake Error" "$log" | grep -v "^\[" | sort -u | head -60
tail -3 "$log"
exit $rc
