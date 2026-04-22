#!/usr/bin/env python3
"""Summarize and plot sim1 large leaf-spine results.

Expected traces are produced by scratch/sim1.cc:
  sim1_<tag>.goodput.tr
  sim1_<tag>.tor-egress-queue.tr
  sim1_<tag>.msg.tr
"""

from __future__ import annotations

import argparse
import csv
import math
from collections import defaultdict, deque
from pathlib import Path
from typing import DefaultDict, Deque, Dict, Iterable, List, Optional, Tuple

import matplotlib.pyplot as plt


MsgRecord = Tuple[int, int, int, str, str]


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


def mean(values: Iterable[float]) -> Optional[float]:
    xs = [v for v in values if math.isfinite(v)]
    if not xs:
        return None
    return sum(xs) / len(xs)


def parse_tags(trace_dir: Path) -> List[str]:
    tags = set()
    for suffix in ("goodput.tr", "tor-egress-queue.tr", "msg.tr"):
        for path in trace_dir.glob(f"sim1_*.{suffix}"):
            name = path.name
            tags.add(name[len("sim1_") : -len(f".{suffix}")])
    return sorted(tags)


def parse_tag(tag: str) -> Tuple[str, str, str]:
    traffic = tag.split("_", 1)[0] if "_" in tag else tag
    workload = tag[len(traffic) + 1 :] if tag.startswith(traffic + "_") else ""
    load = ""
    if "_load" in workload:
        workload, load = workload.rsplit("_load", 1)
    return traffic, workload, load


def parse_goodput(path: Path, start_sec: Optional[float], end_sec: Optional[float]) -> List[Tuple[float, float]]:
    rows: List[Tuple[float, float]] = []
    if not path.exists():
        return rows
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            time_ns, values = parse_kv_line(line)
            if time_ns is None or "goodputGbps" not in values:
                continue
            time_s = time_ns / 1e9
            if start_sec is not None and time_s < start_sec:
                continue
            if end_sec is not None and time_s > end_sec:
                continue
            try:
                rows.append((time_s, float(values["goodputGbps"])))
            except ValueError:
                continue
    return rows


def parse_queue(path: Path, start_sec: Optional[float], end_sec: Optional[float]) -> List[Tuple[float, float, float]]:
    rows: List[Tuple[float, float, float]] = []
    if not path.exists():
        return rows
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            time_ns, values = parse_kv_line(line)
            if time_ns is None or values.get("queue") != "aggregate":
                continue
            time_s = time_ns / 1e9
            if start_sec is not None and time_s < start_sec:
                continue
            if end_sec is not None and time_s > end_sec:
                continue
            try:
                rows.append((time_s, float(values["maxPackets"]), float(values["meanPackets"])))
            except (KeyError, ValueError):
                continue
    return rows


def parse_msg_trace(path: Path) -> List[MsgRecord]:
    starts: DefaultDict[Tuple[str, str, str, int], Deque[int]] = defaultdict(deque)
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
            key = (src, dst, tx_msg_id, size)
            if event == "+":
                starts[key].append(time_ns)
            elif event == "-" and starts[key]:
                start_ns = starts[key].popleft()
                records.append((start_ns, time_ns, size, src, dst))
    return records


def host_index(endpoint: str) -> Optional[int]:
    ip = endpoint.split(":", 1)[0]
    parts = ip.split(".")
    if len(parts) != 4:
        return None
    try:
        return int(parts[2])
    except ValueError:
        return None


def one_way_delay_us(src: str, dst: str, hosts_per_tor: int, link_delay_us: float) -> float:
    src_idx = host_index(src)
    dst_idx = host_index(dst)
    if src_idx is None or dst_idx is None:
        return 4.0 * link_delay_us
    if src_idx // hosts_per_tor == dst_idx // hosts_per_tor:
        return 2.0 * link_delay_us
    return 4.0 * link_delay_us


