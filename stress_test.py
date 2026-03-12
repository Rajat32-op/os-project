#!/usr/bin/env python3
"""
stress_test.py — Stress-test runner for the OS monitor project.

Architecture
------------
                 ┌─────────────────────┐
  event_pipe ──► │  C++ EventController│
                 │  → MonitorManager   │
                 │    → turbostat      │
                 │    → monitor_pipe   │
                 └──────────┬──────────┘
                            │ JSON lines
                 ┌──────────▼──────────┐
                 │  This script reads  │
                 │  metrics in real    │
                 │  time while running │
                 │  stress-ng / sysbench│
                 └─────────────────────┘

Prerequisites
-------------
    sudo apt-get install stress-ng sysbench   # already done
    make run                                  # in another terminal (keeps running)

Usage
-----
    python3 stress_test.py [--scenarios idle light medium heavy burst]
                           [--duration 10]
                           [--output results/stress_report.png]
"""

import argparse
import json
import os
import subprocess
import sys
import threading
import time
from collections import defaultdict
from datetime import datetime

import matplotlib
matplotlib.use("Agg")          # headless – no display needed
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import pandas as pd

# ──────────────────────────────────────────────────────────────────────────────
# Paths (relative to project root)
# ──────────────────────────────────────────────────────────────────────────────
MONITOR_PIPE = "config/monitor_pipe"
EVENT_PIPE   = "config/event_pipe"
RESULTS_DIR  = "results"

# ──────────────────────────────────────────────────────────────────────────────
# Scenario definitions
# ──────────────────────────────────────────────────────────────────────────────
NCPUS = os.cpu_count() or 4

