# Lab2 Trace Guide

This document describes the trace files produced by [`scratch/lab2.cc`](/Users/dyl/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3/scratch/lab2.cc), how they are enabled from [`scripts/lab2.sh`](/Users/dyl/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3/scripts/lab2.sh), and what analyses or plots each trace can support.

`lab2` is the sender-congestion / sender-information outcast scenario:

- 1 sender
- 3 receivers
- 1 switch
- 100 Gbps links
- 1 us per-hop delay

The default file prefix is:

```text
<outputDir>/lab2_<tag>.*
```

where `<tag>` is usually `feedback` or `no_feedback`.

## Trace Inventory

`lab2` currently knows how to emit these trace families:

1. `msg.tr`
2. `sird-credit.tr`
3. `sird-bucket.tr`
4. `credit-events.tr`
5. `credit-series.tr`
6. `switch-egress-queue.tr`
7. `run.log`

Not all of them are enabled in every run. The current `fast` server run uses:

- `traceMsg=1`
- `traceCreditSeries=1`
- `traceSwitchEgressQueue=1`
- `traceSirdCredit=0`
- `traceSirdBucket=0`
- `traceCreditEvents=0`

So that run will definitely produce:

- `lab2_<tag>.msg.tr`
- `lab2_<tag>.credit-series.tr`
- `lab2_<tag>.switch-egress-queue.tr`
- `lab2_<tag>.run.log`

## 1. `msg.tr`

Produced by:

- `traceMsg=1` in [`scratch/lab2.cc`](/Users/dyl/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3/scratch/lab2.cc:468)

Callbacks:

- `MsgBegin`
- `MsgFinish`

Format:

```text
+ <time_ns> <msg_size> <src_ip:src_port> <dst_ip:dst_port> <txMsgId>
- <time_ns> <msg_size> <src_ip:src_port> <dst_ip:dst_port> <txMsgId>
```

Meaning:

- `+` means a message starts at the sender-side Homa stack
- `-` means the same message finishes at the receiver-side Homa stack

What it is useful for:

- FCT calculation
- per-flow completion counting
- checking whether one case finishes fewer messages than another
- validating that the staggered-start traffic pattern is actually happening

What plots it can support:

- message FCT CDF
- message completion timeline
- started vs finished counts
- per-receiver completion rate over time

Current status in `lab2_plot.py`:

- not consumed yet
- available if we want to add FCT-oriented diagnostics later

## 2. `sird-credit.tr`

Produced by:

- `traceSirdCredit=1` in [`scratch/lab2.cc`](/Users/dyl/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3/scratch/lab2.cc:477)

Source trace:

- `SirdGrantDecision`

Format:

```text
<time_ns> sender=<ip> txMsgId=<id> grantOffset=<pkt> senderBudgetPkts=<value> ecnEwma=<value> senderCsn=<0|1>
```

Meaning:

- one receiver has decided to grant more packets to one sender message
- `senderBudgetPkts` is the receiver's current view of the sender-side budget
- `senderCsn` shows whether the sender was signaling congestion in feedback

What it is useful for:

- direct observation of receiver grant behavior
- direct observation of `senderBudgetPkts`
- checking whether sender feedback suppresses budget over time
- correlating budget reduction with CSN activity

What plots it can support:

- sender budget time series
- budget comparison between `feedback` and `no_feedback`
- CSN-marked vs non-CSN grant overlays
- grant pacing over time

Current status in `lab2_plot.py`:

- parsed by `parse_sird_budget()`
- used to generate `lab2_sender_budget_timeseries.png`
- if this file is absent, that budget plot is skipped

## 3. `sird-bucket.tr`

Produced by:

- `traceSirdBucket=1` in [`scratch/lab2.cc`](/Users/dyl/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3/scratch/lab2.cc:484)

Source trace:

- `SirdBucketState`

Format:

```text
<time_ns> receiver=<ip> sender=<ip> eventType=<n> senderBudgetHostPkts=<value> senderInUsePkts=<value> senderAvailPkts=<value> globalInUsePkts=<value> globalBudgetPkts=<value> globalAvailPkts=<value>
```