def min_fct_us(
    size_bytes: int,
    src: str,
    dst: str,
    mss_bytes: int,
    wire_bytes: int,
    host_rate_gbps: float,
    hosts_per_tor: int,
    link_delay_us: float,
) -> float:
    pkts = max(1, math.ceil(size_bytes / mss_bytes))
    tx_us = (pkts * wire_bytes * 8.0) / (host_rate_gbps * 1e9) * 1e6
    return one_way_delay_us(src, dst, hosts_per_tor, link_delay_us) + tx_us


def size_group(size_bytes: int, mss_bytes: int, bdp_pkts: float) -> str:
    bdp_bytes = bdp_pkts * mss_bytes
    if size_bytes < mss_bytes:
        return "A:<MSS"
    if size_bytes < bdp_bytes:
        return "B:MSS-BDP"
    if size_bytes < 8.0 * bdp_bytes:
        return "C:BDP-8BDP"
    return "D:>8BDP"


def summarize(trace_dir: Path, args: argparse.Namespace) -> Tuple[List[Dict[str, object]], List[Dict[str, object]]]:
    perf_rows: List[Dict[str, object]] = []
    slowdown_rows: List[Dict[str, object]] = []
    for tag in parse_tags(trace_dir):
        traffic, workload, load = parse_tag(tag)
        goodput = parse_goodput(trace_dir / f"sim1_{tag}.goodput.tr", args.start_sec, args.end_sec)
        queue = parse_queue(trace_dir / f"sim1_{tag}.tor-egress-queue.tr", args.start_sec, args.end_sec)
        goodput_values = [row[1] for row in goodput]
        queue_max_values = [row[1] for row in queue]
        queue_mean_values = [row[2] for row in queue]
        perf_rows.append(
            {
                "tag": tag,
                "traffic_config": traffic,
                "workload": workload,
                "load": load,
                "goodput_mean_gbps": mean(goodput_values),
                "goodput_peak_gbps": max(goodput_values) if goodput_values else None,
                "tor_queue_max_pkts": max(queue_max_values) if queue_max_values else None,
                "tor_queue_mean_pkts": mean(queue_mean_values),
                "tor_queue_mean_peak_pkts": max(queue_mean_values) if queue_mean_values else None,
                "goodput_samples": len(goodput_values),
                "queue_samples": len(queue_mean_values),
            }
        )

        by_group: DefaultDict[str, List[float]] = defaultdict(list)
        for start_ns, finish_ns, size, src, dst in parse_msg_trace(trace_dir / f"sim1_{tag}.msg.tr"):
            if args.start_sec is not None and start_ns / 1e9 < args.start_sec:
                continue
            if args.end_sec is not None and start_ns / 1e9 > args.end_sec:
                continue
            fct_us = (finish_ns - start_ns) / 1000.0
            ideal_us = min_fct_us(
                size,
                src,
                dst,
                args.mss_bytes,
                args.wire_bytes,
                args.host_rate_gbps,
                args.hosts_per_tor,
                args.link_delay_us,
            )
            if ideal_us > 0:
                by_group[size_group(size, args.mss_bytes, args.bdp_pkts)].append(fct_us / ideal_us)

        for group in ("A:<MSS", "B:MSS-BDP", "C:BDP-8BDP", "D:>8BDP"):
            values = by_group.get(group, [])
            slowdown_rows.append(
                {
                    "tag": tag,
                    "traffic_config": traffic,
                    "workload": workload,
                    "load": load,
                    "size_group": group,
                    "count": len(values),
                    "slowdown_median": percentile(values, 50),
                    "slowdown_p99": percentile(values, 99),
                }
            )
    return perf_rows, slowdown_rows


def write_csv(path: Path, rows: List[Dict[str, object]]) -> None:
    if not rows:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def finite_or_zero(value: object) -> float:
    if value is None:
        return 0.0
    try:
        numeric = float(value)
    except (TypeError, ValueError):
        return 0.0
    return numeric if math.isfinite(numeric) else 0.0


