#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROFILE="${1:-fast}"

START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-0.005}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.02}"
SHORT_INTERVAL_8B_US="${SHORT_INTERVAL_8B_US:-1}"
SHORT_INTERVAL_500KB_US="${SHORT_INTERVAL_500KB_US:-500}"
TARGET_PROBE_MESSAGES="${TARGET_PROBE_MESSAGES:-0}"

if [[ "$PROFILE" == "last" ]]; then
  TARGET_PROBE_MESSAGES="${TARGET_PROBE_MESSAGES_LAST:-500}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_LAST:-0.3}"
elif [[ "$PROFILE" != "fast" ]]; then
  echo "Usage: bash scripts/lab1.sh [fast|last]"
  exit 2
fi

ENABLE_SIRD="${ENABLE_SIRD:-1}"
LONG_MSG_SIZE_BYTES="${LONG_MSG_SIZE_BYTES:-10000000}"
LONG_SENDER_RATE_GBPS="${LONG_SENDER_RATE_GBPS:-17.0}"
PLOT="${PLOT:-1}"
BUILD="${BUILD:-1}"
TRACE_QUEUE="${TRACE_QUEUE:-0}"
TRACE_QUEUE_SAMPLE_US="${TRACE_QUEUE_SAMPLE_US:-1}"
TRACE_SIRD_CREDIT="${TRACE_SIRD_CREDIT:-0}"
TRACE_SIRD_LOOP="${TRACE_SIRD_LOOP:-0}"
ENFORCE_MSG_COMPLETE="${ENFORCE_MSG_COMPLETE:-0}"
MAX_SETTLE_RETRIES="${MAX_SETTLE_RETRIES:-4}"
SETTLE_TAIL_MULTIPLIER="${SETTLE_TAIL_MULTIPLIER:-2}"

BDP_PKTS="${BDP_PKTS:-33.32}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-17p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"

if [[ -z "${OUT_DIR+x}" ]]; then
  OUT_DIR="outputs/sird-scenarios/HomaL4Protocol-lab1-receiver-congestion"
fi
if [[ "$OUT_DIR" == *" "* ]]; then
  echo "OUT_DIR must not contain spaces because ns-3 trace output parsing truncates such paths: $OUT_DIR"
  exit 2
fi
PLOT_OUT_DIR="${PLOT_OUT_DIR:-$OUT_DIR/plots}"
mkdir -p "$OUT_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/lab1"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  echo "Building scratch/lab1..."
  ./waf build
fi

if [[ "$ENABLE_SIRD" == "1" ]]; then
  CC_LABEL="sird"
else
  CC_LABEL="homa"
fi

COMMON_ARGS=(
  "--enableSird=$ENABLE_SIRD"
  "--outputDir=$OUT_DIR"
  "--startSec=$START_SEC"
  "--longMsgSizeBytes=$LONG_MSG_SIZE_BYTES"
  "--longSenderRateGbps=$LONG_SENDER_RATE_GBPS"
  "--bdpPkts=$BDP_PKTS"
  "--traceMsg=1"
  "--tracePathRtt=0"
  "--traceSwitchEgressQueue=$TRACE_QUEUE"
  "--switchQueueSampleUs=$TRACE_QUEUE_SAMPLE_US"
  "--traceLinkThroughput=1"
  "--traceSirdCredit=$TRACE_SIRD_CREDIT"
  "--traceSirdLoop=$TRACE_SIRD_LOOP"
  "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
  "--qdiscMaxSize=$QDISC_MAX_SIZE"
  "--showProgressBar=0"
)

