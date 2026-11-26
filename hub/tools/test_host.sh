#!/bin/bash
# Host tests for the parts of the hub firmware that are plain C (no ESP-IDF).
# Usage: hub/tools/test_host.sh
set -e
cd "$(dirname "$0")/.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -O1 -Icomponents/knx/include \
   components/knx/knx_proto.c components/knx/test/test_knx_proto.c -lm -o "$out/test_knx_proto"
"$out/test_knx_proto"
