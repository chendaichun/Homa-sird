#!/usr/bin/env python3
"""Plot lab2 sender-congestion / sender-information traces.

Expected input files are produced by scripts/lab2.sh through
scratch/lab2.cc:
  lab2_<tag>.credit-sample.tr
  lab2_<tag>.sender-credit.tr
  lab2_<tag>.receiver-credit.tr
  lab2_<tag>.switch-egress-queue.tr (optional)
  lab2_<tag>.sird-credit.tr (optional)
"""

from __future__ import annotations

import argparse
import csv
from collections import Counter, defaultdict
from pathlib import Path
from typing import DefaultDict, Dict, Iterable, List, Optional, Set, Tuple

import matplotlib.pyplot as plt


Event = Tuple[int, str, int, str]
CreditSample = Tuple[int, str, float, float]
SenderCreditEvent = Tuple[int, str, str, int, int]
ReceiverCreditEvent = Tuple[int, str, str, int, int, int, int]


def parse_kv_fields(tokens: Iterable[str]) -> Dict[str, str]:
    fields: Dict[str, str] = {}
    for token in tokens:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def parse_credit_events(path: Path) -> List[Event]:
    events: List[Event] = []
    if not path.exists():
        return events

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv_fields(parts[1:])
            if "type" not in fields or "recvNode" not in fields or "sender" not in fields:
                continue
            try:
                recv_node = int(fields["recvNode"])
            except ValueError:
                continue
            events.append((time_ns, fields["type"], recv_node, fields["sender"]))

    events.sort(key=lambda row: row[0])
    return events


def parse_credit_sample(path: Path) -> List[CreditSample]:
    rows: List[CreditSample] = []
    if not path.exists():
        return rows

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv_fields(parts[1:])
            required = {"sender", "senderCreditXbdp", "receiverAvailXbdp"}
            if not required.issubset(fields):
                continue
            try:
                sender_credit_xbdp = float(fields["senderCreditXbdp"])
                receiver_avail_xbdp = float(fields["receiverAvailXbdp"])
            except ValueError:
                continue
            rows.append((time_ns, fields["sender"], sender_credit_xbdp, receiver_avail_xbdp))

    rows.sort(key=lambda row: row[0])
    return rows


def parse_sender_credit(path: Path) -> List[SenderCreditEvent]:
    rows: List[SenderCreditEvent] = []
    if not path.exists():
        return rows

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv_fields(parts[1:])
            required = {"sender", "receiver", "txMsgId", "senderCreditPkts", "eventType"}
            if not required.issubset(fields):
                continue
            try:
                tx_msg_id = int(fields["txMsgId"])
                sender_credit_pkts = int(fields["senderCreditPkts"])
                event_type = int(fields["eventType"])
            except ValueError:
                continue
            rows.append(
                (
                    time_ns,
                    fields["sender"],
                    fields["receiver"],
                    tx_msg_id,
                    sender_credit_pkts,
                )
            )

    rows.sort(key=lambda row: row[0])
    return rows


def parse_receiver_credit(path: Path) -> List[ReceiverCreditEvent]:
    rows: List[ReceiverCreditEvent] = []
    if not path.exists():
        return rows

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv_fields(parts[1:])
            required = {
                "receiver",
                "sender",
                "receiverAvailPkts",
                "receiverBudgetPkts",
                "senderAvailPkts",
                "senderBudgetPkts",
            }
            if not required.issubset(fields):
                continue
            try:
                receiver_avail_pkts = int(fields["receiverAvailPkts"])
                receiver_budget_pkts = int(fields["receiverBudgetPkts"])
                sender_avail_pkts = int(fields["senderAvailPkts"])
                sender_budget_pkts = int(fields["senderBudgetPkts"])
            except ValueError:
                continue
            rows.append(
                (
                    time_ns,
                    fields["receiver"],
                    fields["sender"],
                    receiver_avail_pkts,
                    receiver_budget_pkts,
                    sender_avail_pkts,
                    sender_budget_pkts,
                )
            )

    rows.sort(key=lambda row: row[0])
    return rows


def moving_average(values: List[float], window: int) -> List[float]:
    if window <= 1:
        return values[:]
    result: List[float] = []
    running = 0.0
    queue: List[float] = []
    for value in values:
        queue.append(value)
        running += value
        if len(queue) > window:
            running -= queue.pop(0)
        result.append(running / len(queue))
    return result


