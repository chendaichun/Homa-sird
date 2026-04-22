#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BIN_PATH="$ROOT_DIR/build/scratch/sim1"
if [[ ! -x "$BIN_PATH" ]]; then
  ./waf build
fi
export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

OUT_DIR="${OUT_DIR:-/tmp/sim1-paper-raw-test}"
mkdir -p "$OUT_DIR"

for workload in google_rpc facebook_hadoop web_search; do
  "$BIN_PATH" \
    "--simTag=paper_raw_${workload}" \
    "--outputDir=$OUT_DIR" \
    "--trafficConfig=balanced" \
    "--paperRawWorkload=$workload" \
    "--paperRawLoadFactor=0.5" \
    "--offeredLoad=0.5" \
    "--startSec=0.0001" \
    "--durationSec=0.0001" \
    "--settleTailSec=0.0001" \
    "--traceMsg=0" \
    "--traceTorQueue=0" \
    "--traceGoodput=0"
done