run_case() {
  local label="$1"
  local probe_size="$2"
  local enable_background="$3"
  local use_srr="$4"
  local short_interval_us="$5"
  local tag="lab1_${PROFILE}_${CC_LABEL}_${label}"
  local log_file="$OUT_DIR/${tag}.run.log"
  local settle_tail_sec="$SETTLE_TAIL_SEC"
  local target_probe_messages="$TARGET_PROBE_MESSAGES"
  local duration_sec="$DURATION_SEC"
  local attempt=1

  if [[ "$target_probe_messages" -gt 0 ]]; then
    duration_sec=$(awk -v n="$target_probe_messages" -v us="$short_interval_us" 'BEGIN {printf "%.9f", n * us / 1000000.0}')
  fi

  while true; do
    rm -f \
      "$OUT_DIR/lab1_${tag}.msg.tr" \
      "$OUT_DIR/lab1_${tag}.path-rtt.tr" \
      "$OUT_DIR/lab1_${tag}.link-throughput.tr" \
      "$OUT_DIR/lab1_${tag}.switch-egress-queue.tr" \
      "$OUT_DIR/lab1_${tag}.sird-credit.tr"

    echo "[$tag] attempt=$attempt durationSec=$duration_sec settleTailSec=$settle_tail_sec shortIntervalUs=$short_interval_us targetProbeMessages=$target_probe_messages start $(date '+%F %T')" | tee "$log_file"
    "$BIN_PATH" \
      "--simTag=$tag" \
      "--shortMsgSizeBytes=$probe_size" \
      "--shortIntervalUs=$short_interval_us" \
      "--durationSec=$duration_sec" \
      "--targetProbeMessages=$target_probe_messages" \
      "--enableBackgroundTraffic=$enable_background" \
      "--useSrrScheduling=$use_srr" \
      "--settleTailSec=$settle_tail_sec" \
      "${COMMON_ARGS[@]}" >>"$log_file" 2>&1

    local msg_trace="$OUT_DIR/lab1_${tag}.msg.tr"
    local started=0
    local finished=0
    if [[ -f "$msg_trace" ]]; then
      started=$(awk -v sz="$probe_size" '$1=="+" && $3==sz {c++} END {print c+0}' "$msg_trace")
      finished=$(awk -v sz="$probe_size" '$1=="-" && $3==sz {c++} END {print c+0}' "$msg_trace")
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

echo "profile=$PROFILE enableSird=$ENABLE_SIRD durationSec=$DURATION_SEC settleTailSec=$SETTLE_TAIL_SEC targetProbeMessages=$TARGET_PROBE_MESSAGES"
echo "outputs: $OUT_DIR"

pids=()
run_case "unloaded_8B" 8 0 0 "$SHORT_INTERVAL_8B_US" &
pids+=("$!")
run_case "incast_8B_srpt" 8 1 0 "$SHORT_INTERVAL_8B_US" &
pids+=("$!")
run_case "unloaded_500KB" 500000 0 0 "$SHORT_INTERVAL_500KB_US" &
pids+=("$!")
run_case "incast_500KB_srpt" 500000 1 0 "$SHORT_INTERVAL_500KB_US" &
pids+=("$!")
run_case "incast_500KB_srr" 500000 1 1 "$SHORT_INTERVAL_500KB_US" &
pids+=("$!")

status=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    status=1
  fi
done

if [[ "$status" -ne 0 ]]; then
  echo "At least one lab1 run failed. Check $OUT_DIR/*.run.log"
  exit "$status"
fi

echo "lab1 runs finished."

if [[ "$PLOT" == "1" ]]; then
  mkdir -p "$PLOT_OUT_DIR"
  rm -f "$PLOT_OUT_DIR"/lab1_*.png "$PLOT_OUT_DIR"/lab1_summary.csv
  python3 scripts/lab1_plot.py \
    --trace-dir "$OUT_DIR" \
    --out-dir "$PLOT_OUT_DIR" \
    --tag "lab1_${PROFILE}_${CC_LABEL}_unloaded_8B" \
    --tag "lab1_${PROFILE}_${CC_LABEL}_incast_8B_srpt" \
    --tag "lab1_${PROFILE}_${CC_LABEL}_unloaded_500KB" \
    --tag "lab1_${PROFILE}_${CC_LABEL}_incast_500KB_srpt" \
    --tag "lab1_${PROFILE}_${CC_LABEL}_incast_500KB_srr" \
    --size 8 \
    --size 500000 \
    --start-sec "$START_SEC"
fi
