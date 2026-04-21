#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TRACE_DIR="${TRACE_DIR:-/mnt/nasDisk_ds3617/sird/HomaL4Protocol-sim1-large-leaf-spine}"
BUILD="${BUILD:-1}"
TRAFFIC_CONFIGS="${TRAFFIC_CONFIGS:-balanced core incast}"
OFFERED_LOAD="${OFFERED_LOAD:-0.5}"
START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-1.0}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.2}"
MAX_SETTLE_RETRIES="${MAX_SETTLE_RETRIES:-3}"
SETTLE_TAIL_MULTIPLIER="${SETTLE_TAIL_MULTIPLIER:-2}"
QUEUE_SAMPLE_US="${QUEUE_SAMPLE_US:-100}"
GOODPUT_SAMPLE_US="${GOODPUT_SAMPLE_US:-100}"
TRACE_MSG="${TRACE_MSG:-0}"
TRACE_TOR_QUEUE="${TRACE_TOR_QUEUE:-0}"
TRACE_GOODPUT="${TRACE_GOODPUT:-1}"
BDP_PKTS="${BDP_PKTS:-66.67}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-17p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"
QDISC_MARK_THRESHOLD="${QDISC_MARK_THRESHOLD:-$(awk "BEGIN { printf \"%dp\", (1.25 * $BDP_PKTS) + 0.5 }")}"
SUMMARY_FILE="${SUMMARY_FILE:-$TRACE_DIR/sim1_matrix_summary.csv}"
ENFORCE_MSG_COMPLETE="${ENFORCE_MSG_COMPLETE:-0}"

mkdir -p "$TRACE_DIR"
export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/sim1"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  ./waf build
fi

run_case() {
  local traffic_config="$1"
  local workload_tag="$2"
  local workload_file="$3"
  local tag="${traffic_config}_${workload_tag}_load50"
  local log_file="$TRACE_DIR/sim1_${tag}.run.log"
  local settle_tail_sec="$SETTLE_TAIL_SEC"
  local attempt=1

  while true; do
    rm -f \
      "$TRACE_DIR/sim1_${tag}.msg.tr" \
      "$TRACE_DIR/sim1_${tag}.tor-egress-queue.tr" \
      "$TRACE_DIR/sim1_${tag}.goodput.tr"

    echo "[$tag] attempt=$attempt settleTailSec=$settle_tail_sec start $(date '+%F %T') workload=$workload_file trafficConfig=$traffic_config offeredLoad=$OFFERED_LOAD" | tee "$log_file"
    "$BIN_PATH" \
      "--simTag=$tag" \
      "--outputDir=$TRACE_DIR" \
      "--trafficConfig=$traffic_config" \
      "--workloadFile=$workload_file" \
      "--offeredLoad=$OFFERED_LOAD" \
      "--startSec=$START_SEC" \
      "--durationSec=$DURATION_SEC" \
      "--settleTailSec=$settle_tail_sec" \
      "--traceMsg=$TRACE_MSG" \
      "--traceTorQueue=$TRACE_TOR_QUEUE" \
      "--traceGoodput=$TRACE_GOODPUT" \
      "--queueSampleUs=$QUEUE_SAMPLE_US" \
      "--goodputSampleUs=$GOODPUT_SAMPLE_US" \
      "--bdpPkts=$BDP_PKTS" \
      "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE" \
      "--qdiscMaxSize=$QDISC_MAX_SIZE" \
      "--qdiscMarkThreshold=$QDISC_MARK_THRESHOLD" >>"$log_file" 2>&1

    local msg_trace="$TRACE_DIR/sim1_${tag}.msg.tr"
    local started=0
    local finished=0
    if [[ -f "$msg_trace" ]]; then
      started=$(awk '$1=="+" {c++} END {print c+0}' "$msg_trace")
      finished=$(awk '$1=="-" {c++} END {print c+0}' "$msg_trace")
    fi

    if [[ "$ENFORCE_MSG_COMPLETE" != "1" ]]; then
      echo "[$tag] done $(date '+%F %T') started=$started finished=$finished incomplete=$((started - finished))" | tee -a "$log_file"
      break
    fi

    if [[ "$started" -eq "$finished" && "$started" -gt 0 ]]; then
      echo "[$tag] done $(date '+%F %T') started=$started finished=$finished" | tee -a "$log_file"
      break
    fi

    if [[ "$attempt" -ge "$MAX_SETTLE_RETRIES" ]]; then
      echo "[$tag] incomplete after $attempt attempts: started=$started finished=$finished" | tee -a "$log_file"
      return 1
    fi

    echo "[$tag] incomplete: started=$started finished=$finished, increasing settleTailSec" | tee -a "$log_file"
    settle_tail_sec=$(awk -v x="$settle_tail_sec" -v m="$SETTLE_TAIL_MULTIPLIER" 'BEGIN {printf "%.6f", x*m}')
    attempt=$((attempt + 1))
  done
}

pids=()
read -r -a traffic_configs <<< "$TRAFFIC_CONFIGS"
workload_tags=(dctcp)
workload_files=(inputs/homa-paper-reproduction/DCTCP-MsgSizeDist.txt)

if [[ -f inputs/Facebook_HadoopDist_All.txt ]]; then
  workload_tags+=(facebook_hadoop)
  workload_files+=(inputs/Facebook_HadoopDist_All.txt)
fi

for traffic_config in "${traffic_configs[@]}"; do
  for idx in "${!workload_tags[@]}"; do
    run_case "$traffic_config" "${workload_tags[$idx]}" "${workload_files[$idx]}" &
    pids+=("$!")
  done
done

status=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    status=1
  fi
done

{
  echo "tag,msg_begin,msg_finish,msg_incomplete,goodput_lines,tor_egress_queue_lines,last_goodput_line"
  for traffic_config in "${traffic_configs[@]}"; do
    for workload_tag in "${workload_tags[@]}"; do
      tag="${traffic_config}_${workload_tag}_load50"
      msg_begin=$(awk '$1=="+" {c++} END {print c+0}' "$TRACE_DIR/sim1_${tag}.msg.tr" 2>/dev/null || echo 0)
      msg_finish=$(awk '$1=="-" {c++} END {print c+0}' "$TRACE_DIR/sim1_${tag}.msg.tr" 2>/dev/null || echo 0)
      msg_incomplete=$((msg_begin - msg_finish))
      goodput_lines=$(wc -l < "$TRACE_DIR/sim1_${tag}.goodput.tr" 2>/dev/null || echo 0)
      tor_queue_lines=$(wc -l < "$TRACE_DIR/sim1_${tag}.tor-egress-queue.tr" 2>/dev/null || echo 0)
      last_goodput_line=$(tail -n 1 "$TRACE_DIR/sim1_${tag}.goodput.tr" 2>/dev/null | tr ',' ';')
      echo "$tag,$msg_begin,$msg_finish,$msg_incomplete,$goodput_lines,$tor_queue_lines,$last_goodput_line"
    done
  done
} > "$SUMMARY_FILE"

exit "$status"