def pick_target_sender(
    sender_events: List[SenderCreditEvent],
    receiver_events: List[ReceiverCreditEvent],
) -> Optional[str]:
    senders: Counter[str] = Counter()
    for _, sender, _, _, _ in sender_events:
        senders[sender] += 1
    for _, _, sender, _, _, _, _ in receiver_events:
        senders[sender] += 1
    if not senders:
        return None
    return senders.most_common(1)[0][0]


def derive_credit_series_from_samples(
    samples: List[CreditSample],
    dt_s: float,
    ma_s: float,
    start_sec: Optional[float] = None,
) -> Optional[Tuple[List[float], List[float], List[float], str, List[int]]]:
    if not samples:
        return None

    target_sender = Counter(row[1] for row in samples).most_common(1)[0][0]
    filtered = [row for row in samples if row[1] == target_sender]
    if not filtered:
        return None

    start_ns = int(start_sec * 1e9) if start_sec is not None else filtered[0][0]
    filtered = [row for row in filtered if row[0] >= start_ns]
    if not filtered:
        return None

    xs = [row[0] / 1e9 for row in filtered]
    sender_credit_xbdp = [row[2] for row in filtered]
    receiver_avail_xbdp = [row[3] for row in filtered]

    ma_window = max(1, int(round(ma_s / dt_s)))
    return (
        xs,
        moving_average(sender_credit_xbdp, ma_window),
        moving_average(receiver_avail_xbdp, ma_window),
        target_sender,
        [],
    )


def derive_credit_series_from_protocol_traces(
    sender_events: List[SenderCreditEvent],
    receiver_events: List[ReceiverCreditEvent],
    bdp_pkts: float,
    dt_s: float,
    ma_s: float,
    start_sec: Optional[float] = None,
) -> Optional[Tuple[List[float], List[float], List[float], str, List[int]]]:
    target_sender = pick_target_sender(sender_events, receiver_events)
    if target_sender is None:
        return None

    sender_times = [row[0] for row in sender_events]
    receiver_times = [row[0] for row in receiver_events]
    if not sender_times and not receiver_times:
        return None

    all_times = sender_times + receiver_times
    t0 = min(all_times)
    t_end = max(all_times)
    sample_ns = max(1, int(dt_s * 1e9))
    start_ns = max(t0, int(start_sec * 1e9)) if start_sec is not None else t0
    if start_ns > t_end:
        return None

    sender_idx = 0
    receiver_idx = 0
    current_sender_credit: Dict[Tuple[str, str, int], int] = {}
    initial_receiver_budget: Dict[Tuple[str, str], int] = {}
    receiver_nodes: Set[str] = set()
    for _, receiver, sender, _, receiver_budget_pkts, _, _ in receiver_events:
        if sender != target_sender:
            continue
        initial_receiver_budget[(receiver, sender)] = receiver_budget_pkts
        receiver_nodes.add(receiver)
    current_receiver_avail = initial_receiver_budget.copy()

    xs: List[float] = []
    sender_credit_xbdp: List[float] = []
    receiver_avail_xbdp: List[float] = []

    next_sample = start_ns
    while next_sample <= t_end:
        while sender_idx < len(sender_events) and sender_events[sender_idx][0] <= next_sample:
            _, sender, receiver, tx_msg_id, sender_credit_pkts = sender_events[sender_idx]
            current_sender_credit[(sender, receiver, tx_msg_id)] = sender_credit_pkts
            sender_idx += 1

        while receiver_idx < len(receiver_events) and receiver_events[receiver_idx][0] <= next_sample:
            _, receiver, sender, receiver_avail_pkts, _, _, _ = receiver_events[receiver_idx]
            current_receiver_avail[(receiver, sender)] = receiver_avail_pkts
            if sender == target_sender:
                receiver_nodes.add(receiver)
            receiver_idx += 1

        sender_credit_pkts = sum(
            credit
            for (sender, _, _), credit in current_sender_credit.items()
            if sender == target_sender
        )
        receiver_avail_pkts = sum(
            current_receiver_avail.get((receiver, target_sender), 0) for receiver in receiver_nodes
        )

        xs.append(next_sample / 1e9)
        sender_credit_xbdp.append(sender_credit_pkts / float(bdp_pkts))
        receiver_avail_xbdp.append(receiver_avail_pkts / float(bdp_pkts))
        next_sample += sample_ns

    ma_window = max(1, int(round(ma_s / dt_s)))
    return (
        xs,
        moving_average(sender_credit_xbdp, ma_window),
        moving_average(receiver_avail_xbdp, ma_window),
        target_sender,
        [],
    )


