#!/usr/bin/env bash
# Builds the ESP-IDF examples and tools/stack_probe for each supported target
# so device-only compiler errors (e.g. uint32_t being `long` on Xtensa/RISC-V)
# are caught.
#
#   tools/build_examples.sh [target ...]     default: esp32s3 esp32c3 esp32p4 esp32
#
# Expects an activated ESP-IDF, or ESP-IDF at $IDF_PATH or ~/esp/esp-idf.
# Set A2UI_FAIL_ON_WARNINGS=1 to fail when a build prints compiler warnings.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=(esp32s3 esp32c3 esp32p4 esp32)

if ! command -v idf.py >/dev/null 2>&1; then
  # Optional: a directory holding the python3 that ESP-IDF should use.
  [ -n "${A2UI_IDF_PYTHON_BIN:-}" ] && export PATH="$A2UI_IDF_PYTHON_BIN:$PATH"
  # shellcheck disable=SC1091
  . "${IDF_PATH:-$HOME/esp/esp-idf}/export.sh" >/dev/null
fi

status=0
for example in "$root"/examples/*/ "$root"/tools/stack_probe/; do
  name="$(basename "$example")"
  for target in "${targets[@]}"; do
    log="$root/build/examples/$name-$target.log"
    mkdir -p "$(dirname "$log")"
    printf '%-20s %-8s ' "$name" "$target"
    # A fresh configure picks up new sources (ESP-IDF globs only at configure time).
    # Each target keeps its own sdkconfig in its build dir; the shared one pins a single target.
    # Build dirs live under the repository's build/, not in the project. ESP-IDF
    # still writes managed_components/ and dependencies.lock next to the project;
    # both are gitignored and left out of registry archives by default.
    out="$root/build/examples/$name-$target"
    if (cd "$example" && idf.py -B "$out" -DIDF_TARGET="$target" \
          -DSDKCONFIG="$out/sdkconfig" reconfigure build) >"$log" 2>&1; then
      warnings=$(grep -c 'warning:' "$log" || true)
      echo "ok (warnings: $warnings)"
      if [ "$warnings" -gt 0 ] && [ -n "${A2UI_FAIL_ON_WARNINGS:-}" ]; then
        grep 'warning:' "$log" | sort -u | head -20
        status=1
      fi
    else
      echo "FAILED (see $log)"
      grep -E 'error:' "$log" | sort -u | head -5
      status=1
    fi
  done
done
exit $status
