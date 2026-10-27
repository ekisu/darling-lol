#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"

# Session diagnostics are opt-in; the collector re-enters with RCLOL_COLLECTED=1.
if [ "${RCLOL_COLLECT:-0}" = "1" ] && [ -z "${RCLOL_COLLECTED:-}" ] && [ -z "${RCLOL_NO_COLLECT:-}" ] && [ -x "$repo/tools/collect-riot-session.py" ]; then
  RCLOL_COLLECTED=1 exec "$repo/tools/collect-riot-session.py"
fi
if [ -n "${DARLING_WORKSPACE:-}" ] && ! python3 scripts/nix-state.py check; then
  echo 'Rootless runtime readiness is missing or incompatible; run darling-lol --installer in a terminal.' >&2
  exit 1
fi
export PATH="${DARLING_RUNNER_DIR:+$DARLING_RUNNER_DIR:}$repo/usr-local/bin:/usr/local/bin:$PATH"
python3 scripts/configure.py >/dev/null
if [ -d build/venv/bin ]; then PATH="$repo/build/venv/bin:$PATH"; export PATH; fi
# darling-lol perf: optionally wrap with gamemoderun for automatic
# CPU governor, nice level, and scheduler optimizations.
if [ "${RCLOL_GAMEMODE:-auto}" != "0" ] && command -v gamemoderun >/dev/null 2>&1; then
  exec gamemoderun sh build/runtime/tools/run-riot-client.sh
fi
exec sh build/runtime/tools/run-riot-client.sh
