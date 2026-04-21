#!/usr/bin/env python3
"""Plot lab1 receiver-congestion traces.

Expected input files are produced by scratch/lab1.cc:
  lab1_<tag>.msg.tr
  lab1_<tag>.switch-egress-queue.tr
  lab1_<tag>.link-throughput.tr
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import math
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

import matplotlib.pyplot as plt


DEFAULT_TRACE_DIR = Path(
    "outputs/sird-scenarios/HomaL4Protocol-lab1-receiver-congestion"
)
DEFAULT_OUT_DIR = Path("output-f/lab1")


class MsgRecord:
    def __init__(
        self,
        size: int,
        src: str,
        dst: str,
        tx_msg_id: str,
        start_ns: int,
        finish_ns: int,
    ) -> None:
        self.size = size
        self.src = src
        self.dst = dst
        self.tx_msg_id = tx_msg_id
        self.start_ns = start_ns
        self.finish_ns = finish_ns

    @property
    def fct_us(self) -> float:
        return (self.finish_ns - self.start_ns) / 1000.0


class SirdLoopSample:
    def __init__(
        self,
        time_s: float,
        receiver: str,
        sender: str,
        net_budget_pkts: float,
        host_budget_pkts: float,
        effective_budget_pkts: float,
        sender_ce: bool,
        sender_csn: bool,
        ce_ewma: float,
        sender_credits_in_use_pkts: int,
        global_credits_in_use_pkts: int,
        global_budget_pkts: int,
        ce_marks_observed: int,
        csn_marks_observed: int,
        data_pkts_observed: int,
        event_type: int,
    ) -> None:
        self.time_s = time_s
        self.receiver = receiver
        self.sender = sender
        self.net_budget_pkts = net_budget_pkts
        self.host_budget_pkts = host_budget_pkts
        self.effective_budget_pkts = effective_budget_pkts
        self.sender_ce = sender_ce
        self.sender_csn = sender_csn
        self.ce_ewma = ce_ewma
        self.sender_credits_in_use_pkts = sender_credits_in_use_pkts
        self.global_credits_in_use_pkts = global_credits_in_use_pkts
        self.global_budget_pkts = global_budget_pkts
        self.ce_marks_observed = ce_marks_observed
        self.csn_marks_observed = csn_marks_observed
        self.data_pkts_observed = data_pkts_observed
        self.event_type = event_type


def count_msg_events(path: Path, size: int) -> Tuple[int, int]:
    started = 0
    finished = 0
    if not path.exists():
        return started, finished

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 6:
                continue
            try:
                msg_size = int(parts[2])
            except ValueError:
                continue
            if msg_size != size:
                continue
            if parts[0] == "+":
                started += 1
            elif parts[0] == "-":
                finished += 1
    return started, finished


def parse_tags(raw_tags: Optional[List[str]], trace_dir: Path) -> List[str]:
    if raw_tags:
        return raw_tags

    tags = []
    for path in sorted(trace_dir.glob("lab1_*.msg.tr")):
        name = path.name
        tags.append(name[len("lab1_") : -len(".msg.tr")])
    return tags


def human_size(size: int) -> str:
    if size < 1000:
        return f"{size}B"
    if size < 1000 * 1000:
        return f"{size / 1000:.0f}KB"
    return f"{size / 1000 / 1000:.0f}MB"


def infer_label(tag: str) -> str:
    label = tag
    label = label.replace("receiver-congestion_", "")
    label = label.replace("incast_", "")
    label = label.replace("probe_", "")
    label = label.replace("_", " ")
    return label


def cdf_points(values: Iterable[float]) -> Tuple[List[float], List[float]]:
    xs = sorted(v for v in values if math.isfinite(v))
    if not xs:
        return [], []
    n = len(xs)
    ys = [(i + 1) / n for i in range(n)]
    return xs, ys


def percentile(values: Iterable[float], pct: float) -> Optional[float]:
    xs = sorted(v for v in values if math.isfinite(v))
    if not xs:
        return None
    if len(xs) == 1:
        return xs[0]
    rank = (len(xs) - 1) * pct / 100.0
    low = int(math.floor(rank))
    high = int(math.ceil(rank))
    if low == high:
        return xs[low]
    frac = rank - low
    return xs[low] * (1.0 - frac) + xs[high] * frac


def parse_msg_trace(path: Path) -> List[MsgRecord]:
    starts: Dict[Tuple[str, str, str, str, int], int] = {}
    records: List[MsgRecord] = []

    if not path.exists():
        return records

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.split()
            if len(parts) != 6:
                continue
            event, time_ns_s, size_s, src, dst, tx_msg_id = parts
            try:
                time_ns = int(time_ns_s)
                size = int(size_s)
            except ValueError:
                continue

            key = (src, dst, tx_msg_id, str(size), size)
            if event == "+":
                starts[key] = time_ns
            elif event == "-":
                start_ns = starts.pop(key, None)
                if start_ns is None:
                    continue
                records.append(
                    MsgRecord(
                        size=size,
                        src=src,
                        dst=dst,
                        tx_msg_id=tx_msg_id,
                        start_ns=start_ns,
                        finish_ns=time_ns,
                    )
                )
    return records


def parse_kv_line(line: str) -> Tuple[Optional[int], Dict[str, str]]:
    parts = line.split()
    if not parts:
        return None, {}
    try:
        time_ns = int(parts[0])
    except ValueError:
        return None, {}

    values: Dict[str, str] = {}
    for part in parts[1:]:
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        values[key] = value
    return time_ns, values


def parse_receiver_egress_queue(path: Path) -> List[Tuple[float, float]]:
    rows: List[Tuple[float, float]] = []
    if not path.exists():
        return rows

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            time_ns, values = parse_kv_line(line)
            if time_ns is None or values.get("queue") != "switch_port_to_receiver":
                continue
            try:
                rows.append((time_ns / 1e9, float(values["packets"])))
            except (KeyError, ValueError):
                continue
    return rows


def parse_receiver_throughput(path: Path) -> List[Tuple[float, float]]:
    rows: List[Tuple[float, float]] = []
    if not path.exists():
        return rows

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            time_ns, values = parse_kv_line(line)
            if time_ns is None or values.get("link") != "switch_to_receiver":
                continue
            try:
                rows.append((time_ns / 1e9, float(values["instGbps"])))
            except (KeyError, ValueError):
                continue
    return rows


def parse_all_link_throughput(path: Path) -> Dict[str, List[Tuple[float, float]]]:
    rows: Dict[str, List[Tuple[float, float]]] = defaultdict(list)
    if not path.exists():
        return rows

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            time_ns, values = parse_kv_line(line)
            if time_ns is None:
                continue
            try:
                rows[values["link"]].append((time_ns / 1e9, float(values["instGbps"])))
            except (KeyError, ValueError):
                continue
    return rows


def parse_sird_loop_trace(path: Path) -> List[SirdLoopSample]:
    rows: List[SirdLoopSample] = []
    if not path.exists():
        return rows

    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            time_ns, values = parse_kv_line(line)
            if time_ns is None:
                continue
            try:
                rows.append(
                    SirdLoopSample(
                        time_s=time_ns / 1e9,
                        receiver=values["receiver"],
                        sender=values["sender"],
                        net_budget_pkts=float(values["netBudgetPkts"]),
                        host_budget_pkts=float(values["hostBudgetPkts"]),
                        effective_budget_pkts=float(values["effectiveBudgetPkts"]),
                        sender_ce=bool(int(values["senderCe"])),
                        sender_csn=bool(int(values["senderCsn"])),
                        ce_ewma=float(values["ceEwma"]),
                        sender_credits_in_use_pkts=int(values["senderCreditsInUsePkts"]),
                        global_credits_in_use_pkts=int(values["globalCreditsInUsePkts"]),
                        global_budget_pkts=int(values["globalBudgetPkts"]),
                        ce_marks_observed=int(values["ceMarksObserved"]),
                        csn_marks_observed=int(values["csnMarksObserved"]),
                        data_pkts_observed=int(values["dataPktsObserved"]),
                        event_type=int(values["eventType"]),
                    )
                )
            except (KeyError, ValueError):
                continue
    return rows


def trim_time(rows: List[Tuple[float, float]], start_sec: Optional[float]) -> List[Tuple[float, float]]:
    if start_sec is None:
        return rows
    return [(t - start_sec, value) for t, value in rows if t >= start_sec]


def sanitize_sender(sender: str) -> str:
    return sender.replace(":", "_").replace(".", "_")


def build_windowed_count_series(
    samples: List[SirdLoopSample],
    start_sec: Optional[float],
    window_sec: float,
    counter_attr: str,
) -> List[Tuple[float, int]]:
    if not samples:
        return []

    filtered = [sample for sample in samples if start_sec is None or sample.time_s >= start_sec]
    if not filtered:
        return []

    base_time = start_sec if start_sec is not None else filtered[0].time_s
    bucket_counts: Dict[int, int] = defaultdict(int)
    prev_value = 0
    for sample in filtered:
        current_value = int(getattr(sample, counter_attr))
        delta = max(0, current_value - prev_value)
        prev_value = current_value
        bucket_idx = int(max(0.0, sample.time_s - base_time) / window_sec)
        bucket_counts[bucket_idx] += delta

    return [
        (bucket_idx * window_sec, bucket_counts[bucket_idx])
        for bucket_idx in sorted(bucket_counts)
    ]


def plot_sender_sird_loop(
    trace_dir: Path,
    out_dir: Path,
    tags: List[str],
    start_sec: Optional[float],
    window_us: float,
) -> None:
    window_sec = max(window_us, 1.0) / 1e6
    for tag in tags:
        samples = parse_sird_loop_trace(trace_dir / f"lab1_{tag}.sird-loop.tr")
        if not samples:
            continue

        by_sender: Dict[str, List[SirdLoopSample]] = defaultdict(list)
        for sample in samples:
            by_sender[sample.sender].append(sample)

        for sender, sender_samples in by_sender.items():
            sender_samples.sort(key=lambda sample: sample.time_s)
            if start_sec is not None:
                sender_samples = [sample for sample in sender_samples if sample.time_s >= start_sec]
            if not sender_samples:
                continue

            xs = [(sample.time_s - (start_sec or 0.0)) for sample in sender_samples]
            ce_counts = build_windowed_count_series(sender_samples, start_sec, window_sec, "ce_marks_observed")
            csn_counts = build_windowed_count_series(sender_samples, start_sec, window_sec, "csn_marks_observed")
            sender_suffix = sanitize_sender(sender)
            sender_title = f"{infer_label(tag)} sender {sender}"

            plt.figure(figsize=(8.0, 4.6))
            ax1 = plt.gca()
            ax1.step(xs, [sample.net_budget_pkts for sample in sender_samples], where="post", label="net budget", linewidth=1.8)
            ax1.step(xs, [sample.effective_budget_pkts for sample in sender_samples], where="post", label="effective budget", linewidth=1.5, linestyle="--")
            ax1.set_xlabel("Time since traffic start (s)" if start_sec is not None else "Time (s)")
            ax1.set_ylabel("Budget (packets)")
            ax1.set_title(f"CE Loop {sender_title}")
            ax1.grid(True, alpha=0.25)
            ax2 = ax1.twinx()
            if ce_counts:
                ce_xs, ce_ys = zip(*ce_counts)
                ax2.bar(ce_xs, ce_ys, width=window_sec * 0.85, alpha=0.22, color="tab:red", label=f"CE count / {window_us:.0f}us")
            ax2.set_ylabel("CE count")
            handles1, labels1 = ax1.get_legend_handles_labels()
            handles2, labels2 = ax2.get_legend_handles_labels()
            ax1.legend(handles1 + handles2, labels1 + labels2, fontsize=8, loc="upper right")
            plt.tight_layout()
            plt.savefig(out_dir / f"lab1_sird_ce_loop_{tag}_{sender_suffix}.png", dpi=200)
            plt.close()

            plt.figure(figsize=(8.0, 4.6))
            ax1 = plt.gca()
            ax1.step(xs, [sample.host_budget_pkts for sample in sender_samples], where="post", label="host budget", linewidth=1.8)
            ax1.step(xs, [sample.effective_budget_pkts for sample in sender_samples], where="post", label="effective budget", linewidth=1.5, linestyle="--")
            ax1.set_xlabel("Time since traffic start (s)" if start_sec is not None else "Time (s)")
            ax1.set_ylabel("Budget (packets)")
            ax1.set_title(f"CSN Loop {sender_title}")
            ax1.grid(True, alpha=0.25)
            ax2 = ax1.twinx()
            if csn_counts:
                csn_xs, csn_ys = zip(*csn_counts)
                ax2.bar(csn_xs, csn_ys, width=window_sec * 0.85, alpha=0.22, color="tab:purple", label=f"CSN count / {window_us:.0f}us")
            ax2.set_ylabel("CSN count")
            handles1, labels1 = ax1.get_legend_handles_labels()
            handles2, labels2 = ax2.get_legend_handles_labels()
            ax1.legend(handles1 + handles2, labels1 + labels2, fontsize=8, loc="upper right")
            plt.tight_layout()
            plt.savefig(out_dir / f"lab1_sird_csn_loop_{tag}_{sender_suffix}.png", dpi=200)
            plt.close()


def plot_fct_cdf(
    trace_dir: Path,
    out_dir: Path,
    tags: List[str],
    sizes: List[int],
) -> None:
    for size in sizes:
        plt.figure(figsize=(7.2, 4.2))
        plotted = False
        for tag in tags:
            records = parse_msg_trace(trace_dir / f"lab1_{tag}.msg.tr")
            fcts = [r.fct_us for r in records if r.size == size]
            xs, ys = cdf_points(fcts)
            if not xs:
                continue
            plotted = True
            started, finished = count_msg_events(trace_dir / f"lab1_{tag}.msg.tr", size)
            p50 = percentile(fcts, 50)
            p99 = percentile(fcts, 99)
            suffix = ""
            if p50 is not None and p99 is not None:
                suffix = f" p50={p50:.2f}us p99={p99:.2f}us"
            if started != finished:
                suffix += f" incomplete({finished}/{started})"
            plt.plot(xs, ys, label=f"{infer_label(tag)}{suffix}", linewidth=2)

        if not plotted:
            plt.close()
            continue
        plt.xlabel("FCT (us)")
        plt.ylabel("CDF")
        plt.title(f"{human_size(size)} message FCT")
        plt.grid(True, alpha=0.25)
        plt.legend(fontsize=8)
        plt.tight_layout()
        plt.savefig(out_dir / f"lab1_fct_cdf_{human_size(size)}.png", dpi=200)
        plt.close()


def plot_queue_timeseries(
    trace_dir: Path,
    out_dir: Path,
    tags: List[str],
    start_sec: Optional[float],
) -> None:
    plt.figure(figsize=(7.2, 4.2))
    plotted = False
    for tag in tags:
        rows = parse_receiver_egress_queue(trace_dir / f"lab1_{tag}.switch-egress-queue.tr")
        rows = trim_time(rows, start_sec)
        if not rows:
            continue
        plotted = True
        xs, ys = zip(*rows)
        plt.plot(xs, ys, label=infer_label(tag), linewidth=1.4)
    if not plotted:
        plt.close()
        return
    plt.xlabel("Time since traffic start (s)" if start_sec is not None else "Time (s)")
    plt.ylabel("Switch egress queue to receiver (packets)")
    plt.title("Receiver-facing switch egress queue")
    plt.grid(True, alpha=0.25)
    plt.legend(fontsize=8)
    plt.tight_layout()
    plt.savefig(out_dir / "lab1_receiver_queue_pkts_timeseries.png", dpi=200)
    plt.close()


def plot_queue_cdf(trace_dir: Path, out_dir: Path, tags: List[str]) -> None:
    plt.figure(figsize=(7.2, 4.2))
    plotted = False
    for tag in tags:
        rows = parse_receiver_egress_queue(trace_dir / f"lab1_{tag}.switch-egress-queue.tr")
        values = [value for _, value in rows]
        xs, ys = cdf_points(values)
        if not xs:
            continue
        plotted = True
        p99 = percentile(values, 99)
        suffix = f" p99={p99:.1f}pkts" if p99 is not None else ""
        plt.plot(xs, ys, label=f"{infer_label(tag)}{suffix}", linewidth=2)
    if not plotted:
        plt.close()
        return
    plt.xlabel("Switch egress queue to receiver (packets)")
    plt.ylabel("CDF")
    plt.title("Receiver-facing switch egress queue CDF")
    plt.grid(True, alpha=0.25)
    plt.legend(fontsize=8)
    plt.tight_layout()
    plt.savefig(out_dir / "lab1_receiver_queue_pkts_cdf.png", dpi=200)
    plt.close()


def plot_receiver_throughput(
    trace_dir: Path,
    out_dir: Path,
    tags: List[str],
    start_sec: Optional[float],
) -> None:
    plt.figure(figsize=(7.2, 4.2))
    plotted = False
    for tag in tags:
        rows = parse_receiver_throughput(trace_dir / f"lab1_{tag}.link-throughput.tr")
        rows = trim_time(rows, start_sec)
        if not rows:
            continue
        plotted = True
        xs, ys = zip(*rows)
        plt.plot(xs, ys, label=infer_label(tag), linewidth=1.6)
    if not plotted:
        plt.close()
        return
    plt.xlabel("Time since traffic start (s)" if start_sec is not None else "Time (s)")
    plt.ylabel("Throughput (Gbps)")
    plt.title("Receiver downlink throughput")
    plt.grid(True, alpha=0.25)
    plt.legend(fontsize=8)
    plt.tight_layout()
    plt.savefig(out_dir / "lab1_receiver_throughput_timeseries.png", dpi=200)
    plt.close()


def plot_all_link_throughput(
    trace_dir: Path,
    out_dir: Path,
    tags: List[str],
    start_sec: Optional[float],
) -> None:
    for tag in tags:
        by_link = parse_all_link_throughput(trace_dir / f"lab1_{tag}.link-throughput.tr")
        if not by_link:
            continue

        plt.figure(figsize=(10.5, 5.8))
        plotted = False
        for link in sorted(by_link):
            rows = trim_time(by_link[link], start_sec)
            if not rows:
                continue
            plotted = True
            xs, ys = zip(*rows)
            plt.plot(xs, ys, label=link, linewidth=1.2)

        if not plotted:
            plt.close()
            continue

        plt.xlabel("Time since traffic start (s)" if start_sec is not None else "Time (s)")
        plt.ylabel("Throughput (Gbps)")
        plt.title(f"All link throughput {infer_label(tag)}")
        plt.grid(True, alpha=0.25)
        plt.legend(fontsize=7, ncol=2)
        plt.tight_layout()
        plt.savefig(out_dir / f"lab1_all_links_throughput_{tag}.png", dpi=200)
        plt.close()


def write_summary(trace_dir: Path, out_dir: Path, tags: List[str], sizes: List[int]) -> None:
    lines: List[str] = []
    lines.append("tag,size,started,finished,incomplete,count,p50_us,p99_us,mean_us")
    for tag in tags:
        records = parse_msg_trace(trace_dir / f"lab1_{tag}.msg.tr")
        for size in sizes:
            fcts = [r.fct_us for r in records if r.size == size]
            if not fcts:
                continue
            started, finished = count_msg_events(trace_dir / f"lab1_{tag}.msg.tr", size)
            p50 = percentile(fcts, 50)
            p99 = percentile(fcts, 99)
            mean = sum(fcts) / len(fcts)
            lines.append(
                f"{tag},{size},{started},{finished},{started-finished},{len(fcts)},{p50:.6f},{p99:.6f},{mean:.6f}"
            )
    (out_dir / "lab1_summary.csv").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Plot scratch/lab1 receiver-congestion traces."
    )
    parser.add_argument("--trace-dir", type=Path, default=DEFAULT_TRACE_DIR)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)
    parser.add_argument(
        "--tag",
        action="append",
        dest="tags",
        help="Trace tag without the lab1_ prefix. Can be passed multiple times.",
    )
    parser.add_argument(
        "--size",
        action="append",
        type=int,
        dest="sizes",
        help="Message size to plot. Defaults to 8 and 500000.",
    )
    parser.add_argument(
        "--start-sec",
        type=float,
        default=0.2,
        help="Traffic start time, used only to shift time-series x axes.",
    )
    parser.add_argument(
        "--sird-loop-window-us",
        type=float,
        default=500.0,
        help="Window size in microseconds for CE/CSN count aggregation in lab1_sird_loop plots.",
    )
    args = parser.parse_args()

    trace_dir = args.trace_dir
    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    tags = parse_tags(args.tags, trace_dir)
    if not tags:
        raise SystemExit(f"No lab1_*.msg.tr traces found under {trace_dir}")

    sizes = args.sizes or [8, 500000]
    plot_fct_cdf(trace_dir, out_dir, tags, sizes)
    plot_queue_timeseries(trace_dir, out_dir, tags, args.start_sec)
    plot_queue_cdf(trace_dir, out_dir, tags)
    plot_receiver_throughput(trace_dir, out_dir, tags, args.start_sec)
    plot_all_link_throughput(trace_dir, out_dir, tags, args.start_sec)
    plot_sender_sird_loop(trace_dir, out_dir, tags, args.start_sec, args.sird_loop_window_us)
    write_summary(trace_dir, out_dir, tags, sizes)

    print(f"Wrote lab1 plots and summary to {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
