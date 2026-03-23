import json
import os
import tkinter as tk
import threading
import queue
import time
from datetime import datetime

import matplotlib
matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
import matplotlib.pyplot as plt
import matplotlib.gridspec as gridspec

# ── Paths ─────────────────────────────────────────────────────────────────────
MONITOR_PIPE = "config/monitor_pipe"
CONTROL_PIPE = "config/event_pipe"
RESULTS_DIR  = "results"

# ── Graph config: (key, title, ylabel, colour, ylim or None) ─────────────────
GRAPH_CFG = [
    ("util",   "CPU Utilization",   "Util (%)",          "#4361ee", (0, 105)),
    ("ipc",    "IPC",               "Instructions/Cycle","#2ec4b6", None),
    ("mhz",    "Avg Frequency",     "MHz",               "#ff9f1c", None),
    ("iowait", "I/O Wait",          "Wait (%)",          "#e63946", (0, 105)),
    ("ctxsw",  "Context Switches",  "Switches / s",      "#9b5de5", None),
    ("temp",   "Temperature",       "°C",                "#f4a261", None),
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

# ─────────────────────────────────────────────────────────────────────────────
# Window
# ─────────────────────────────────────────────────────────────────────────────
root = tk.Tk()
root.title("Adaptive CPU Persona Manager — Monitor")
root.configure(bg="#eceff4")
root.geometry("1300x800")
root.minsize(900, 620)

# ── Status bar ────────────────────────────────────────────────────────────────
status_frame = tk.Frame(root, bg="#1e293b", pady=10)
status_frame.pack(fill=tk.X, side=tk.TOP)

tk.Label(status_frame, text="  🖥  Adaptive CPU Manager",
         bg="#1e293b", fg="#e2e8f0",
         font=("Helvetica", 13, "bold")).pack(side=tk.LEFT, padx=(14, 36))

def _badge(parent, label, init, bg):
    wrap = tk.Frame(parent, bg="#1e293b"); wrap.pack(side=tk.LEFT, padx=10)
    tk.Label(wrap, text=label, bg="#1e293b", fg="#94a3b8",
             font=("Helvetica", 8)).pack(side=tk.LEFT)
    val = tk.Label(wrap, text=init, bg=bg, fg="white",
                   font=("Helvetica", 9, "bold"),
                   padx=10, pady=3, relief="flat")
    val.pack(side=tk.LEFT, padx=(5, 0))
    return val

mode_badge  = _badge(status_frame, "Mode",     "AUTO", "#3b82f6")
state_badge = _badge(status_frame, "State",    "—",    "#475569")
gov_badge   = _badge(status_frame, "Governor", "—",    "#10b981")
temp_badge  = _badge(status_frame, "Temp",     "—",    "#f59e0b")
power_badge = _badge(status_frame, "Power",    "—",    "#6366f1")

# ── 2×3 Graph grid ────────────────────────────────────────────────────────────
graph_frame = tk.Frame(root, bg="#eceff4")
graph_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=(8, 0))

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

canvas = FigureCanvasTkAgg(fig, master=graph_frame)
canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

# ── Button bar ────────────────────────────────────────────────────────────────
btn_bar = tk.Frame(root, bg="#dde3ea", pady=10)
btn_bar.pack(fill=tk.X, side=tk.BOTTOM)

def _btn(parent, text, cmd, bg, fg="white", width=14):
    b = tk.Button(parent, text=text, command=cmd, bg=bg, fg=fg,
                  width=width, font=("Helvetica", 9, "bold"),
                  relief="flat", bd=0, cursor="hand2",
                  activebackground=bg, activeforeground=fg,
                  padx=6, pady=6)
    b.pack(side=tk.LEFT, padx=7)
    return b

