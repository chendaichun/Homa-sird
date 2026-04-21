# SIRD Experiment Memory

Date: 2026-04-17

## Workspace

- Local repo: `/Users/dyl/Library/Mobile Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3`
- Server: `ssh r75251`
- Server repo: `/home/r75251/hx/sird/HomaL4Protocol-ns-3`
- NAS result root: `/mnt/nasDisk_ds3617/sird`
- User preference: do not run simulations locally; run experiments on the server.

## Current NAS Layout

Old result directories were moved into:

```text
/mnt/nasDisk_ds3617/sird/old
```

The current fresh lab1 run is:

```text
/mnt/nasDisk_ds3617/sird/lab1_full_duration0p3_settle0p3_8b1us_500kb50us_q1000p_bdp16p6_tracequeue_nosirdloop_fixcredit_20260417_204529
```

At creation time, the new run was active with parent PID:

```text
1690485
```

Check whether it is still running:

```bash
ssh r75251 'ps -eo pid,ppid,stat,etime,cmd | grep -E "scripts/lab1.sh full|build/scratch/lab1" | grep -v grep'
```

No output means lab1 finished and plotting should have run.

## Current Lab1 Run Parameters

The active fresh run is `lab1.sh full` with five cases:

```text
lab1_full_sird_unloaded_8B
lab1_full_sird_incast_8B_srpt
lab1_full_sird_unloaded_500KB
lab1_full_sird_incast_500KB_srpt
lab1_full_sird_incast_500KB_srr
```

Parameters:

```text
durationSec=0.3
settleTailSec=0.3
shortInterval 8B=1us
shortInterval 500KB=50us
deviceQueueMaxSize=1000p
qdiscMaxSize=1000p
bdpPkts=16.6
traceSwitchEgressQueue=1
traceLinkThroughput=1
traceSirdLoop=0
traceSirdCredit=0
ENFORCE_MSG_COMPLETE=0
```

## Important Code State

Key local files currently modified:

- `src/internet/model/homa-l4-protocol.cc`
- `src/internet/model/homa-l4-protocol.h`
- `scratch/lab1.cc`
- `scratch/lab2.cc`
- `scratch/sim1.cc`
- `scripts/lab1.sh`
- `scripts/lab1_plot.py`

Important semantic decisions:

- `RttPackets` is still the Homa attribute name, but current scenarios pass one-way BDP packets through it.
- For lab1, `bdpPkts=16.6` rounds to Homa `RttPackets=17`.
- Lab1 derives SIRD parameters from `bdpPkts`:
  - `SirdCreditBudgetPkts = round(1.5 * BDP)`, about 25 packets
  - `SirdUnschThresholdPkts = round(1.0 * BDP)`, about 17 packets
  - `SirdSenderCsnThreshold = round(0.5 * BDP)`, about 8 packets
  - `SirdQueueDisc MarkThreshold = round(1.25 * BDP)`, about 21 packets

## Fixes Under Test

The current SIRD/Homa integration fix addresses two suspected root causes of switch queue explosion:

1. Homa GRANT carries a cumulative `grantOffset`, but old SIRD accounting charged each GRANT as only `+1` credit. This undercounted in-use credits if `grantOffset` jumped by more than one packet.
2. Homa's original receive-side self-clocking advanced `m_maxGrantableIdx++` on each arriving DATA packet. For long SIRD messages, this conflicted with the SIRD credit bucket being the intended sole grant-window source.

Implemented changes:

- Added `HomaInboundMsg::GetNextGrantCreditPkts()`.
- Added `m_sirdCreditControlled` to `HomaInboundMsg`.
- For long SIRD messages, DATA arrival no longer auto-advances `m_maxGrantableIdx`.
- SIRD in-use accounting now increments by actual newly exposed packet credits, not by one per GRANT packet.

Relevant code areas:

- `HomaInboundMsg` initialization: long SIRD messages set `m_sirdCreditControlled=true`.
- `HomaInboundMsg::GetNextGrantCreditPkts()`: computes cumulative GRANT delta.
- `HomaInboundMsg::ReceiveDataPacket()`: skips auto `m_maxGrantableIdx++` when SIRD credit controlled.
- `HomaRecvScheduler::SendAppropriateGrants()`: charges `m_sirdSenderCreditsInUsePkts` and `m_sirdGlobalCreditsInUsePkts` by `grantCreditPkts`.

## Evidence From Focused Test

Focused server test after the fix:

```text
/mnt/nasDisk_ds3617/sird/old/lab1_probe_fix_creditdelta_noauto_500KB_srr_50us_q1000p_bdp16p6_20260417_202714
```

Parameters:

```text
incast_500KB_srr
shortIntervalUs=50
durationSec=0.03
settleTailSec=0.03
qdisc/device queue=1000p
bdpPkts=16.6
trace queue + sird credit + sird loop
```

