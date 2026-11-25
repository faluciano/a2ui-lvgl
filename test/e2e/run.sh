#!/usr/bin/env bash
# Starts examples/agent_surface/sample_agent.py on a free local port, runs the
# host end-to-end test against it, and stops the agent.
#
#   test/e2e/run.sh build/host/agent_e2e      (or: make e2e)
#
# Needs uv; the first run downloads the agent's one dependency (websockets).
set -euo pipefail

bin="$1"
root="$(cd "$(dirname "$0")/../.." && pwd)"
uv="${UV:-uv}"
logs="$root/build/e2e"
mkdir -p "$logs"

"$uv" run -q --script "$root/examples/agent_surface/sample_agent.py" --host 127.0.0.1 --port 0 \
  >"$logs/agent.out" 2>"$logs/agent.log" &
agent=$!
trap 'kill "$agent" 2>/dev/null || true; wait "$agent" 2>/dev/null || true' EXIT

url=""
for _ in $(seq 1 600); do  # Up to 60 s, for the first download.
  url="$(sed -n 's/^listening on //p' "$logs/agent.out")"
  [ -n "$url" ] && break
  if ! kill -0 "$agent" 2>/dev/null; then
    echo "sample agent exited:" >&2
    cat "$logs/agent.log" >&2
    exit 1
  fi
  sleep 0.1
done
if [ -z "$url" ]; then
  echo "sample agent did not start in 60 s" >&2
  exit 1
fi

if ! "$bin" "$url"; then
  echo "--- sample agent log ---" >&2
  cat "$logs/agent.log" >&2
  exit 1
fi