tk.Label(btn_bar, text="  Controls:", bg="#dde3ea", fg="#475569",
         font=("Helvetica", 9)).pack(side=tk.LEFT, padx=(12, 0))

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
        # ── Header ────────────────────────────────────────────────────────────
        f.write("=" * 68 + "\n")
        f.write("  ADAPTIVE CPU PERSONA MANAGER — SESSION REPORT\n")
        f.write("=" * 68 + "\n")
        f.write(f"  Date/Time  : {session_dt.strftime('%Y-%m-%d %H:%M:%S')}\n")
        f.write(f"  Duration   : {duration_s:.1f} s\n")
        f.write(f"  Samples    : {len(records)}\n")
        f.write(f"  CPU cores  : {num_cores}\n")
        f.write("=" * 68 + "\n\n")

        # ── System-wide summary ───────────────────────────────────────────────
        f.write("SYSTEM-WIDE SUMMARY\n")
        f.write("-" * 68 + "\n")
        f.write(f"{'Metric':<22} {'Min':>10} {'Avg':>10} {'Max':>10}\n")
        f.write("-" * 68 + "\n")

        metrics = [
            ("CPU Util (%)",      "util",    "{:.1f}"),
            ("Avg Freq (MHz)",    "mhz",     "{:.0f}"),
            ("IPC",               "ipc",     "{:.3f}"),
            ("I/O Wait (%)",      "iowait",  "{:.2f}"),
            ("Ctx Switches /s",   "ctxsw",   "{:.0f}"),
            ("Temperature (°C)",  "temp",    "{:.1f}"),
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

        # ── Per-core summary ──────────────────────────────────────────────────
        if num_cores > 0:
            f.write("PER-CORE SUMMARY\n")
            f.write("-" * 68 + "\n")
            f.write(f"  {'Core':<8} {'Avg Util (%)':>14} {'Avg Freq (MHz)':>16}\n")
            f.write("-" * 68 + "\n")

            for core in range(num_cores):
                u_vals = [r["core_util"][core] for r in records
                          if "core_util" in r and core < len(r["core_util"])]
                m_vals = [r["core_mhz"][core]  for r in records
                          if "core_mhz"  in r and core < len(r["core_mhz"])]
                avg_u = sum(u_vals) / len(u_vals) if u_vals else 0.0
                avg_m = sum(m_vals) / len(m_vals) if m_vals else 0.0
                f.write(f"  CPU {core:<4d} {avg_u:>14.1f} {avg_m:>16.0f}\n")
            f.write("\n")

       # ── Time-series CSV — saved as separate file ──────────────────────────────
    csv_fname = fname.replace(".txt", "_timeseries.csv")
    csv_fpath = os.path.join(RESULTS_DIR, csv_fname)

    csv_cols = ["t_s", "util", "mhz", "ipc", "iowait", "ctxsw", "temp", "power"]
    for c in range(num_cores):
        csv_cols += [f"core{c}_util", f"core{c}_mhz"]

    with open(csv_fpath, "w") as cf:
        cf.write(",".join(csv_cols) + "\n")
        for r in records:
            row = [
                f"{r.get('t', 0):.1f}",
                f"{r.get('util',   0):.2f}",
                f"{r.get('mhz',    0):.1f}",
                f"{r.get('ipc',    0):.3f}",
                f"{r.get('iowait', 0):.2f}",
                f"{r.get('ctxsw',  0):.0f}",
                f"{r.get('temp',  -1):.1f}",
                f"{r.get('power', -1):.2f}",
            ]
            cu = r.get("core_util", [])
            cm = r.get("core_mhz",  [])
            for c in range(num_cores):
                row.append(f"{cu[c]:.1f}" if c < len(cu) else "0")
                row.append(f"{cm[c]:.0f}" if c < len(cm) else "0")
            cf.write(",".join(row) + "\n")

    f.write(f"  Time-series : {csv_fname}\n")
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
    timestamps.clear()
    for v in data_series.values():
        v.clear()
    all_records.clear()
    start_time = None
    for key, ln in lines_map.items():
        ln.set_data([], [])
        axes_map[key].relim()
    canvas.draw_idle()

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
    # Generate report in background so GUI stays responsive
    if all_records and session_start_wall:
        duration = timestamps[-1] if timestamps else 0.0
        recs     = list(all_records)
        dt       = session_start_wall
        threading.Thread(
            target=generate_report, args=(recs, dt, duration), daemon=True
        ).start()

def on_close():
    global is_monitoring
    is_monitoring = False
    shutdown_event.set()
    try: send_command({"cmd": "QUIT"})
    except Exception: pass
    root.destroy()

root.protocol("WM_DELETE_WINDOW", on_close)

# ── Buttons ───────────────────────────────────────────────────────────────────
_btn(btn_bar, "▶  Start",        on_start,  "#22c55e")
_btn(btn_bar, "■  Stop",         on_stop,   "#ef4444")
_btn(btn_bar, "⟳  Auto Mode",    lambda: send_command({"cmd": "SET_MODE", "mode": "AUTO"}),       "#3b82f6")
_btn(btn_bar, "📌  Manual",       lambda: send_command({"cmd": "SET_MODE", "mode": "MANUAL"}),     "#475569")
_btn(btn_bar, "⚡  Performance",  lambda: send_command({"cmd": "SET_GOV",  "gov": "performance"}), "#f59e0b")
_btn(btn_bar, "🌿  Powersave",    lambda: send_command({"cmd": "SET_GOV",  "gov": "powersave"}),   "#10b981")

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
            print("[fifo] C++ exited — closing GUI")
            root.after(0, on_close)
    except Exception as e:
        if not shutdown_event.is_set():
            print(f"[fifo] Error: {e}")
            root.after(0, on_close)

# ─────────────────────────────────────────────────────────────────────────────
# GUI update loop
# ─────────────────────────────────────────────────────────────────────────────

def _temp_color(t):
    if t < 0:   return "#475569"
    if t < 55:  return "#10b981"
    if t < 75:  return "#f59e0b"
    return              "#ef4444"

def update_gui():
    global start_time

    if is_monitoring:
        updated = False
        while not data_queue.empty():
            try:
                data = data_queue.get_nowait()
            except queue.Empty:
                break

            if "util" not in data or "mhz" not in data:
                continue

            if start_time is None:
                start_time = time.time()

            t = time.time() - start_time
            timestamps.append(t)
            data["t"] = t

            for key in SCALAR_KEYS:
                data_series[key].append(data.get(key, 0.0))

            all_records.append(data)

            # Rolling window trim
            if len(timestamps) > MAX_POINTS:
                timestamps.pop(0)
                all_records.pop(0)
                for v in data_series.values():
                    if v: v.pop(0)

            # Status badges
            temp  = data.get("temp",  -1.0)
            power = data.get("power", -1.0)
            temp_badge.config(
                text=f"{temp:.0f} °C" if temp >= 0 else "—",
                bg=_temp_color(temp))
            power_badge.config(
                text=f"{power:.1f} W" if power >= 0 else "—")

            updated = True

        if updated:
            ts = timestamps
            for key, ln in lines_map.items():
                vals = data_series[key]
                n    = min(len(ts), len(vals))
                if n > 0:
                    ln.set_data(ts[:n], vals[:n])
                    axes_map[key].relim()
                    axes_map[key].autoscale_view()
            canvas.draw_idle()

    root.after(100, update_gui)

# ─────────────────────────────────────────────────────────────────────────────
# Launch
# ─────────────────────────────────────────────────────────────────────────────
print("[gui] Starting Adaptive CPU Manager …")
threading.Thread(target=read_fifo, daemon=True).start()
root.after(100, update_gui)
root.mainloop()