#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PROFILE="${1:-fast}"

START_SEC="${START_SEC:-0.2}"
DURATION_SEC="${DURATION_SEC:-0.6}"
FLOW_GAP_US="${FLOW_GAP_US:-200000}"
ACTIVE_RECEIVER_COUNT="${ACTIVE_RECEIVER_COUNT:-3}"
SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC:-0.1}"
SEND_INTERVAL_US="${SEND_INTERVAL_US:-400}"
BACKLOGGED_FLOW="${BACKLOGGED_FLOW:-1}"
BACKLOG_DEPTH_MSGS="${BACKLOG_DEPTH_MSGS:-2}"

if [[ "$PROFILE" == "smoke" ]]; then
  START_SEC="${START_SEC_SMOKE:-0.2}"
  DURATION_SEC="${DURATION_SEC_SMOKE:-0.15}"
  FLOW_GAP_US="${FLOW_GAP_US_SMOKE:-50000}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_SMOKE:-0.05}"
elif [[ "$PROFILE" == "test" || "$PROFILE" == "one_receiver_smoke" ]]; then
  START_SEC="${START_SEC_ONE_RECEIVER_SMOKE:-0.2}"
  DURATION_SEC="${DURATION_SEC_ONE_RECEIVER_SMOKE:-0.05}"
  FLOW_GAP_US="${FLOW_GAP_US_ONE_RECEIVER_SMOKE:-50000}"
  ACTIVE_RECEIVER_COUNT="${ACTIVE_RECEIVER_COUNT_ONE_RECEIVER_SMOKE:-1}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_ONE_RECEIVER_SMOKE:-0.0}"
elif [[ "$PROFILE" == "full" ]]; then
  START_SEC="${START_SEC_FULL:-0.2}"
  DURATION_SEC="${DURATION_SEC_FULL:-13.2}"
  FLOW_GAP_US="${FLOW_GAP_US_FULL:-4500000}"
  SETTLE_TAIL_SEC="${SETTLE_TAIL_SEC_FULL:-0.1}"
elif [[ "$PROFILE" != "fast" ]]; then
  echo "Usage: bash scripts/lab2.sh [test|smoke|one_receiver_smoke|fast|full]"
  exit 2
fi

BUILD="${BUILD:-1}"
PLOT="${PLOT:-1}"
OUTCAST_MSG_SIZE_BYTES="${OUTCAST_MSG_SIZE_BYTES:-10000000}"
TRACE_MSG="${TRACE_MSG:-0}"
TRACE_PROTOCOL_CREDIT="${TRACE_PROTOCOL_CREDIT:-0}"
TRACE_CREDIT_SAMPLE="${TRACE_CREDIT_SAMPLE:-1}"
TRACE_SIRD_CREDIT="${TRACE_SIRD_CREDIT:-0}"
TRACE_SIRD_BUCKET="${TRACE_SIRD_BUCKET:-0}"
TRACE_CREDIT_EVENTS="${TRACE_CREDIT_EVENTS:-0}"
TRACE_SWITCH_QUEUE="${TRACE_SWITCH_QUEUE:-1}"
CREDIT_SAMPLE_US="${CREDIT_SAMPLE_US:-100}"
TRACE_SWITCH_QUEUE_SAMPLE_US="${TRACE_SWITCH_QUEUE_SAMPLE_US:-1000}"

# Topology-derived RTT BDP in packets for the current lab2 topology.
BDP_PKTS="${BDP_PKTS:-33.32}"
SIRD_CSN_THRESHOLD_PKTS="${SIRD_CSN_THRESHOLD_PKTS:-$(awk "BEGIN { printf \"%d\", (0.5 * $BDP_PKTS) + 0.5 }")}"
SIRD_INF_CSN_THRESHOLD_PKTS="${SIRD_INF_CSN_THRESHOLD_PKTS:-65535}"
DEVICE_QUEUE_MAX_SIZE="${DEVICE_QUEUE_MAX_SIZE:-17p}"
QDISC_MAX_SIZE="${QDISC_MAX_SIZE:-1000p}"

TRACE_DIR="${TRACE_DIR:-$ROOT_DIR/outputs/sird-scenarios/HomaL4Protocol-lab2-sender-congestion}"
PLOT_OUT_DIR="${PLOT_OUT_DIR:-$ROOT_DIR/output-f/lab2}"
mkdir -p "$TRACE_DIR"

export LD_LIBRARY_PATH="$ROOT_DIR/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BIN_PATH="$ROOT_DIR/build/scratch/lab2"
if [[ "$BUILD" == "1" || ! -x "$BIN_PATH" ]]; then
  echo "Building scratch/lab2..."
  ./waf build
fi