SCENARIOS = {
    "idle": {
        "description": "No load – baseline",
        "cmd": None,                       # just sleep
    },
    "light": {
        "description": "25 % CPU load (matrix ops)",
        "cmd": ["stress-ng", "--cpu", str(max(1, NCPUS // 4)),
                "--cpu-method", "matrixprod", "--timeout", "0"],
    },
    "medium": {
        "description": "50 % CPU load (all methods mix)",
        "cmd": ["stress-ng", "--cpu", str(max(1, NCPUS // 2)),
                "--cpu-method", "all", "--timeout", "0"],
    },
    "heavy": {
        "description": "100 % CPU load (prime numbers)",
        "cmd": ["stress-ng", "--cpu", str(NCPUS),
                "--cpu-method", "prime", "--timeout", "0"],
    },
    "burst": {
        "description": "CPU + memory + I/O simultaneous pressure",
        "cmd": ["stress-ng", "--cpu", str(NCPUS),
                "--vm", "2", "--vm-bytes", "256M",
                "--io", "2", "--timeout", "0"],
    },
}

# ──────────────────────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────────────────────

def send_command(cmd: str):
    """Write a control command to event_pipe (START / STOP)."""
    try:
        with open(EVENT_PIPE, "w") as p:
            p.write(json.dumps({"cmd": cmd}) + "\n")
    except Exception as e:
        print(f"[warn] event_pipe write failed: {e}", file=sys.stderr)


def collect_metrics(duration: float, stop_event: threading.Event) -> list[dict]:
    """
    Open monitor_pipe in non-blocking mode and collect JSON lines for
    'duration' seconds.  Returns a list of dicts with keys: util, mhz, ipc, t.
    """
    records = []
    t0 = time.monotonic()

    try:
        fd = os.open(MONITOR_PIPE, os.O_RDONLY | os.O_NONBLOCK)
    except OSError as e:
        print(f"[error] Cannot open monitor_pipe: {e}\n"
              "        Is the C++ backend running?  (make run)", file=sys.stderr)
        stop_event.set()
        return records

    buf = b""
    with os.fdopen(fd, "rb", buffering=0) as pipe:
        while not stop_event.is_set():
            elapsed = time.monotonic() - t0
            if elapsed >= duration:
                break
            try:
                chunk = pipe.read(4096)
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            data = json.loads(line)
                            data["t"] = elapsed
                            records.append(data)
                        except json.JSONDecodeError:
                            pass
            except BlockingIOError:
                time.sleep(0.1)

    return records


# ──────────────────────────────────────────────────────────────────────────────
# Sysbench baseline
# ──────────────────────────────────────────────────────────────────────────────

def run_sysbench_benchmark() -> dict:
    """
    Run a quick sysbench CPU benchmark and return parsed results.
    """
    print("  [sysbench] running prime-number benchmark …", flush=True)
    try:
        result = subprocess.run(
            ["sysbench", "cpu", "--cpu-max-prime=10000",
             f"--threads={NCPUS}", "--time=10", "run"],
            capture_output=True, text=True, timeout=30
        )
        output = result.stdout
        parsed = {}
        for line in output.splitlines():
            if "events per second" in line:
                parsed["events_per_second"] = float(line.split(":")[1].strip())
            if "total time" in line and "s" in line:
                parsed["total_time_s"] = float(line.split(":")[1].strip().rstrip("s"))
            if "min:" in line:
                parsed["latency_min_ms"] = float(line.split(":")[1].strip())
            if "avg:" in line:
                parsed["latency_avg_ms"] = float(line.split(":")[1].strip())
            if "max:" in line:
                parsed["latency_max_ms"] = float(line.split(":")[1].strip())
        return parsed
    except Exception as e:
        print(f"  [sysbench] failed: {e}", file=sys.stderr)
        return {}


# ──────────────────────────────────────────────────────────────────────────────
# Per-scenario runner
# ──────────────────────────────────────────────────────────────────────────────

def run_scenario(name: str, duration: float) -> list[dict]:
    scenario = SCENARIOS[name]
    print(f"\n{'─'*60}")
    print(f"  Scenario : {name.upper()}")
    print(f"  Load     : {scenario['description']}")
    print(f"  Duration : {duration}s")
    print(f"{'─'*60}", flush=True)

    stop_event = threading.Event()

    # Start stress process (if any)
    stress_proc = None
    if scenario["cmd"]:
        stress_proc = subprocess.Popen(
            scenario["cmd"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )

    # Give 1 s for load to ramp up, then collect
    time.sleep(1)
    records = collect_metrics(duration - 1, stop_event)

    # Tear down stress process
    if stress_proc:
        stress_proc.terminate()
        try:
            stress_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            stress_proc.kill()

    if records:
        utils  = [r["util"] for r in records]
        mhzs   = [r["mhz"]  for r in records]
        ipcs   = [r["ipc"]  for r in records]
        print(f"  Samples  : {len(records)}")
        print(f"  Util     : avg={sum(utils)/len(utils):.1f}%  "
              f"max={max(utils):.1f}%")
        print(f"  MHz      : avg={sum(mhzs)/len(mhzs):.0f}  "
              f"max={max(mhzs):.0f}")
        print(f"  IPC      : avg={sum(ipcs)/len(ipcs):.3f}  "
              f"max={max(ipcs):.3f}")
    else:
        print("  [warn] No samples received – is the backend running?")

    # Tag records with scenario name
    for r in records:
        r["scenario"] = name

    return records


# ──────────────────────────────────────────────────────────────────────────────
# Plotting
# ──────────────────────────────────────────────────────────────────────────────

COLORS = {
    "idle":   "#2196F3",
    "light":  "#4CAF50",
    "medium": "#FF9800",
    "heavy":  "#F44336",
    "burst":  "#9C27B0",
}


def plot_results(all_records: list[dict], sysbench: dict, output_path: str):
    if not all_records:
        print("[warn] No data to plot.")
        return

    df = pd.DataFrame(all_records)
    scenarios_present = df["scenario"].unique().tolist()

    fig = plt.figure(figsize=(18, 12))
    fig.suptitle(
        f"CPU Stress-Test Report  —  {datetime.now().strftime('%Y-%m-%d %H:%M')}",
        fontsize=14, fontweight="bold"
    )

    gs = fig.add_gridspec(3, 3, hspace=0.45, wspace=0.35)

    # ── 1. Time-series: CPU Utilisation ──────────────────────────────────────
    ax_util = fig.add_subplot(gs[0, :2])
    for sname in scenarios_present:
        sub = df[df["scenario"] == sname].sort_values("t")
        ax_util.plot(sub["t"], sub["util"],
                     label=sname, color=COLORS.get(sname, "grey"), linewidth=1.5)
    ax_util.set_title("CPU Utilisation over Time")
    ax_util.set_xlabel("Time (s)")
    ax_util.set_ylabel("Utilisation (%)")
    ax_util.set_ylim(0, 105)
    ax_util.legend(fontsize=8)
    ax_util.grid(True, alpha=0.3)

    # ── 2. Time-series: MHz ───────────────────────────────────────────────────
    ax_mhz = fig.add_subplot(gs[1, :2])
    for sname in scenarios_present:
        sub = df[df["scenario"] == sname].sort_values("t")
        ax_mhz.plot(sub["t"], sub["mhz"],
                    label=sname, color=COLORS.get(sname, "grey"), linewidth=1.5)
    ax_mhz.set_title("Average CPU Frequency (MHz)")
    ax_mhz.set_xlabel("Time (s)")
    ax_mhz.set_ylabel("MHz")
    ax_mhz.legend(fontsize=8)
    ax_mhz.grid(True, alpha=0.3)

    # ── 3. Time-series: IPC ───────────────────────────────────────────────────
    ax_ipc = fig.add_subplot(gs[2, :2])
    for sname in scenarios_present:
        sub = df[df["scenario"] == sname].sort_values("t")
        ax_ipc.plot(sub["t"], sub["ipc"],
                    label=sname, color=COLORS.get(sname, "grey"), linewidth=1.5)
    ax_ipc.set_title("Instructions Per Cycle (IPC)")
    ax_ipc.set_xlabel("Time (s)")
    ax_ipc.set_ylabel("IPC")
    ax_ipc.legend(fontsize=8)
    ax_ipc.grid(True, alpha=0.3)

    # ── 4. Box-plot summary: Utilisation ─────────────────────────────────────
    ax_box = fig.add_subplot(gs[0, 2])
    data_by_scenario = [
        df[df["scenario"] == s]["util"].values
        for s in scenarios_present
        if len(df[df["scenario"] == s]) > 0
    ]
    bp = ax_box.boxplot(data_by_scenario, patch_artist=True,
                        labels=scenarios_present)
    for patch, sname in zip(bp["boxes"], scenarios_present):
        patch.set_facecolor(COLORS.get(sname, "grey"))
        patch.set_alpha(0.7)
    ax_box.set_title("Utilisation Distribution")
    ax_box.set_ylabel("Utilisation (%)")
    ax_box.set_ylim(0, 105)
    ax_box.tick_params(axis="x", rotation=30)
    ax_box.grid(True, alpha=0.3)

    # ── 5. Bar: mean IPC per scenario ────────────────────────────────────────
    ax_ipc_bar = fig.add_subplot(gs[1, 2])
    mean_ipc = [df[df["scenario"] == s]["ipc"].mean() for s in scenarios_present]
    bars = ax_ipc_bar.bar(scenarios_present, mean_ipc,
                          color=[COLORS.get(s, "grey") for s in scenarios_present],
                          alpha=0.8, edgecolor="black", linewidth=0.5)
    ax_ipc_bar.set_title("Mean IPC per Scenario")
    ax_ipc_bar.set_ylabel("IPC")
    ax_ipc_bar.tick_params(axis="x", rotation=30)
    ax_ipc_bar.grid(True, alpha=0.3, axis="y")
    for bar, val in zip(bars, mean_ipc):
        ax_ipc_bar.text(bar.get_x() + bar.get_width() / 2,
                        bar.get_height() + 0.005,
                        f"{val:.2f}", ha="center", va="bottom", fontsize=8)

    # ── 6. Sysbench summary text ──────────────────────────────────────────────
    ax_text = fig.add_subplot(gs[2, 2])
    ax_text.axis("off")
    if sysbench:
        lines = [
            "sysbench  (prime ≤ 10 000)",
            f"Threads        : {NCPUS}",
            f"Events/s       : {sysbench.get('events_per_second', 'n/a')}",
            f"Total time     : {sysbench.get('total_time_s', 'n/a')} s",
            f"Latency min    : {sysbench.get('latency_min_ms', 'n/a')} ms",
            f"Latency avg    : {sysbench.get('latency_avg_ms', 'n/a')} ms",
            f"Latency max    : {sysbench.get('latency_max_ms', 'n/a')} ms",
        ]
    else:
        lines = ["sysbench results unavailable"]
    ax_text.text(0.05, 0.95, "\n".join(lines),
                 transform=ax_text.transAxes,
                 fontsize=9, verticalalignment="top",
                 fontfamily="monospace",
                 bbox=dict(boxstyle="round", facecolor="lightyellow", alpha=0.8))
    ax_text.set_title("Benchmark Summary")

    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    fig.savefig(output_path, dpi=150, bbox_inches="tight")
    print(f"\n[✓] Report saved → {output_path}")


# ──────────────────────────────────────────────────────────────────────────────
# CSV dump
# ──────────────────────────────────────────────────────────────────────────────

def dump_csv(all_records: list[dict], csv_path: str):
    if not all_records:
        return
    df = pd.DataFrame(all_records)
    df.to_csv(csv_path, index=False)
    print(f"[✓] Raw data  saved → {csv_path}")


# ──────────────────────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Stress-test runner for the OS monitor project"
    )
    parser.add_argument(
        "--scenarios", nargs="+",
        choices=list(SCENARIOS.keys()),
        default=["idle", "light", "medium", "heavy", "burst"],
        help="Which scenarios to run (default: all)"
    )
    parser.add_argument(
        "--duration", type=float, default=15,
        help="Seconds to run each scenario (default: 15)"
    )
    parser.add_argument(
        "--output", default=f"{RESULTS_DIR}/stress_report.png",
        help="Output PNG path (default: results/stress_report.png)"
    )
    parser.add_argument(
        "--no-sysbench", action="store_true",
        help="Skip the sysbench benchmark"
    )
    args = parser.parse_args()

    print("=" * 60)
    print("  OS Monitor  –  Stress Test Runner")
    print(f"  Scenarios : {', '.join(args.scenarios)}")
    print(f"  Duration  : {args.duration}s per scenario")
    print(f"  CPUs      : {NCPUS}")
    print("=" * 60)
    print("\n[!] Make sure the C++ backend is running: make run\n")

    # Tell the backend to start collecting
    send_command("START")
    time.sleep(1)   # let turbostat warm up

    all_records: list[dict] = []

    for scenario_name in args.scenarios:
        records = run_scenario(scenario_name, args.duration)
        all_records.extend(records)
        time.sleep(2)  # brief cooldown between scenarios

    # Stop the backend collection
    send_command("STOP")

    # Sysbench baseline (independent of backend)
    sysbench_results = {}
    if not args.no_sysbench:
        print(f"\n{'─'*60}")
        print("  Sysbench baseline benchmark")
        print(f"{'─'*60}")
        sysbench_results = run_sysbench_benchmark()
        if sysbench_results:
            for k, v in sysbench_results.items():
                print(f"  {k:<25}: {v}")

    # Save outputs
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_path = f"{RESULTS_DIR}/stress_data_{ts}.csv"
    png_path = args.output

    dump_csv(all_records, csv_path)
    plot_results(all_records, sysbench_results, png_path)

    print("\nDone.")


if __name__ == "__main__":
    main()