Meaning:

- snapshot of receiver-side bucket state around a sender
- `sender*` fields are per-sender host-loop state
- `global*` fields are per-receiver aggregate credit state

What it is useful for:

- checking credit conservation
- checking whether per-sender and global limits bind at different times
- debugging cases where queue is large but grants do not advance
- understanding whether the sender or the receiver bucket is the bottleneck

What plots it can support:

- per-sender available credit over time
- global available credit over time
- stacked plots of in-use vs available credit
- event-type annotated bucket dynamics

Current status in `lab2_plot.py`:

- not consumed yet
- mainly a debugging and mechanism-validation trace today

## 4. `credit-events.tr`

Produced by:

- `traceCreditEvents=1` in [`scratch/lab2.cc`](/Users/dyl/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3/scratch/lab2.cc:491)

Source callbacks:

- `SirdGrantDecision` converted into `type=grant`
- `DataPktArrival` converted into `type=data`

Formats:

Grant event:

```text
<time_ns> type=grant recvNode=<id> sender=<ip> txMsgId=<id> grantOffset=<pkt> senderBudgetPkts=<value> ecnEwma=<value> senderCsn=<0|1>
```

Data event:

```text
<time_ns> type=data recvNode=<id> sender=<ip> receiver=<ip> txMsgId=<id> pktOffset=<pkt> prio=<n> size=<bytes>
```

Meaning:

- `grant` increases outstanding credits for a `(receiver, sender)` pair
- `data` consumes outstanding credits when a packet arrives

What it is useful for:

- reconstructing sender-accumulated credit and receiver-available credit offline
- reproducing Figure-4-style sender-information dynamics without relying on in-simulator sampled summaries
- debugging whether sampled series match true event-level dynamics

What plots it can support:

- the same credit-dynamics plots as `credit-series.tr`
- event-driven rather than sampled reconstructions
- receiver-by-receiver credit occupancy

Current status in `lab2_plot.py`:

- fallback input if `credit-series.tr` does not exist
- parsed by `parse_credit_events()`
- reconstructed through `derive_credit_series()`

## 5. `credit-series.tr`

Produced by:

- `traceCreditSeries=1` in [`scratch/lab2.cc`](/Users/dyl/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3/scratch/lab2.cc:529)

Produced by periodic sampling of derived compact state.

Format:

```text
<time_ns> senderAccumPkts=<value> receiverAvailPkts=<value> senderAccumXbdp=<value> receiverAvailXbdp=<value> targetSender=<sender_key>
```

Meaning:

- `senderAccumPkts`: credits accumulated at the sender across the three receivers
- `receiverAvailPkts`: total receiver-side credit still available across the three receivers
- `senderAccumXbdp`: sender accumulation normalized by BDP
- `receiverAvailXbdp`: receiver availability normalized by BDP

This is the most compact trace for the main `lab2` story.

What it is useful for:

- direct plotting of sender-information dynamics
- comparing `feedback` vs `no_feedback`
- checking convergence in units of BDP instead of raw packets
- generating summary statistics such as mean/final xBDP

What plots it can support:

- sender accumulated credit time series
- total receiver available credit time series
- two-panel feedback vs no-feedback comparison
- compact CSV summaries

Current status in `lab2_plot.py`:

- primary input
- parsed by `parse_credit_series()`
- used to produce `lab2_sender_credit_dynamics.png`
- used to produce `lab2_summary.csv`

## 6. `switch-egress-queue.tr`

Produced by:

- `traceSwitchEgressQueue=1` in [`scratch/lab2.cc`](/Users/dyl/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3/scratch/lab2.cc:544)

Sampled queues:

- `switch_port_to_sender`
- `switch_port_to_receiver1`
- `switch_port_to_receiver2`
- `switch_port_to_receiver3`

Format:

```text
<time_ns> queue=<label> packets=<n> bytes=<n>
```

Meaning:

- sampled occupancy of each switch egress queue
- for sender-congestion analysis, the most important queue is `switch_port_to_sender`

What it is useful for:

- verifying that the sender-facing switch port is the actual bottleneck
- correlating sender accumulated credit with queue buildup
- checking whether sender feedback smooths sender-facing queue occupancy
- comparing queue amplitude and persistence between `feedback` and `no_feedback`

What plots it can support:

- sender-facing queue time series
- per-port queue comparison
- queue occupancy CDF
- queue peak and mean summaries

Current status in `lab2_plot.py`:

- parsed by `parse_switch_sender_queue()`
- only `queue=switch_port_to_sender` is used
- produces `lab2_sender_switch_egress_queue.png`

## 7. `run.log`

Produced by:

- [`scripts/lab2.sh`](/Users/dyl/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3/scripts/lab2.sh:84)

Contents:

- per-case start timestamp
- stdout/stderr from `build/scratch/lab2`
- per-case done timestamp

What it is useful for:

- checking whether a run finished cleanly
- checking which flags were actually used
- diagnosing crashes or plotting failures

What plots it can support:

- none directly

## How `lab2_plot.py` Builds the Result Figures

The plotting pipeline is:

1. read `credit-series.tr` for `feedback`
2. read `credit-series.tr` for `no_feedback`
3. if either one is missing, try `credit-events.tr` and reconstruct compact series
4. optionally read `sird-credit.tr` and generate sender-budget figure
5. read `switch-egress-queue.tr` and keep only `switch_port_to_sender`
6. write plots and `lab2_summary.csv`

Code path:

- main entry: [`scripts/lab2_plot.py`](/Users/dyl/Library/Mobile%20Documents/com~apple~CloudDocs/Desktop/bishe/code/HomaL4Protocol-ns-3/scripts/lab2_plot.py:379)

### Figure 1: `lab2_sender_credit_dynamics.png`

Inputs:

- primary: `credit-series.tr`
- fallback: `credit-events.tr`

Panels:

- left: sender accumulated credit in xBDP
- right: total receiver available credit in xBDP

Generated by:

- `plot_credit_dynamics()`

This is the main figure for the sender-information story.

### Figure 2: `lab2_sender_budget_timeseries.png`

Inputs:

- `sird-credit.tr`

Generated by:

- `parse_sird_budget()`
- `plot_sender_budget()`

This figure exists only if `traceSirdCredit=1`.

### Figure 3: `lab2_sender_switch_egress_queue.png`

Inputs:

- `switch-egress-queue.tr`

Generated by:

- `parse_switch_sender_queue()`
- `plot_switch_sender_queue()`

Only the sender-facing switch queue is plotted in the current version.

### Summary: `lab2_summary.csv`

Inputs:

- same compact credit series used for Figure 1

Fields:

- `case`
- `target_sender`
- `receiver_nodes`
- `samples`
- `mean_sender_accum_xbdp`
- `final_sender_accum_xbdp`
- `mean_receiver_avail_xbdp`
- `final_receiver_avail_xbdp`

Generated by:

- `write_summary()`

## Recommended Trace Sets

### Minimum useful set

Use this for fast iteration:

- `traceMsg=1`
- `traceCreditSeries=1`
- `traceSwitchEgressQueue=1`

This is enough for:

- sender credit dynamics
- sender-facing switch queue plot
- compact summary CSV

### Mechanism-validation set

Use this when validating the control logic itself:

- `traceMsg=1`
- `traceCreditSeries=1`
- `traceCreditEvents=1`
- `traceSirdCredit=1`
- `traceSirdBucket=1`
- `traceSwitchEgressQueue=1`

This adds:

- event-level reconstruction
- sender budget evolution
- bucket-state debugging

## Current Gaps

There are a few traces that exist but are not fully exploited yet:

1. `msg.tr` is not yet turned into FCT plots in `lab2_plot.py`
2. `sird-bucket.tr` is not yet visualized
3. `switch-egress-queue.tr` contains all switch ports, but the plotter only uses the sender-facing one
4. `credit-events.tr` is currently a fallback path, not the primary source

If needed, the next logical additions are:

- FCT CDF from `msg.tr`
- multi-port switch queue panels
- bucket-state plots from `sird-bucket.tr`
- event-level grant/data raster plots from `credit-events.tr`
