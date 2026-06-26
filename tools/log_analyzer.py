#!/usr/bin/env python3
"""CAN/ISO-TP demo log analyzer.

Features:
- Offline analysis of saved UART logs.
- Optional live capture from a serial port such as COM11.
- Counts repeated events, errors, ACK behavior, and scheduler behavior.
- If host-side timestamps exist, computes timing/jitter statistics.

Recommended live usage:
    python tools/log_analyzer.py live --port COM11 --baud 115200 --duration 20 --start-on-first-frame --save live_log.txt

Recommended offline usage:
    python tools/log_analyzer.py analyze --file C:\\Users\\24705\\Desktop\\log.txt
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import datetime as dt
import math
import pathlib
import re
import statistics
import sys
import time
from typing import Iterable, Optional


TIMESTAMP_PREFIX_RE = re.compile(
    r"^(?P<ts>"
    r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z"
    r"|"
    r"\d+(?:\.\d+)?"
    r")\s*\|\s*(?P<msg>.*)$"
)

SCHED_RE = re.compile(
    r"^(?P<can>CAN[12]) SCHED (?P<event>[A-Z_]+) SLOT=(?P<slot>\d+) FUNC=0x(?P<func>[0-9A-Fa-f]{2}) "
    r"SEQ=0x(?P<seq>[0-9A-Fa-f]{2})"
)
DISPATCH_RE = re.compile(
    r"^DISPATCH CAN(?P<can>[12]) FUNC=0x(?P<func>[0-9A-Fa-f]{2}) SEQ=0x(?P<seq>[0-9A-Fa-f]{2}) "
    r"TYPE=0x(?P<dtype>[0-9A-Fa-f]{2}) LEN=(?P<length>\d+)"
)
COUNT_RE = re.compile(
    r"^(?P<can>CAN[12]) COUNT TX_OK=(?P<tx_ok>\d+) TX_FAIL=(?P<tx_fail>\d+) "
    r"RX_IRQ=(?P<rx_irq>\d+) RX_FRAME=(?P<rx_frame>\d+)"
)
HEALTH_RE = re.compile(
    r"^(?P<can>CAN[12]) HEALTH TEC=(?P<tec>\d+) REC=(?P<rec>\d+) BOFF=(?P<boff>\d+) "
    r"EPVF=(?P<epvf>\d+) EWGF=(?P<ewgf>\d+) LEC=(?P<lec>[A-Za-z]+)"
)
ACK_TX_RE = re.compile(r"^ACK TX CAN(?P<can>[12]) SEQ=0x(?P<seq>[0-9A-Fa-f]{2}) RESULT=0x(?P<result>[0-9A-Fa-f]{2})$")


@dataclasses.dataclass
class ParsedLine:
    timestamp_s: Optional[float]
    message: str


@dataclasses.dataclass
class EventSeries:
    label: str
    timestamps: list[float]

    def stats(self) -> Optional[dict[str, float]]:
        if len(self.timestamps) < 2:
            return None
        intervals = [self.timestamps[i] - self.timestamps[i - 1] for i in range(1, len(self.timestamps))]
        return {
            "count": float(len(self.timestamps)),
            "interval_avg_ms": statistics.mean(intervals) * 1000.0,
            "interval_min_ms": min(intervals) * 1000.0,
            "interval_max_ms": max(intervals) * 1000.0,
            "interval_std_ms": statistics.pstdev(intervals) * 1000.0 if len(intervals) > 1 else 0.0,
        }


class Analyzer:
    def __init__(self, can1_period_ms: float = 1000.0, can2_period_ms: float = 200.0) -> None:
        self.total_lines = 0
        self.raw_counter: collections.Counter[str] = collections.Counter()
        self.error_counter: collections.Counter[str] = collections.Counter()
        self.sched_counter: collections.Counter[str] = collections.Counter()
        self.dispatch_counter: collections.Counter[str] = collections.Counter()
        self.health_snapshots: dict[str, dict[str, int]] = {}
        self.count_snapshots: dict[str, dict[str, int]] = {}
        self.event_series: dict[str, EventSeries] = {}
        self.seq_seen: dict[str, list[int]] = collections.defaultdict(list)
        self.ack_tx_count = 0
        self.can1_period_ms = can1_period_ms
        self.can2_period_ms = can2_period_ms

    def _record_timed_event(self, label: str, timestamp_s: Optional[float]) -> None:
        if timestamp_s is None:
            return
        if label not in self.event_series:
            self.event_series[label] = EventSeries(label=label, timestamps=[])
        self.event_series[label].timestamps.append(timestamp_s)

    def feed(self, item: ParsedLine) -> None:
        msg = item.message.strip()
        if not msg:
            return

        self.total_lines += 1
        self.raw_counter[msg] += 1

        if (
            " ERROR " in f" {msg} "
            or "TX FAIL" in msg
            or "TIMEOUT_DROPPED" in msg
            or "ACK_REJECTED" in msg
        ):
            self.error_counter[msg] += 1

        match = SCHED_RE.match(msg)
        if match:
            can_name = match.group("can")
            event = match.group("event")
            func = match.group("func").upper()
            seq = int(match.group("seq"), 16)
            key = f"{can_name}:{event}:0x{func}"
            self.sched_counter[key] += 1
            self.seq_seen[f"{can_name}:FUNC_0x{func}"].append(seq)
            if event == "ENQUEUED":
                self._record_timed_event(f"{can_name}_ENQUEUE_0x{func}", item.timestamp_s)
            if event == "ACK_OK":
                self._record_timed_event(f"{can_name}_ACK_OK_0x{func}", item.timestamp_s)
            return

        match = DISPATCH_RE.match(msg)
        if match:
            can_name = f"CAN{match.group('can')}"
            func = match.group("func").upper()
            key = f"{can_name}:DISPATCH:0x{func}"
            self.dispatch_counter[key] += 1
            self._record_timed_event(f"{can_name}_DISPATCH_0x{func}", item.timestamp_s)
            return

        match = COUNT_RE.match(msg)
        if match:
            can_name = match.group("can")
            self.count_snapshots[can_name] = {
                "tx_ok": int(match.group("tx_ok")),
                "tx_fail": int(match.group("tx_fail")),
                "rx_irq": int(match.group("rx_irq")),
                "rx_frame": int(match.group("rx_frame")),
            }
            self._record_timed_event(f"{can_name}_COUNT", item.timestamp_s)
            return

        match = HEALTH_RE.match(msg)
        if match:
            can_name = match.group("can")
            self.health_snapshots[can_name] = {
                "tec": int(match.group("tec")),
                "rec": int(match.group("rec")),
                "boff": int(match.group("boff")),
                "epvf": int(match.group("epvf")),
                "ewgf": int(match.group("ewgf")),
            }
            self._record_timed_event(f"{can_name}_HEALTH", item.timestamp_s)
            return

        match = ACK_TX_RE.match(msg)
        if match:
            self.ack_tx_count += 1
            self._record_timed_event(f"CAN{match.group('can')}_ACK_TX", item.timestamp_s)

    def _format_table(self, title: str, rows: list[tuple[str, str]]) -> str:
        if not rows:
            return f"{title}\n  (none)"
        width = max(len(k) for k, _ in rows)
        body = "\n".join(f"  {k.ljust(width)} : {v}" for k, v in rows)
        return f"{title}\n{body}"

    def _top_repeated_lines(self, limit: int = 12) -> list[tuple[str, str]]:
        rows = []
        for msg, count in self.raw_counter.most_common(limit):
            rows.append((str(count), msg))
        return rows

    def _timing_rows(self) -> list[tuple[str, str]]:
        rows: list[tuple[str, str]] = []
        for label in sorted(self.event_series):
            stats = self.event_series[label].stats()
            if not stats:
                continue
            target_ms = None
            if label == "CAN1_ENQUEUE_0x51":
                target_ms = self.can1_period_ms
            elif label == "CAN2_ENQUEUE_0x02":
                target_ms = self.can2_period_ms

            delta_text = ""
            if target_ms is not None:
                delta_ms = stats["interval_avg_ms"] - target_ms
                delta_pct = 0.0 if math.isclose(target_ms, 0.0) else (delta_ms / target_ms) * 100.0
                delta_text = f" target={target_ms:.1f}ms delta={delta_ms:+.2f}ms ({delta_pct:+.2f}%)"

            rows.append((
                label,
                "avg={avg:.2f}ms min={minv:.2f}ms max={maxv:.2f}ms std={std:.2f}ms n={n}{delta}".format(
                    avg=stats["interval_avg_ms"],
                    minv=stats["interval_min_ms"],
                    maxv=stats["interval_max_ms"],
                    std=stats["interval_std_ms"],
                    n=int(stats["count"]),
                    delta=delta_text,
                ),
            ))
        return rows

    def report(self) -> str:
        sections: list[str] = []

        sections.append(
            self._format_table(
                "Summary",
                [
                    ("total_lines", str(self.total_lines)),
                    ("error_lines", str(sum(self.error_counter.values()))),
                    ("ack_tx_count", str(self.ack_tx_count)),
                ],
            )
        )

        if self.health_snapshots:
            rows = []
            for can_name in sorted(self.health_snapshots):
                snap = self.health_snapshots[can_name]
                rows.append((
                    can_name,
                    "TEC={tec} REC={rec} BOFF={boff} EPVF={epvf} EWGF={ewgf}".format(**snap),
                ))
            sections.append(self._format_table("Latest Health", rows))

        if self.count_snapshots:
            rows = []
            for can_name in sorted(self.count_snapshots):
                snap = self.count_snapshots[can_name]
                rows.append((
                    can_name,
                    "TX_OK={tx_ok} TX_FAIL={tx_fail} RX_IRQ={rx_irq} RX_FRAME={rx_frame}".format(**snap),
                ))
            sections.append(self._format_table("Latest Counters", rows))

        if self.sched_counter:
            rows = [(k, str(v)) for k, v in sorted(self.sched_counter.items())]
            sections.append(self._format_table("Scheduler Events", rows))

        if self.dispatch_counter:
            rows = [(k, str(v)) for k, v in sorted(self.dispatch_counter.items())]
            sections.append(self._format_table("Dispatch Counts", rows))

        if self.error_counter:
            rows = [(str(v), k) for k, v in self.error_counter.most_common(20)]
            sections.append(self._format_table("Errors", rows))
        else:
            sections.append("Errors\n  (none)")

        timing_rows = self._timing_rows()
        if timing_rows:
            sections.append(self._format_table("Timing", timing_rows))
        else:
            sections.append(
                "Timing\n"
                "  No timestamped lines detected. Offline logs without host timestamps cannot produce true period/jitter stats.\n"
                "  Use live mode or save logs with the script so each line carries host receive time."
            )

        seq_rows = []
        for label, values in sorted(self.seq_seen.items()):
            unique_values = len(set(values))
            if values:
                seq_rows.append((label, f"samples={len(values)} unique={unique_values} first=0x{values[0]:02X} last=0x{values[-1]:02X}"))
        if seq_rows:
            sections.append(self._format_table("Sequence Usage", seq_rows))

        sections.append(self._format_table("Top Repeated Lines", self._top_repeated_lines()))
        return "\n\n".join(sections)


def parse_line(raw: str) -> ParsedLine:
    raw = raw.rstrip("\r\n")
    match = TIMESTAMP_PREFIX_RE.match(raw)
    if not match:
        return ParsedLine(timestamp_s=None, message=raw)

    ts_text = match.group("ts")
    msg = match.group("msg")

    if "T" in ts_text:
        dt_obj = dt.datetime.fromisoformat(ts_text.replace("Z", "+00:00"))
        return ParsedLine(timestamp_s=dt_obj.timestamp(), message=msg)

    return ParsedLine(timestamp_s=float(ts_text), message=msg)


def analyze_lines(lines: Iterable[str], can1_period_ms: float = 1000.0, can2_period_ms: float = 200.0) -> Analyzer:
    analyzer = Analyzer(can1_period_ms=can1_period_ms, can2_period_ms=can2_period_ms)
    for raw in lines:
        analyzer.feed(parse_line(raw))
    return analyzer


def cmd_analyze(args: argparse.Namespace) -> int:
    path = pathlib.Path(args.file)
    with path.open("r", encoding=args.encoding, errors="replace") as handle:
        analyzer = analyze_lines(handle, can1_period_ms=args.can1_period_ms, can2_period_ms=args.can2_period_ms)
    print(analyzer.report())
    return 0


def cmd_live(args: argparse.Namespace) -> int:
    try:
        import serial  # type: ignore
    except ImportError:
        print("pyserial is required for live mode. Install it in your Python environment first.", file=sys.stderr)
        return 2

    duration_s = args.duration
    save_handle = None
    if args.save:
        save_path = pathlib.Path(args.save)
        save_path.parent.mkdir(parents=True, exist_ok=True)
        save_handle = save_path.open("w", encoding="utf-8", newline="\n")

    analyzer = Analyzer(can1_period_ms=args.can1_period_ms, can2_period_ms=args.can2_period_ms)
    session_start = time.time()
    active_start: Optional[float] = None
    with serial.Serial(args.port, args.baud, timeout=0.2) as ser:
        if args.start_on_first_frame:
            print(
                f"Listening on {args.port} @ {args.baud} baud. "
                f"Timing window starts at first received line, then runs for {duration_s:.1f}s."
            )
        else:
            print(f"Listening on {args.port} @ {args.baud} baud for {duration_s:.1f}s")

        while True:
            if (not args.start_on_first_frame) and ((time.time() - session_start) >= duration_s):
                break
            if (args.start_on_first_frame) and (active_start is not None) and ((time.time() - active_start) >= duration_s):
                break

            raw = ser.readline()
            if not raw:
                if args.max_wait_first_frame > 0 and active_start is None:
                    if (time.time() - session_start) >= args.max_wait_first_frame:
                        print(f"No frame received within {args.max_wait_first_frame:.1f}s, stopping.")
                        break
                continue
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if args.start_on_first_frame and active_start is None:
                active_start = time.time()
                print(f"First frame received, timing window started for {duration_s:.1f}s")
            stamp = dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")
            stamped_line = f"{stamp} | {line}"
            print(stamped_line)
            if save_handle is not None:
                save_handle.write(stamped_line + "\n")
            analyzer.feed(parse_line(stamped_line))

    if save_handle is not None:
        save_handle.close()

    print()
    print(analyzer.report())
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Analyze CAN/ISO-TP UART logs.")
    subparsers = parser.add_subparsers(dest="command", required=True)

    analyze_parser = subparsers.add_parser("analyze", help="Analyze an existing log file.")
    analyze_parser.add_argument("--file", required=True, help="Path to an existing log file.")
    analyze_parser.add_argument("--encoding", default="utf-8", help="Text encoding, default utf-8.")
    analyze_parser.add_argument("--can1-period-ms", type=float, default=1000.0, help="Expected CAN1 enqueue period.")
    analyze_parser.add_argument("--can2-period-ms", type=float, default=200.0, help="Expected CAN2 enqueue period.")
    analyze_parser.set_defaults(func=cmd_analyze)

    live_parser = subparsers.add_parser("live", help="Capture and analyze a live serial log stream.")
    live_parser.add_argument("--port", default="COM11", help="Serial port, default COM11.")
    live_parser.add_argument("--baud", type=int, default=115200, help="Baud rate, default 115200.")
    live_parser.add_argument("--duration", type=float, default=15.0, help="Capture duration in seconds.")
    live_parser.add_argument("--save", help="Optional file path to save timestamped logs.")
    live_parser.add_argument(
        "--start-on-first-frame",
        action="store_true",
        help="Start the duration timer only after the first received line.",
    )
    live_parser.add_argument(
        "--max-wait-first-frame",
        type=float,
        default=0.0,
        help="Optional max wait time before first frame. 0 means wait indefinitely.",
    )
    live_parser.add_argument("--can1-period-ms", type=float, default=1000.0, help="Expected CAN1 enqueue period.")
    live_parser.add_argument("--can2-period-ms", type=float, default=200.0, help="Expected CAN2 enqueue period.")
    live_parser.set_defaults(func=cmd_live)

    return parser


def main(argv: Optional[list[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