def plot_perf(perf_rows: List[Dict[str, object]], out_dir: Path) -> None:
    if not perf_rows:
        return
    labels = [str(row["tag"]) for row in perf_rows]
    x = list(range(len(labels)))

    fig, ax = plt.subplots(figsize=(max(10, len(labels) * 0.65), 5))
    ax.bar(x, [finite_or_zero(row["goodput_peak_gbps"]) for row in perf_rows], label="Peak")
    ax.bar(x, [finite_or_zero(row["goodput_mean_gbps"]) for row in perf_rows], alpha=0.65, label="Mean")
    ax.set_ylabel("Goodput (Gbps)")
    ax.set_title("sim1 Goodput")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=35, ha="right")
    ax.grid(axis="y", alpha=0.25)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_dir / "sim1_goodput.png", dpi=180)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(max(10, len(labels) * 0.65), 5))
    ax.bar(x, [finite_or_zero(row["tor_queue_max_pkts"]) for row in perf_rows], label="Maximum")
    ax.bar(x, [finite_or_zero(row["tor_queue_mean_pkts"]) for row in perf_rows], alpha=0.65, label="Mean")
    ax.set_ylabel("ToR queue (packets)")
    ax.set_title("sim1 ToR Queuing")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=35, ha="right")
    ax.grid(axis="y", alpha=0.25)
    ax.legend()
    fig.tight_layout()
    fig.savefig(out_dir / "sim1_tor_queue.png", dpi=180)
    plt.close(fig)


def plot_slowdown(slowdown_rows: List[Dict[str, object]], out_dir: Path) -> None:
    by_tag: DefaultDict[str, List[Dict[str, object]]] = defaultdict(list)
    for row in slowdown_rows:
        by_tag[str(row["tag"])].append(row)
    for tag, rows in by_tag.items():
        groups = ["A:<MSS", "B:MSS-BDP", "C:BDP-8BDP", "D:>8BDP"]
        row_by_group = {str(row["size_group"]): row for row in rows}
        x = list(range(len(groups)))
        width = 0.38
        fig, ax = plt.subplots(figsize=(8, 5))
        ax.bar(
            [i - width / 2 for i in x],
            [finite_or_zero(row_by_group[g].get("slowdown_median")) for g in groups],
            width=width,
            label="Median",
        )
        ax.bar(
            [i + width / 2 for i in x],
            [finite_or_zero(row_by_group[g].get("slowdown_p99")) for g in groups],
            width=width,
            label="p99",
        )
        ax.set_ylabel("Message slowdown")
        ax.set_title(f"sim1 Slowdown: {tag}")
        ax.set_xticks(x)
        ax.set_xticklabels(groups)
        ax.grid(axis="y", alpha=0.25)
        ax.legend()
        fig.tight_layout()
        fig.savefig(out_dir / f"sim1_slowdown_{tag}.png", dpi=180)
        plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace-dir", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--start-sec", type=float, default=None)
    parser.add_argument("--end-sec", type=float, default=None)
    parser.add_argument("--bdp-pkts", type=float, default=66.67)
    parser.add_argument("--mss-bytes", type=int, default=1442)
    parser.add_argument("--wire-bytes", type=int, default=1504)
    parser.add_argument("--host-rate-gbps", type=float, default=100.0)
    parser.add_argument("--hosts-per-tor", type=int, default=16)
    parser.add_argument("--link-delay-us", type=float, default=1.0)
    args = parser.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    perf_rows, slowdown_rows = summarize(args.trace_dir, args)
    write_csv(args.out_dir / "sim1_goodput_queue_summary.csv", perf_rows)
    write_csv(args.out_dir / "sim1_slowdown_summary.csv", slowdown_rows)
    plot_perf(perf_rows, args.out_dir)
    plot_slowdown(slowdown_rows, args.out_dir)


if __name__ == "__main__":
    main()
