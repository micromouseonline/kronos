"""Extracts /api/event receipts from a cerberus debug log and reports
one-way trigger-to-receipt latency (see NETWORK-TIMING-LOG.md).

Each cerberus log line looks like:
  #[224255534] [HTTP] POST /api/event from 192.168.0.189 body={"gate_id":"UNKNOWN","event":"GOAL","tsf_us":224254712237,"gate_us":584216857}

`#[recv_ms]` and the JSON body's `tsf_us` are both on the shared Wi-Fi TSF
timeline (cerberus and hesperus are stations on the same AP), so
`recv_ms*1000 - tsf_us` is directly comparable across boards without NTP or
any other clock sync -- this is the same method used throughout
NETWORK-TIMING-LOG.md.

Usage:
  python3 tools/cerberus_log_stats.py LOGFILE [--event GOAL] [--csv out.csv] [--gaps]
  python3 tools/cerberus_log_stats.py -              # read from stdin
"""
import argparse
import csv
import json
import re
import statistics
import sys

LINE_RE = re.compile(r"#\[(?P<recv_ms>\d+)\].*?body=(?P<body>\{.*\})\s*$")


def parse_lines(lines):
    events = []
    for lineno, line in enumerate(lines, 1):
        m = LINE_RE.search(line)
        if not m:
            continue
        try:
            body = json.loads(m.group("body"))
        except json.JSONDecodeError:
            continue
        tsf_us = body.get("tsf_us")
        if tsf_us is None:
            continue
        recv_ms = int(m.group("recv_ms"))
        events.append({
            "line": lineno,
            "recv_ms": recv_ms,
            "gate_id": body.get("gate_id"),
            "event": body.get("event"),
            "tsf_us": tsf_us,
            "gate_us": body.get("gate_us"),
            "latency_us": recv_ms * 1000 - tsf_us,
        })
    return events


def percentile(sorted_vals, p):
    idx = min(int(len(sorted_vals) * p), len(sorted_vals) - 1)
    return sorted_vals[idx]


def print_stats(label, events):
    lat = sorted(e["latency_us"] for e in events)
    if not lat:
        return
    print(f"\n-- {label} (n={len(lat)}) --")
    print(f"  mean   {statistics.mean(lat)/1000:8.1f} ms")
    print(f"  median {statistics.median(lat)/1000:8.1f} ms")
    if len(lat) > 1:
        print(f"  stdev  {statistics.stdev(lat)/1000:8.1f} ms")
    print(f"  min    {lat[0]/1000:8.1f} ms")
    print(f"  max    {lat[-1]/1000:8.1f} ms")
    print(f"  p90    {percentile(lat, 0.90)/1000:8.1f} ms")
    print(f"  p95    {percentile(lat, 0.95)/1000:8.1f} ms")
    print(f"  p99    {percentile(lat, 0.99)/1000:8.1f} ms")


def print_table(events):
    print(f"{'#':>4}  {'recv_ms':>12}  {'event':<8} {'tsf_us':>15}  {'gate_us':>12}  {'latency_ms':>10}")
    for i, e in enumerate(events):
        gate_us = e["gate_us"] if e["gate_us"] is not None else ""
        print(f"{i:>4}  {e['recv_ms']:>12}  {e['event'] or '':<8} {e['tsf_us']:>15}  "
              f"{gate_us:>12}  {e['latency_us']/1000:>10.1f}")


def print_gaps(events):
    print(f"\n-- consecutive gaps (n={len(events)}) --")
    print(f"{'#':>4}  {'d_gate_ms':>10}  {'d_latency_ms':>13}  {'latency_ms':>10}")
    for i in range(1, len(events)):
        prev, cur = events[i - 1], events[i]
        if prev["gate_us"] is None or cur["gate_us"] is None:
            continue
        d_gate = (cur["gate_us"] - prev["gate_us"]) / 1000
        d_lat = (cur["latency_us"] - prev["latency_us"]) / 1000
        print(f"{i:>4}  {d_gate:>10.1f}  {d_lat:>13.1f}  {cur['latency_us']/1000:>10.1f}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logfile", help="cerberus debug log, or '-' for stdin")
    parser.add_argument("--event", help="only include this event type (e.g. GOAL, START, ARM)")
    parser.add_argument("--csv", help="write extracted rows to this CSV path")
    parser.add_argument("--gaps", action="store_true", help="show consecutive gate_us/latency deltas (burst/queueing diagnosis)")
    parser.add_argument("--no-table", action="store_true", help="skip the per-row table, print stats only")
    args = parser.parse_args()

    if args.logfile == "-":
        lines = sys.stdin.readlines()
    else:
        with open(args.logfile) as f:
            lines = f.readlines()

    events = parse_lines(lines)
    if args.event:
        events = [e for e in events if e["event"] == args.event]

    if not events:
        print("No /api/event receipts found.", file=sys.stderr)
        sys.exit(1)

    if not args.no_table:
        print_table(events)

    if args.gaps:
        print_gaps(events)

    event_types = sorted(set(e["event"] for e in events))
    if len(event_types) > 1:
        for t in event_types:
            print_stats(t, [e for e in events if e["event"] == t])
    print_stats("all", events)

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["line", "recv_ms", "gate_id", "event", "tsf_us", "gate_us", "latency_us"])
            writer.writeheader()
            writer.writerows(events)
        print(f"\nWrote {len(events)} rows to {args.csv}")


if __name__ == "__main__":
    main()
