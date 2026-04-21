import json
import os
import threading
import queue
import time
from datetime import datetime
import io

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

from flask import Flask, render_template, request, jsonify, Response

app = Flask(__name__)

# ── Paths ─────────────────────────────────────────────────────────────────────
MONITOR_PIPE = "config/monitor_pipe"
CONTROL_PIPE = "config/event_pipe"
RESULTS_DIR  = "results"

# ── Graph config: (key, title, ylabel, colour, ylim or None) ─────────────────
GRAPH_CFG = [
    ("util",   "CPU Utilization",   "Util (%)",          "#4361ee", (0, 105)),
    ("ipc",    "IPC",               "Instructions/Cycle","#2ec4b6", None),
    ("iowait", "I/O Wait",          "Wait (%)",          "#e63946", (0, 105)),
    ("ctxsw",  "Context Switches",  "Switches / s",      "#9b5de5", None),
]
SCALAR_KEYS = [g[0] for g in GRAPH_CFG]
MAX_POINTS  = 120

# ── Shared state ──────────────────────────────────────────────────────────────
timestamps   = []
data_series  = {k: [] for k in SCALAR_KEYS}
all_records  = []          # full raw dicts — used for report
session_start_wall = None  # wall-clock datetime when Start was pressed

start_time     = None
data_queue     = queue.Queue()
is_monitoring  = False
shutdown_event = threading.Event()

current_status = {
    "mode": "AUTO",
    "state": "—",
    "governor": "—",
    "power": -1.0
}

# ── Matplotlib style ──────────────────────────────────────────────────────────
plt.rcParams.update({
    "axes.facecolor":   "#f8fafc",
    "figure.facecolor": "#eceff4",
    "axes.grid":        True,
    "grid.color":       "#e2e8f0",
    "grid.linewidth":   0.8,
    "axes.spines.top":  False,
    "axes.spines.right":False,
    "axes.labelsize":   8,
    "axes.titlesize":   9,
    "axes.titleweight": "bold",
    "xtick.labelsize":  7,
    "ytick.labelsize":  7,
    "lines.linewidth":  2.2,
})

# ── Setup Graph ────────────────────────────────────────────────────────────
fig = plt.figure(figsize=(13, 6.2), facecolor="#eceff4")
gs  = gridspec.GridSpec(2, 3, figure=fig, hspace=0.32, wspace=0.28)
axes_map  = {}
lines_map = {}

for idx, (key, title, ylabel, colour, ylim) in enumerate(GRAPH_CFG):
    row, col = divmod(idx, 3)
    ax = fig.add_subplot(gs[row, col])
    ax.set_title(title,     color="#1e293b", pad=7)
    ax.set_xlabel("Time (s)", color="#64748b", labelpad=4)
    ax.set_ylabel(ylabel,     color="#64748b", labelpad=4)
    ax.tick_params(colors="#94a3b8")
    for sp in ["left", "bottom"]:
        ax.spines[sp].set_edgecolor("#cbd5e1")
    if ylim:
        ax.set_ylim(*ylim)
    (ln,) = ax.plot([], [], color=colour, linewidth=2.2, solid_capstyle="round")
    axes_map[key]  = ax
    lines_map[key] = ln

fig_lock = threading.Lock()

# ─────────────────────────────────────────────────────────────────────────────
# Report generation
# ─────────────────────────────────────────────────────────────────────────────

def _stat(values):
    """Return (min, avg, max) or (0,0,0) for empty list."""
    v = [x for x in values if x is not None and x >= 0]
    if not v:
        return 0.0, 0.0, 0.0
    return min(v), sum(v) / len(v), max(v)

