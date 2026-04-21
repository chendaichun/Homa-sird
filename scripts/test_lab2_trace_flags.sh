#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

grep -q 'TRACE_MSG="${TRACE_MSG:-' scripts/lab2.sh ||
  fail "scripts/lab2.sh is missing TRACE_MSG env wiring"

grep -q '"--traceMsg=\$TRACE_MSG"' scripts/lab2.sh ||
  fail "scripts/lab2.sh does not pass --traceMsg=\$TRACE_MSG"

grep -q 'TRACE_PROTOCOL_CREDIT="${TRACE_PROTOCOL_CREDIT:-' scripts/lab2.sh ||
  fail "scripts/lab2.sh is missing TRACE_PROTOCOL_CREDIT env wiring"

grep -q '"--traceProtocolCredit=\$TRACE_PROTOCOL_CREDIT"' scripts/lab2.sh ||
  fail "scripts/lab2.sh does not pass --traceProtocolCredit=\$TRACE_PROTOCOL_CREDIT"

grep -q 'bool traceProtocolCredit =' scratch/lab2.cc ||
  fail "scratch/lab2.cc is missing traceProtocolCredit flag"

grep -q 'cmd.AddValue ("traceProtocolCredit"' scratch/lab2.cc ||
  fail "scratch/lab2.cc is missing traceProtocolCredit CLI flag"

grep -q 'if (enableSird && traceProtocolCredit)' scratch/lab2.cc ||
  fail "sender/receiver protocol credit traces are not gated by traceProtocolCredit"

grep -q 'g_creditEventStream = ascii.CreateFileStream (prefix.str () + ".credit-events.tr")' scratch/lab2.cc ||
  fail "credit-events trace creation disappeared unexpectedly"

echo "PASS"
