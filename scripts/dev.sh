#!/usr/bin/env bash
# Build everything, start one fixgen-sim per configured venue and the gateway.
# Open http://127.0.0.1:8080 afterwards. Ctrl-C stops everything.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -G Ninja >/dev/null
cmake --build build
export DEMO_PASSWORD=${DEMO_PASSWORD:-demo} ACME_PASSWORD=${ACME_PASSWORD:-acme}
pids=()
trap 'kill "${pids[@]}" 2>/dev/null' EXIT
./build/fixgen-sim --venue demo_exch --port 9878 --fill partial --quiet & pids+=($!)
./build/fixgen-sim --venue acme_mtf --port 9879 --fill immediate --quiet & pids+=($!)
./build/fixgen-gateway --config config/gateway.json
