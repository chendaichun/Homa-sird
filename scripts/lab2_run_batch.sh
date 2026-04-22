#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LABEL="${1:-backlogged_sample}"
shift || true

PROFILES=("$@")
if [[ "${#PROFILES[@]}" -eq 0 ]]; then
  PROFILES=(test smoke fast full)
fi

TIMESTAMP="${TIMESTAMP:-$(date +%Y%m%d_%H%M%S)}"
BATCH_BASE_DIR="${BATCH_BASE_DIR:-/mnt/nasDisk_ds3617/sird/lab2}"
BATCH_DIR="${BATCH_DIR:-$BATCH_BASE_DIR/lab2_batch_${LABEL}_${TIMESTAMP}}"

mkdir -p "$BATCH_DIR"

echo "batchDir=$BATCH_DIR"
echo "profiles=${PROFILES[*]}"

for profile in "${PROFILES[@]}"; do
  out_dir="$BATCH_DIR/$profile"
  mkdir -p "$out_dir" "$out_dir/plots"
  nohup env \
    BUILD="${BUILD:-0}" \
    PLOT="${PLOT:-1}" \
    TRACE_DIR="$out_dir" \
    PLOT_OUT_DIR="$out_dir/plots" \
    MPLCONFIGDIR="${MPLCONFIGDIR:-/tmp/mpl-cache-lab2}" \
    TRACE_PROTOCOL_CREDIT="${TRACE_PROTOCOL_CREDIT:-0}" \
    TRACE_CREDIT_SAMPLE="${TRACE_CREDIT_SAMPLE:-1}" \
    TRACE_SIRD_CREDIT="${TRACE_SIRD_CREDIT:-0}" \
    TRACE_SIRD_BUCKET="${TRACE_SIRD_BUCKET:-0}" \
    TRACE_CREDIT_EVENTS="${TRACE_CREDIT_EVENTS:-0}" \
    TRACE_MSG="${TRACE_MSG:-0}" \
    TRACE_SWITCH_QUEUE="${TRACE_SWITCH_QUEUE:-1}" \
    bash scripts/lab2.sh "$profile" >"$out_dir/nohup.log" 2>&1 < /dev/null &
  echo "profile=$profile pid=$! out=$out_dir"
done
