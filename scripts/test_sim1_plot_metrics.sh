#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TRACE_DIR="${TRACE_DIR:-/tmp/sim1-plot-test/traces}"
OUT_DIR="${OUT_DIR:-/tmp/sim1-plot-test/plots}"
rm -rf "$TRACE_DIR" "$OUT_DIR"
mkdir -p "$TRACE_DIR" "$OUT_DIR"

cat >"$TRACE_DIR/sim1_balanced_google_rpc_load50.goodput.tr" <<'TRACE'
100000 goodputGbps=10 completedBytes=125000
200000 goodputGbps=20 completedBytes=375000
300000 goodputGbps=15 completedBytes=562500
TRACE

cat >"$TRACE_DIR/sim1_balanced_google_rpc_load50.tor-egress-queue.tr" <<'TRACE'
100000 queue=tor0_to_spine0 packets=2 bytes=3000
100000 queue=aggregate maxPackets=2 meanPackets=1.5 numQueues=2
200000 queue=tor0_to_spine0 packets=5 bytes=7500
200000 queue=aggregate maxPackets=5 meanPackets=3.0 numQueues=2
TRACE

cat >"$TRACE_DIR/sim1_balanced_google_rpc_load50.msg.tr" <<'TRACE'
+ 100000 1000 10.10.0.1:30000 10.10.1.1:30000 1
- 104100 1000 10.10.0.1:30000 10.10.1.1:30000 1
+ 200000 100000 10.10.0.1:30000 10.10.32.1:30000 2
- 230000 100000 10.10.0.1:30000 10.10.32.1:30000 2
TRACE

python3 scripts/sim1_plot.py \
  --trace-dir "$TRACE_DIR" \
  --out-dir "$OUT_DIR" \
  --bdp-pkts 10 \
  --mss-bytes 1000 \
  --wire-bytes 1000 \
  --host-rate-gbps 10 \
  --hosts-per-tor 16 \
  --link-delay-us 1

test -s "$OUT_DIR/sim1_goodput_queue_summary.csv"
test -s "$OUT_DIR/sim1_slowdown_summary.csv"
test -s "$OUT_DIR/sim1_goodput.png"
test -s "$OUT_DIR/sim1_tor_queue.png"
test -s "$OUT_DIR/sim1_slowdown_balanced_google_rpc_load50.png"

grep -q "balanced_google_rpc_load50,balanced,google_rpc,50,15.0,20.0,5.0,2.25,3.0,3,2" \
  "$OUT_DIR/sim1_goodput_queue_summary.csv"
grep -q "balanced_google_rpc_load50,balanced,google_rpc,50,A:<MSS,0" \
  "$OUT_DIR/sim1_slowdown_summary.csv"
grep -q "balanced_google_rpc_load50,balanced,google_rpc,50,B:MSS-BDP,1" \
  "$OUT_DIR/sim1_slowdown_summary.csv"
grep -q "balanced_google_rpc_load50,balanced,google_rpc,50,D:>8BDP,1" \
  "$OUT_DIR/sim1_slowdown_summary.csv"