def derive_credit_series(
    events: List[Event],
    bdp_pkts: float,
    budget_pkts: int,
    dt_s: float,
    ma_s: float,
) -> Optional[Tuple[List[float], List[float], List[float], str, List[int]]]:
    if not events:
        return None

    grants_by_sender: Counter[str] = Counter()
    grants_by_receiver: Counter[int] = Counter()
    for _, event_type, recv_node, sender in events:
        if event_type == "grant":
            grants_by_sender[sender] += 1
            grants_by_receiver[recv_node] += 1

    if not grants_by_sender or not grants_by_receiver:
        return None

    target_sender = grants_by_sender.most_common(1)[0][0]
    receivers = [node for node, _ in grants_by_receiver.most_common(3)]

    outstanding: DefaultDict[Tuple[int, str], int] = defaultdict(int)
    total_by_receiver: DefaultDict[int, int] = defaultdict(int)

    t0 = events[0][0]
    t_end = events[-1][0]
    sample_ns = max(1, int(dt_s * 1e9))
    next_sample = t0
    index = 0

    xs: List[float] = []
    sender_accum_bdp: List[float] = []
    receiver_avail_bdp: List[float] = []

    while next_sample <= t_end:
        while index < len(events) and events[index][0] <= next_sample:
            _, event_type, recv_node, sender = events[index]
            key = (recv_node, sender)
            if event_type == "grant":
                outstanding[key] += 1
                total_by_receiver[recv_node] += 1
            elif event_type == "data":
                if outstanding[key] > 0:
                    outstanding[key] -= 1
                    total_by_receiver[recv_node] = max(0, total_by_receiver[recv_node] - 1)
            index += 1

        sender_accum = sum(outstanding[(recv_node, target_sender)] for recv_node in receivers)
        receiver_avail = sum(
            max(0, budget_pkts - total_by_receiver[recv_node]) for recv_node in receivers
        )

        xs.append((next_sample - t0) / 1e9)
        sender_accum_bdp.append(sender_accum / float(bdp_pkts))
        receiver_avail_bdp.append(receiver_avail / float(bdp_pkts))
        next_sample += sample_ns

    ma_window = max(1, int(round(ma_s / dt_s)))
    return (
        xs,
        moving_average(sender_accum_bdp, ma_window),
        moving_average(receiver_avail_bdp, ma_window),
        target_sender,
        receivers,
    )


def parse_sird_budget(path: Path) -> Dict[str, List[Tuple[float, float, int]]]:
    rows: Dict[str, List[Tuple[float, float, int]]] = defaultdict(list)
    if not path.exists():
        return rows

    first_ns: Optional[int] = None
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv_fields(parts[1:])
            if "sender" not in fields or "senderBudgetPkts" not in fields:
                continue
            try:
                budget = float(fields["senderBudgetPkts"])
                csn = int(fields.get("senderCsn", "0"))
            except ValueError:
                continue
            if first_ns is None:
                first_ns = time_ns
            rows[fields["sender"]].append(((time_ns - first_ns) / 1e9, budget, csn))
    return rows


def parse_switch_sender_queue(path: Path) -> List[Tuple[float, float]]:
    rows: List[Tuple[float, float]] = []
    if not path.exists():
        return rows

    first_ns: Optional[int] = None
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if not parts:
                continue
            try:
                time_ns = int(parts[0])
            except ValueError:
                continue
            fields = parse_kv_fields(parts[1:])
            if fields.get("queue") != "switch_port_to_sender":
                continue
            try:
                packets = float(fields["packets"])
            except (KeyError, ValueError):
                continue
            if first_ns is None:
                first_ns = time_ns
            rows.append(((time_ns - first_ns) / 1e9, packets))
    return rows


def pick_sender(rows: Dict[str, List[Tuple[float, float, int]]]) -> Optional[str]:
    if not rows:
        return None
    return max(rows.keys(), key=lambda sender: len(rows[sender]))


def apply_start_padding(ax: plt.Axes, xs_sets: Iterable[List[float]], start_sec: Optional[float]) -> None:
    if start_sec is None:
        return

    xmax: Optional[float] = None
    for xs in xs_sets:
        visible = [x for x in xs if x >= start_sec]
        if not visible:
            continue
        series_max = max(visible)
        xmax = series_max if xmax is None else max(xmax, series_max)

    if xmax is None or xmax <= start_sec:
        ax.set_xlim(left=start_sec)
        return

    pad = (xmax - start_sec) / 15.0
    ax.set_xlim(left=start_sec - pad, right=xmax)


