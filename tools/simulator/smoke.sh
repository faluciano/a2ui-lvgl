#!/bin/sh
# Copyright 2026 The a2ui-lvgl Authors
# SPDX-License-Identifier: Apache-2.0
#
# Headless check of the desktop simulator, run by CI and `make simulator-smoke`:
# shows the upstream coffee-order example with SDL's dummy video driver,
# clicks "Add to cart" and takes a screenshot. Fails unless the simulator
# exits 0, prints the add_to_cart action on stdout, writes a non-empty PNG
# and reports no surface error (the example sends its data after its
# components, so the first resolve sees null arguments).
#
#   tools/simulator/smoke.sh [build/sim/a2ui_simulator] [out-dir]

set -eu
sim=${1:-build/sim/a2ui_simulator}
out=${2:-build/sim/smoke}
example=third_party/a2ui/catalogs/basic/v1/examples/coffee-order.json
mkdir -p "$out"

status=0
SDL_VIDEODRIVER=dummy "$sim" "$example" --size 480x800 --exit-after 2000 \
  --click 172,281 --screenshot "$out/coffee-order.png" \
  >"$out/stdout.jsonl" 2>"$out/stderr.log" || status=$?

fail() {
  echo "simulator smoke: $1" >&2
  echo "--- stdout" >&2
  cat "$out/stdout.jsonl" >&2
  echo "--- stderr" >&2
  cat "$out/stderr.log" >&2
  exit 1
}

[ "$status" -eq 0 ] || fail "exit status $status"
grep -q '"name":"add_to_cart"' "$out/stdout.jsonl" || fail "no add_to_cart action on stdout"
[ -s "$out/coffee-order.png" ] || fail "no screenshot"
if grep -q 'surface error' "$out/stderr.log"; then fail "surface errors reported"; fi
if grep -q '"error"' "$out/stdout.jsonl"; then fail "error messages sent to the agent"; fi
echo "simulator smoke: ok ($(wc -l <"$out/stdout.jsonl" | tr -d ' ') message(s) for the agent)"