Result:

```text
switch_port_to_receiver max queue = 0 packets
cap1000 samples = 0
500KB begin = 600, finish = 600
grantOffset delta max = 1
switch_to_receiver mean throughput = 70.710 Gbps
switch_to_receiver max throughput = 70.796 Gbps
```

Interpretation:

- The queue explosion was very likely caused by GRANT/window accounting mismatch, not by qdisc size alone.
- The combined fix removes queue buildup, but is conservative: receiver throughput was about 70 Gbps in the focused test, below the 100 Gbps link rate.
- Next tuning should focus on restoring throughput without reintroducing queue explosion.

## Packet Size Reminder

For lab1 point-to-point links:

- MTU: 1500 B
- Homa header: 21 B
- IPv4 header: 20 B
- Full DATA payload: `1500 - 21 - 20 = 1459 B`
- PPP header adds 2 B at serialization, so full packet serialization length is about 1502 B.

Message packet counts:

```text
500KB message: ceil(500000 / 1459) = 343 packets
10MB message: ceil(10000000 / 1459) = 6855 packets
```

## Useful Analysis Commands

Set `OUT` to a result directory, then:

```bash
ssh r75251 'OUT=/mnt/nasDisk_ds3617/sird/<result-dir>; cat "$OUT/plots/lab1_summary.csv"'
```

Queue stats:

```bash
ssh r75251 'OUT=/mnt/nasDisk_ds3617/sird/<result-dir>; awk '"'"'$2 ~ /^queue=/{q=$2; sub(/^queue=/,"",q); p=0; for(i=1;i<=NF;i++) if($i ~ /^packets=/){split($i,a,"="); p=a[2]+0} n[q]++; sum[q]+=p; if(p>max[q]){max[q]=p; tmax[q]=$1} if(p>=1000){cap[q]++; if(!(q in firstcap)) firstcap[q]=$1; lastcap[q]=$1}} END{for(q in n) printf "%s max=%d tmax_ns=%d mean=%.6f samples=%d cap1000=%d firstcap_ns=%d lastcap_ns=%d\n", q, max[q], tmax[q], sum[q]/n[q], n[q], cap[q]+0, firstcap[q]+0, lastcap[q]+0}'"'"' "$OUT"/lab1_*.switch-egress-queue.tr'
```

Message counts:

```bash
ssh r75251 'OUT=/mnt/nasDisk_ds3617/sird/<result-dir>; awk '"'"'{if($1=="+") begin[$3]++; else if($1=="-") finish[$3]++} END{for(s in begin) print s,"begin",begin[s]+0,"finish",finish[s]+0,"incomplete",begin[s]-finish[s]}'"'"' "$OUT"/lab1_*.msg.tr'
```

## Current Open Question

After queue explosion is fixed, determine how to recover goodput:

- If receiver queue remains near zero but throughput is too low, SIRD credit issuance is too conservative.
- Candidate tuning targets:
  - credit tick interval
  - global budget `B`
  - per-sender budget update rule
  - whether the receiver should expose more than one credit per tick when budget is available
  - whether SRR should distribute credits more evenly while still filling the receiver link

## 2026-04-17 Hardcoded Credit Tick Test

Change:

- Removed the configurable tick idea.
- Hardcoded `HomaRecvScheduler::GetCreditTickInterval()` to `0.5 * full DATA packet serialization time`, with a 1 ns floor.
- Synced the code to the server and rebuilt successfully with `./waf build`.

Focused run:

```text
/mnt/nasDisk_ds3617/sird/lab1_probe_tick0p5_500KB_srr_50us_q1000p_bdp16p6_20260417_210728
simTag=lab1_full_sird_incast_500KB_srr
durationSec=0.03
settleTailSec=0.03
shortIntervalUs=50
qdisc/device queue=1000p
bdpPkts=16.6
traceSwitchEgressQueue=1
traceLinkThroughput=1
traceSirdLoop=0
```

Result:

```text
500KB begin = 600, finish = 600
switch_port_to_receiver queue samples = 60000
switch_port_to_receiver max queue = 0 packets
switch_to_receiver mean inst throughput = 70.710 Gbps
switch_to_receiver max inst throughput = 70.796 Gbps
500KB p50 FCT = 24974.047 us
500KB p99 FCT = 28909.823 us
```

Interpretation:

- Shortening the receiver credit tick alone does not recover the missing throughput.
- The semantic fix still prevents switch queue buildup, but the receiver link remains underfed.
- Next suspect is no longer tick interval; focus on credit budget/RTT accounting or the condition that exposes new grantable packets.

## 2026-04-17 BDP Semantic Correction

User corrected the BDP interpretation:

```text
single-direction estimate ~= 16.66 packets
protocol/paper BDP for Homa/SIRD thresholds = RTT BDP ~= 16.66 * 2 = 33.32 packets
```

Code update:

- `scratch/lab1.cc` default `bdpPkts` changed from `16.6` to `33.32`.
- `scripts/lab1.sh` default `BDP_PKTS` changed from `16.6` to `33.32`.
- `HomaL4Protocol::RttPackets` description changed to RTT BDP.
- `lab2` and `sim1` command-line descriptions changed to RTT BDP wording.

Expected effect:

- SIRD derived thresholds in lab1 are now:
  - `B = 1.5 * 33.32 ~= 50 packets`
  - `SThr = 0.5 * 33.32 ~= 17 packets`
  - `UnschT = 1.0 * 33.32 ~= 33 packets`
  - `NThr = 1.25 * 33.32 ~= 42 packets`
- Homa's in-flight baseline also receives about `33` packets via `RttPackets`.

Focused verification after BDP correction:

```text
/mnt/nasDisk_ds3617/sird/lab1_probe_rttbdp33p32_tick0p5_500KB_srr_50us_q1000p_20260417_212747
simTag=lab1_full_sird_incast_500KB_srr
durationSec=0.03
settleTailSec=0.03
shortIntervalUs=50
qdisc/device queue=1000p
bdpPkts=33.32
traceSwitchEgressQueue=1
traceLinkThroughput=1
traceSirdLoop=0
```

Result:

```text
500KB begin = 600, finish = 600
10MB begin = 42, finish = 42
switch_port_to_receiver queue samples = 60000
switch_port_to_receiver max queue = 0 packets
switch_to_receiver mean inst throughput = 100.123 Gbps
switch_to_receiver max inst throughput = 100.144 Gbps
500KB p50 FCT = 8332.399 us
500KB p99 FCT = 11882.084 us
```

Comparison with the previous `bdpPkts=16.6` focused run:

```text
bdpPkts=16.6:  switch_to_receiver mean inst throughput = 70.710 Gbps, max queue = 0
bdpPkts=33.32: switch_to_receiver mean inst throughput = 100.123 Gbps, max queue = 0
```

Interpretation:

- The underfeeding problem was caused by using one-way BDP where RTT BDP was needed.
- With RTT BDP, the receiver link is saturated while the switch egress queue remains empty in the focused 500KB SRR test.
- This supports keeping `bdpPkts=33.32` as the lab1 default for the 100Gbps, 1us-link setup.

## 2026-04-17 Simpler Credit Accounting Test

User preferred the simpler external version of the SIRD grant-window code. Applied the following parts:

- Renamed `m_sirdCreditControlled` to `m_creditDrivenGrantWindow`.
- Removed `GetNextGrantCreditPkts()`.
- Reverted GRANT in-use accounting to one credit per GRANT:
  - `m_sirdSenderCreditsInUsePkts[senderKey]++`
  - `m_sirdGlobalCreditsInUsePkts++`
- Reverted `GetCreditTickInterval()` to one full DATA packet serialization time.
- Kept the verified RTT BDP semantics and lab1 default `bdpPkts=33.32`.

Focused verification:

```text
/mnt/nasDisk_ds3617/sird/lab1_probe_hislogic_rttbdp33p32_500KB_srr_50us_q1000p_20260417_224848
simTag=lab1_full_sird_incast_500KB_srr
durationSec=0.03
settleTailSec=0.03
shortIntervalUs=50
qdisc/device queue=1000p
bdpPkts=33.32
traceSwitchEgressQueue=1
traceLinkThroughput=1
traceSirdLoop=0
```

Result:

```text
500KB begin = 600, finish = 600
10MB begin = 42, finish = 42
switch_port_to_receiver max queue = 0 packets
switch_to_receiver mean inst throughput = 100.085 Gbps
switch_to_receiver max inst throughput = 100.141 Gbps
500KB p50 FCT = 1372.540 us
500KB p99 FCT = 2833.081 us
```

Comparison:

```text
previous delta-accounting/tick0.5 version:
  switch_to_receiver mean inst throughput = 100.123 Gbps
  500KB p50 = 8332.399 us
  500KB p99 = 11882.084 us

simpler one-GRANT-one-credit/full-tick version:
  switch_to_receiver mean inst throughput = 100.085 Gbps
  500KB p50 = 1372.540 us
  500KB p99 = 2833.081 us
```

Interpretation:

- With RTT BDP fixed, the simpler version performs better for the focused 500KB SRR test.
- It keeps receiver throughput at line rate and keeps switch egress queue at zero.
- Keep this simpler accounting unless a later case shows a cumulative-GRANT delta mismatch.

## Current Execution Policy

As of 2026-04-17, the workspace moved to an external disk, so local runs are allowed for smaller labs:

```text
lab1: may run locally
lab2: may run locally
sim1: keep running on server
```

Server path remains:

```text
r75251:/home/r75251/hx/sird/HomaL4Protocol-ns-3
```

NAS result root remains:

```text
/mnt/nasDisk_ds3617/sird
```

## 2026-04-17 Local Lab1 One-Round Smoke Run

Ran `lab1` locally with one 10MB background-flow round:

```text
output: /tmp/lab1_one_round_10MB_20260417_233441
profile: fast
durationSec: 0.004705882
settleTailSec: 0.02
longMsgSizeBytes: 10000000
longSenderRateGbps: 17.0
BDP_PKTS: 33.32
TRACE_QUEUE: 0
TRACE_LINK_THROUGHPUT: 1
```

The first attempt used an output path under the repo path containing spaces and failed because ns-3 truncated `--outputDir`; using `/tmp/...` fixed it.

Result:

```text
unloaded_8B:        started=4706 finished=4706 incomplete=0, p50=2.008 us, p99=2.008 us
incast_8B_srpt:    started=4706 finished=4706 incomplete=0, p50=3.836 us, p99=3.904 us
unloaded_500KB:    started=95   finished=95   incomplete=0, p50=50.117 us, p99=50.117 us
incast_500KB_srpt: started=95   finished=95   incomplete=0, p50=50.980 us, p99=79.870 us
incast_500KB_srr:  started=95   finished=95   incomplete=0, p50=415.700 us, p99=435.439 us
```

Background 10MB messages:

```text
incast_8B_srpt:    10MB starts=6 finishes=6, mean FCT=3143.928 us
incast_500KB_srpt: 10MB starts=6 finishes=6, mean FCT=6842.673 us
incast_500KB_srr:  10MB starts=6 finishes=6, mean FCT=8826.757 us
```

## 2026-04-17 Lab1 `last` Profile

Updated `lab1` so the longer run profile is named `last`, not `full`.

Current defaults:

```text
fast:
  durationSec = 0.005
  settleTailSec = 0.02
  SHORT_INTERVAL_8B_US = 1
  SHORT_INTERVAL_500KB_US = 500
  TARGET_PROBE_MESSAGES = 0

last:
  TARGET_PROBE_MESSAGES_LAST = 500
  SETTLE_TAIL_SEC_LAST = 0.3
  SHORT_INTERVAL_8B_US = 1
  SHORT_INTERVAL_500KB_US = 500
```

For `last`, `scripts/lab1.sh` computes each case's `durationSec` from the probe interval:

```text
durationSec = TARGET_PROBE_MESSAGES_LAST * shortIntervalUs / 1e6
```

Therefore the default `last` injection windows are:

```text
8B cases:    500 * 1 us   = 0.0005 s
500KB cases: 500 * 500 us = 0.25 s
```

`scratch/lab1.cc` now accepts `--targetProbeMessages`; `0` keeps the old duration-only behavior, while nonzero caps the probe sender after that many messages. Background 10MB senders still run until the case's computed `durationSec`.

Local validation:

```text
TARGET_PROBE_MESSAGES_LAST=5 PLOT=0 BUILD=0 OUT_DIR=/tmp/lab1_last_target5_smoke_20260417_234355 bash scripts/lab1.sh last
```

All five cases completed with `started=5 finished=5`.

Also updated `lab1.sh` so if `OUT_DIR` is unset, trace output defaults to the relative repo-local path:

```text
outputs/sird-scenarios/HomaL4Protocol-lab1-receiver-congestion
```

This keeps output under the current repo `outputs/` directory while avoiding spaces in the `--outputDir` argument. Explicit `OUT_DIR` values containing spaces are rejected early because ns-3 trace output handling truncates those paths.

Full local `last` run completed:

```text
output: outputs/sird-scenarios/lab1_last_probeonly_20260417_235904
ENFORCE_MSG_COMPLETE: 0
```

Probe results:

```text
unloaded_8B:        started=500 finished=500 incomplete=0, p50=2.008 us,    p99=2.008 us
incast_8B_srpt:    started=500 finished=500 incomplete=0, p50=3.032 us,    p99=3.896 us
unloaded_500KB:    started=500 finished=500 incomplete=0, p50=50.117 us,   p99=50.117 us
incast_500KB_srpt: started=500 finished=500 incomplete=0, p50=50.979 us,   p99=84.102 us
incast_500KB_srr:  started=500 finished=500 incomplete=0, p50=2072.420 us, p99=3809.273 us
```

Background 10MB messages:

```text
incast_8B_srpt:    10MB starts=6   finishes=6
incast_500KB_srpt: 10MB starts=324 finishes=324
incast_500KB_srr:  10MB starts=324 finishes=324
```
