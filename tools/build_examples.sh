#!/usr/bin/env bash
# Builds the ESP-IDF examples for each supported target so device-only
# compiler errors (e.g. uint32_t being `long` on Xtensa/RISC-V) are caught.
#
#   tools/build_examples.sh [target ...]     default: esp32s3 esp32c3 esp32p4 esp32
#
# Expects ESP-IDF at $IDF_PATH or ~/esp/esp-idf.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=(esp32s3 esp32c3 esp32p4 esp32)

if ! command -v idf.py >/dev/null 2>&1; then
  uv_python="$HOME/.local/share/uv/python/cpython-3.12.12-linux-aarch64-gnu/bin"
  [ -d "$uv_python" ] && export PATH="$uv_python:$HOME/.local/bin:$PATH"
  # shellcheck disable=SC1091
  . "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null
fi

status=0
for example in "$root"/examples/*/; do
  name="$(basename "$example")"
  for target in "${targets[@]}"; do
    log="$root/build/examples/$name-$target.log"
    mkdir -p "$(dirname "$log")"
    printf '%-20s %-8s ' "$name" "$target"
    # A fresh configure picks up new sources (ESP-IDF globs only at configure time).
    # Each target keeps its own sdkconfig in its build dir; the shared one pins a single target.
    if (cd "$example" && idf.py -B "build-$target" -DIDF_TARGET="$target" \
          -DSDKCONFIG="build-$target/sdkconfig" reconfigure build) >"$log" 2>&1; then
      warnings=$(grep -c 'warning:' "$log" || true)
      echo "ok (warnings: $warnings)"
    else
      echo "FAILED (see $log)"
      grep -E 'error:' "$log" | sort -u | head -5
      status=1
    fi
  done
done
exit $status
