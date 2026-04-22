#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TRACE_DIR="${TRACE_DIR:-/tmp/lab2-credit-sample-test/traces}"
OUT_DIR="${OUT_DIR:-/tmp/lab2-credit-sample-test/plots}"
rm -rf "$TRACE_DIR" "$OUT_DIR"
mkdir -p "$TRACE_DIR" "$OUT_DIR"

cat >"$TRACE_DIR/lab2_feedback.credit-sample.tr" <<'TRACE'
200000000 sender=10.2.0.1 senderCreditPkts=10 receiverAvailPkts=120 senderCreditXbdp=0.3 receiverAvailXbdp=3.6 receiverCount=3
201000000 sender=10.2.0.1 senderCreditPkts=12 receiverAvailPkts=118 senderCreditXbdp=0.36 receiverAvailXbdp=3.54 receiverCount=3
TRACE

cat >"$TRACE_DIR/lab2_no_feedback.credit-sample.tr" <<'TRACE'
200000000 sender=10.2.0.1 senderCreditPkts=30 receiverAvailPkts=90 senderCreditXbdp=0.9 receiverAvailXbdp=2.7 receiverCount=3
201000000 sender=10.2.0.1 senderCreditPkts=31 receiverAvailPkts=89 senderCreditXbdp=0.93 receiverAvailXbdp=2.67 receiverCount=3
TRACE

MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/mpl-cache-lab2-test}" python3 scripts/lab2_plot.py \
  --trace-dir "$TRACE_DIR" \
  --out-dir "$OUT_DIR" \
  --feedback-tag feedback \
  --no-feedback-tag no_feedback \
  --bdp-pkts 33.32 \
  --sample-us 1000 \
  --start-sec 0.2

test -s "$OUT_DIR/lab2_sender_credit_dynamics.png"
test -s "$OUT_DIR/lab2_summary.csv"
grep -q "feedback,10.2.0.1,,2" "$OUT_DIR/lab2_summary.csv"
grep -q "no_feedback,10.2.0.1,,2" "$OUT_DIR/lab2_summary.csv"