def plot_credit_dynamics(
    out_dir: Path,
    feedback: Tuple[List[float], List[float], List[float], str, List[int]],
    no_feedback: Tuple[List[float], List[float], List[float], str, List[int]],
    start_sec: Optional[float],
) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(11.5, 4.7))

    line_alpha = 0.72

    axes[0].plot(feedback[0], feedback[1], label="SIRD feedback", linewidth=2.2, alpha=line_alpha)
    axes[0].plot(no_feedback[0], no_feedback[1], label="No sender feedback", linewidth=2.2, alpha=line_alpha)
    axes[0].set_xlabel("Time (s)")
    axes[0].set_ylabel("Credit (xBDP)")
    axes[0].set_title("Credit held at sender")
    apply_start_padding(axes[0], [feedback[0], no_feedback[0]], start_sec)
    axes[0].grid(True, alpha=0.3)
    axes[0].legend()

    axes[1].plot(feedback[0], feedback[2], label="SIRD feedback", linewidth=2.2, alpha=line_alpha)
    axes[1].plot(no_feedback[0], no_feedback[2], label="No sender feedback", linewidth=2.2, alpha=line_alpha)
    axes[1].set_xlabel("Time (s)")
    axes[1].set_ylabel("Credit (xBDP)")
    axes[1].set_title("Total receiver credit available")
    apply_start_padding(axes[1], [feedback[0], no_feedback[0]], start_sec)
    axes[1].grid(True, alpha=0.3)
    axes[1].legend()

    fig.suptitle("Lab2 Sender Congestion / Sender Information", y=1.03)
    fig.tight_layout()
    fig.savefig(out_dir / "lab2_sender_credit_dynamics.png", dpi=180, bbox_inches="tight")
    plt.close(fig)


def plot_sender_budget(
    out_dir: Path,
    feedback_budget: Dict[str, List[Tuple[float, float, int]]],
    no_feedback_budget: Dict[str, List[Tuple[float, float, int]]],
) -> None:
    plt.figure(figsize=(8.5, 4.8))
    plotted = False

    sender = pick_sender(feedback_budget)
    if sender is not None:
        xs = [row[0] for row in feedback_budget[sender]]
        ys = [row[1] for row in feedback_budget[sender]]
        if xs:
            plotted = True
            plt.plot(xs, ys, label=f"SIRD feedback ({sender})", linewidth=1.8)

    sender = pick_sender(no_feedback_budget)
    if sender is not None:
        xs = [row[0] for row in no_feedback_budget[sender]]
        ys = [row[1] for row in no_feedback_budget[sender]]
        if xs:
            plotted = True
            plt.plot(xs, ys, label=f"No sender feedback ({sender})", linewidth=1.8)

    if not plotted:
        plt.close()
        return

    plt.xlabel("Time (s)")
    plt.ylabel("senderBudgetPkts")
    plt.title("Receiver per-sender budget")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "lab2_sender_budget_timeseries.png", dpi=180)
    plt.close()


def plot_switch_sender_queue(
    out_dir: Path,
    feedback_rows: List[Tuple[float, float]],
    no_feedback_rows: List[Tuple[float, float]],
) -> None:
    plt.figure(figsize=(8.5, 4.8))
    plotted = False
    if feedback_rows:
        xs, ys = zip(*feedback_rows)
        plt.plot(xs, ys, label="SIRD feedback", linewidth=1.5)
        plotted = True
    if no_feedback_rows:
        xs, ys = zip(*no_feedback_rows)
        plt.plot(xs, ys, label="No sender feedback", linewidth=1.5)
        plotted = True
    if not plotted:
        plt.close()
        return
    plt.xlabel("Time (s)")
    plt.ylabel("Switch egress queue to sender (packets)")
    plt.title("Sender-facing switch egress queue")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_dir / "lab2_sender_switch_egress_queue.png", dpi=180)
    plt.close()