def generate_report(records, session_dt, duration_s):
    if not records:
        print("[report] No data to save.")
        return

    avg_util = sum(r.get("util", 0) for r in records) / len(records)
    dt_str   = session_dt.strftime("%Y%m%d_%H%M")
    fname    = f"{dt_str}_{int(duration_s)}s_{int(avg_util)}pct.txt"
    os.makedirs(RESULTS_DIR, exist_ok=True)
    fpath    = os.path.join(RESULTS_DIR, fname)

    num_cores = len(records[0].get("core_util", []))

    with open(fpath, "w") as f:
        f.write("=" * 68 + "\n")
        f.write("  ADAPTIVE CPU PERSONA MANAGER — SESSION REPORT\n")
        f.write("=" * 68 + "\n")
        f.write(f"  Date/Time  : {session_dt.strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"  Duration   : {duration_s:.1f} s\n")
        f.write(f"  Samples    : {len(records)}\n")
        f.write(f"  CPU cores  : {num_cores}\n")
        f.write("=" * 68 + "\n\n")

        f.write("SYSTEM-WIDE SUMMARY\n")
        f.write("-" * 68 + "\n")
        f.write(f"{'Metric':<22} {'Min':>10} {'Avg':>10} {'Max':>10}\n")
        f.write("-" * 68 + "\n")

        metrics = [
            ("CPU Util (%)",      "util",    "{:.1f}"),
            ("IPC",               "ipc",     "{:.3f}"),
            ("I/O Wait (%)",      "iowait",  "{:.2f}"),
            ("Ctx Switches /s",   "ctxsw",   "{:.0f}"),
            ("Package Power (W)", "power",   "{:.2f}"),
        ]
        for label, key, fmt in metrics:
            vals = [r.get(key, -1) for r in records]
            lo, avg, hi = _stat(vals)
            if lo < 0 and avg <= 0:
                f.write(f"  {label:<20} {'N/A':>10} {'N/A':>10} {'N/A':>10}\n")
            else:
                f.write(f"  {label:<20} {fmt.format(lo):>10} "
                        f"{fmt.format(avg):>10} {fmt.format(hi):>10}\n")
        f.write("\n")

        if num_cores > 0:
            f.write("PER-CORE SUMMARY\n")
            f.write("-" * 68 + "\n")
            f.write(f"  {'Core':<8} {'Avg Util (%)':>14}\n")
            f.write("-" * 68 + "\n")

            for core in range(num_cores):
                u_vals = [r["core_util"][core] for r in records
                          if "core_util" in r and core < len(r["core_util"])]
                avg_u = sum(u_vals) / len(u_vals) if u_vals else 0.0
                f.write(f"  CPU {core:<4d} {avg_u:>14.1f}\n")
            f.write("\n")

    csv_fname = fname.replace(".txt", "_timeseries.csv")
    csv_fpath = os.path.join(RESULTS_DIR, csv_fname)

    csv_cols = ["t_s", "util", "ipc", "iowait", "ctxsw", "power"]
    for c in range(num_cores):
        csv_cols += [f"core{c}_util"]

    with open(csv_fpath, "w") as cf:
        cf.write(",".join(csv_cols) + "\n")
        for r in records:
            row = [
                f"{r.get('t', 0):.1f}",
                f"{r.get('util',   0):.2f}",
                f"{r.get('ipc',    0):.3f}",
                f"{r.get('iowait', 0):.2f}",
                f"{r.get('ctxsw',  0):.0f}",
                f"{r.get('power', -1):.2f}",
            ]
            cu = r.get("core_util", [])
            for c in range(num_cores):
                row.append(f"{cu[c]:.1f}" if c < len(cu) else "0")
            cf.write(",".join(row) + "\n")

    print(f"[report] Saved → {fpath}")
    print(f"[report] CSV   → {csv_fpath}")

# ─────────────────────────────────────────────────────────────────────────────
# IPC / commands
# ─────────────────────────────────────────────────────────────────────────────

def send_command(cmd_dict):
    try:
        with open(CONTROL_PIPE, "w") as pipe:
            pipe.write(json.dumps(cmd_dict) + "\n")
    except Exception as e:
        print("Control pipe error:", e)

def _reset_graphs():
    global start_time
    with fig_lock:
        timestamps.clear()
        for v in data_series.values():
            v.clear()
        all_records.clear()
        start_time = None
        for key, ln in lines_map.items():
            ln.set_data([], [])
            axes_map[key].relim()

def on_start():
    global is_monitoring, session_start_wall
    _reset_graphs()
    while not data_queue.empty():
        try: data_queue.get_nowait()
        except queue.Empty: break
    session_start_wall = datetime.now()
    is_monitoring = True
    send_command({"cmd": "START"})

def on_stop():
    global is_monitoring
    is_monitoring = False
    send_command({"cmd": "STOP"})
    if all_records and session_start_wall:
        duration = timestamps[-1] if timestamps else 0.0
        recs     = list(all_records)
        dt       = session_start_wall
        threading.Thread(
            target=generate_report, args=(recs, dt, duration), daemon=True
        ).start()

# ─────────────────────────────────────────────────────────────────────────────
# FIFO reader thread
# ─────────────────────────────────────────────────────────────────────────────

def read_fifo():
    print(f"[fifo] Waiting for {MONITOR_PIPE} …")
    try:
        with open(MONITOR_PIPE, "r") as pipe:
            print("[fifo] Connected to monitor FIFO")
            for raw in pipe:
                if shutdown_event.is_set():
                    break
                try:
                    data = json.loads(raw.strip())
                    data_queue.put(data)
                except json.JSONDecodeError:
                    continue
        if not shutdown_event.is_set():
            print("[fifo] C++ exited")
    except Exception as e:
        if not shutdown_event.is_set():
            print(f"[fifo] Error: {e}")

# ─────────────────────────────────────────────────────────────────────────────
# Data update loop
# ─────────────────────────────────────────────────────────────────────────────

def update_data():
    global start_time, current_status

    while not shutdown_event.is_set():
        if is_monitoring:
            updated = False
            while not data_queue.empty():
                try:
                    data = data_queue.get_nowait()
                except queue.Empty:
                    break

                if "util" not in data:
                    if "mode" in data: current_status["state"] = data["mode"]
                    if "gov" in data: current_status["governor"] = data["gov"]
                    if "auto" in data: current_status["mode"] = "AUTO" if data["auto"] else "PINNED"
                    continue

                if start_time is None:
                    start_time = time.time()

                t = time.time() - start_time
                with fig_lock:
                    timestamps.append(t)
                    data["t"] = t

                    for key in SCALAR_KEYS:
                        data_series[key].append(data.get(key, 0.0))

                    all_records.append(data)

                    if len(timestamps) > MAX_POINTS:
                        timestamps.pop(0)
                        all_records.pop(0)
                        for v in data_series.values():
                            if v: v.pop(0)

                power = data.get("power", -1.0)
                current_status["power"] = power
                if "mode" in data: current_status["state"] = data["mode"]
                if "gov" in data: current_status["governor"] = data["gov"]
                if "auto" in data: current_status["mode"] = "AUTO" if data["auto"] else "PINNED"

                updated = True

            if updated:
                with fig_lock:
                    ts = timestamps
                    for key, ln in lines_map.items():
                        vals = data_series[key]
                        n    = min(len(ts), len(vals))
                        if n > 0:
                            ln.set_data(ts[:n], vals[:n])
                            axes_map[key].relim()
                            axes_map[key].autoscale_view()
        time.sleep(0.1)

# ─────────────────────────────────────────────────────────────────────────────
# Flask endpoints
# ─────────────────────────────────────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/plot.png")
def plot_png():
    with fig_lock:
        output = io.BytesIO()
        fig.savefig(output, format='png', facecolor="#eceff4")
        output.seek(0)
        return Response(output.getvalue(), mimetype='image/png')

@app.route("/status")
def status():
    return jsonify(current_status)

@app.route("/cmd", methods=["POST"])
def endpoint_cmd():
    cmd = request.json.get("cmd")
    if cmd == "START":
        on_start()
    elif cmd == "STOP":
        on_stop()
    elif cmd == "SET_MODE":
        send_command({"cmd": "SET_MODE", "mode": request.json.get("mode")})
    elif cmd == "SET_GOV":
        send_command({"cmd": "SET_GOV", "gov": request.json.get("gov")})
    elif cmd == "QUIT":
        global is_monitoring
        is_monitoring = False
        shutdown_event.set()
        send_command({"cmd": "QUIT"})
        import sys
        
        # Flask doesn't gracefully shut down using sys.exit in a thread, but for this dev script it's ok.
        sys.exit(0)
    return jsonify({"status": "ok"})

if __name__ == "__main__":
    print("[gui] Starting Adaptive CPU Manager (Flask Web UI) …")
    threading.Thread(target=read_fifo, daemon=True).start()
    threading.Thread(target=update_data, daemon=True).start()
    app.run(host="127.0.0.1", port=5000, debug=False)