COMMON_ARGS=(
  "--enableSird=1"
  "--outputDir=$TRACE_DIR"
  "--traceMsg=$TRACE_MSG"
  "--traceProtocolCredit=$TRACE_PROTOCOL_CREDIT"
  "--traceCreditSample=$TRACE_CREDIT_SAMPLE"
  "--traceSirdCredit=$TRACE_SIRD_CREDIT"
  "--traceSirdBucket=$TRACE_SIRD_BUCKET"
  "--traceCreditEvents=$TRACE_CREDIT_EVENTS"
  "--traceSwitchEgressQueue=$TRACE_SWITCH_QUEUE"
  "--creditSampleUs=$CREDIT_SAMPLE_US"
  "--switchQueueSampleUs=$TRACE_SWITCH_QUEUE_SAMPLE_US"
  "--bdpPkts=$BDP_PKTS"
  "--startSec=$START_SEC"
  "--durationSec=$DURATION_SEC"
  "--settleTailSec=$SETTLE_TAIL_SEC"
  "--msgSizeBytes=$OUTCAST_MSG_SIZE_BYTES"
  "--flowGapUs=$FLOW_GAP_US"
  "--sendIntervalUs=$SEND_INTERVAL_US"
  "--backloggedFlow=$BACKLOGGED_FLOW"
  "--backlogDepthMsgs=$BACKLOG_DEPTH_MSGS"
  "--activeReceiverCount=$ACTIVE_RECEIVER_COUNT"
  "--deviceQueueMaxSize=$DEVICE_QUEUE_MAX_SIZE"
  "--qdiscMaxSize=$QDISC_MAX_SIZE"
)

run_case() {
  local tag="$1"
  local csn_threshold="$2"
  local sender_md="$3"
  local sender_ai="$4"
  local log_file="$TRACE_DIR/lab2_${tag}.run.log"

  rm -f \
    "$TRACE_DIR/lab2_${tag}.msg.tr" \
    "$TRACE_DIR/lab2_${tag}.sird-credit.tr" \
    "$TRACE_DIR/lab2_${tag}.sird-bucket.tr" \
    "$TRACE_DIR/lab2_${tag}.sender-credit.tr" \
    "$TRACE_DIR/lab2_${tag}.receiver-credit.tr" \
    "$TRACE_DIR/lab2_${tag}.credit-sample.tr" \
    "$TRACE_DIR/lab2_${tag}.credit-events.tr" \
    "$TRACE_DIR/lab2_${tag}.switch-egress-queue.tr"

  echo "[$tag] start $(date '+%F %T')" | tee "$log_file"
  "$BIN_PATH" \
    "--simTag=$tag" \
    "--sirdSenderCsnThresholdPkts=$csn_threshold" \
    "--sirdSenderMdFactor=$sender_md" \
    "--sirdSenderAiStep=$sender_ai" \
    "${COMMON_ARGS[@]}" >>"$log_file" 2>&1
  echo "[$tag] done $(date '+%F %T')" | tee -a "$log_file"
}

echo "profile=$PROFILE durationSec=$DURATION_SEC flowGapUs=$FLOW_GAP_US sendIntervalUs=$SEND_INTERVAL_US backloggedFlow=$BACKLOGGED_FLOW backlogDepthMsgs=$BACKLOG_DEPTH_MSGS activeReceiverCount=$ACTIVE_RECEIVER_COUNT sampleUs=$CREDIT_SAMPLE_US settleTailSec=$SETTLE_TAIL_SEC traceMsg=$TRACE_MSG traceProtocolCredit=$TRACE_PROTOCOL_CREDIT traceCreditSample=$TRACE_CREDIT_SAMPLE traceSirdCredit=$TRACE_SIRD_CREDIT traceSirdBucket=$TRACE_SIRD_BUCKET traceCreditEvents=$TRACE_CREDIT_EVENTS"
echo "outputs: $TRACE_DIR"

pids=()
run_case "feedback" "$SIRD_CSN_THRESHOLD_PKTS" 0.8 1.0 &
pids+=("$!")
run_case "no_feedback" "$SIRD_INF_CSN_THRESHOLD_PKTS" 1.0 0.0 &
pids+=("$!")

status=0
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    status=1
  fi
done

if [[ "$status" -ne 0 ]]; then
  echo "At least one lab2 run failed. Check $TRACE_DIR/lab2_*.run.log"
  exit "$status"
fi

echo "lab2 runs finished."

if [[ "$PLOT" == "1" ]]; then
  mkdir -p "$PLOT_OUT_DIR"
  rm -f "$PLOT_OUT_DIR"/lab2_*.png "$PLOT_OUT_DIR"/lab2_summary.csv
  python3 scripts/lab2_plot.py \
    --trace-dir "$TRACE_DIR" \
    --out-dir "$PLOT_OUT_DIR" \
    --feedback-tag feedback \
    --no-feedback-tag no_feedback \
    --bdp-pkts "$BDP_PKTS" \
    --sample-us "$CREDIT_SAMPLE_US" \
    --start-sec "$START_SEC"
fi
