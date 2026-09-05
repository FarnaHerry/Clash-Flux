#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
BIN="${CLASHFLUX_BIN:-build/clash-flux}"
if [ ! -x "$BIN" ]; then
    echo "binary not found — run \`cmake -B build -G Ninja && cmake --build build\` first" >&2
    exit 1
fi
export INTEL_FORCE_PROBE="${INTEL_FORCE_PROBE:-1}"   # Intel Arc B390 iris DRI workaround
exec "./$BIN" "$@"