def write_summary(
    out_dir: Path,
    feedback: Tuple[List[float], List[float], List[float], str, List[int]],
    no_feedback: Tuple[List[float], List[float], List[float], str, List[int]],
) -> None:
    path = out_dir / "lab2_summary.csv"
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "case",
                "target_sender",
                "receiver_nodes",
                "samples",
                "mean_sender_credit_xbdp",
                "final_sender_credit_xbdp",
                "mean_receiver_avail_xbdp",
                "final_receiver_avail_xbdp",
            ]
        )
        for label, series in [("feedback", feedback), ("no_feedback", no_feedback)]:
            xs, sender_accum, receiver_avail, target_sender, receivers = series
            writer.writerow(
                [
                    label,
                    target_sender,
                    " ".join(str(r) for r in receivers),
                    len(xs),
                    sum(sender_accum) / len(sender_accum) if sender_accum else "",
                    sender_accum[-1] if sender_accum else "",
                    sum(receiver_avail) / len(receiver_avail) if receiver_avail else "",
                    receiver_avail[-1] if receiver_avail else "",
                ]
            )


def load_credit_series(
    trace_dir: Path,
    tag: str,
    bdp_pkts: float,
    budget_pkts: int,
    sample_sec: float,
    ma_sec: float,
    start_sec: Optional[float],
) -> Optional[Tuple[List[float], List[float], List[float], str, List[int]]]:
    sample_series = derive_credit_series_from_samples(
        parse_credit_sample(trace_dir / f"lab2_{tag}.credit-sample.tr"),
        sample_sec,
        ma_sec,
        start_sec,
    )
    if sample_series is not None:
        return sample_series

    protocol_series = derive_credit_series_from_protocol_traces(
        parse_sender_credit(trace_dir / f"lab2_{tag}.sender-credit.tr"),
        parse_receiver_credit(trace_dir / f"lab2_{tag}.receiver-credit.tr"),
        bdp_pkts,
        sample_sec,
        ma_sec,
        start_sec,
    )
    if protocol_series is not None:
        return protocol_series

    events = parse_credit_events(trace_dir / f"lab2_{tag}.credit-events.tr")
    return derive_credit_series(
        events,
        bdp_pkts,
        budget_pkts,
        sample_sec,
        ma_sec,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Plot lab2 sender-congestion traces.")
    parser.add_argument(
        "--trace-dir",
        type=Path,
        default=Path("outputs/sird-scenarios/HomaL4Protocol-lab2-sender-congestion"),
    )
    parser.add_argument("--out-dir", type=Path, default=Path("output-f/lab2"))
    parser.add_argument("--feedback-tag", default="lab2_feedback")
    parser.add_argument("--no-feedback-tag", default="lab2_no_feedback")
    parser.add_argument("--bdp-pkts", type=float, default=33.32)
    parser.add_argument("--budget-pkts", type=int, default=36)
    parser.add_argument("--sample-sec", type=float, default=0.01)
    parser.add_argument("--sample-us", type=float, default=None)
    parser.add_argument("--ma-sec", type=float, default=0.1)
    parser.add_argument("--start-sec", type=float, default=None)
    args = parser.parse_args()

    if args.sample_us is not None:
        args.sample_sec = args.sample_us / 1e6

    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    feedback = load_credit_series(
        args.trace_dir,
        args.feedback_tag,
        args.bdp_pkts,
        args.budget_pkts,
        args.sample_sec,
        args.ma_sec,
        args.start_sec,
    )
    no_feedback = load_credit_series(
        args.trace_dir,
        args.no_feedback_tag,
        args.bdp_pkts,
        args.budget_pkts,
        args.sample_sec,
        args.ma_sec,
        args.start_sec,
    )

    if feedback is None or no_feedback is None:
        raise SystemExit("Missing lab2 credit-sample, sender-credit/receiver-credit, or credit-events traces.")

    plot_credit_dynamics(out_dir, feedback, no_feedback, args.start_sec)

    feedback_budget = parse_sird_budget(args.trace_dir / f"lab2_{args.feedback_tag}.sird-credit.tr")
    no_feedback_budget = parse_sird_budget(args.trace_dir / f"lab2_{args.no_feedback_tag}.sird-credit.tr")
    plot_sender_budget(out_dir, feedback_budget, no_feedback_budget)
    feedback_queue = parse_switch_sender_queue(args.trace_dir / f"lab2_{args.feedback_tag}.switch-egress-queue.tr")
    no_feedback_queue = parse_switch_sender_queue(args.trace_dir / f"lab2_{args.no_feedback_tag}.switch-egress-queue.tr")
    plot_switch_sender_queue(out_dir, feedback_queue, no_feedback_queue)
    write_summary(out_dir, feedback, no_feedback)

    print(f"Wrote lab2 plots and summary to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
