import json
import tkinter as tk
import threading
import queue
import time

from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
import matplotlib.pyplot as plt

monitor_pipe_path = "config/monitor_pipe"
control_pipe_path = "config/event_pipe"

timestamps = []
cpu_vals  = []
mhz_vals  = []
ipc_vals  = []

start_time    = None
data_queue    = queue.Queue()
is_monitoring = False
shutdown_event = threading.Event()


root = tk.Tk()
root.title("OS Monitor - Turbostat")

fig, (ax_cpu, ax_mhz) = plt.subplots(1, 2, figsize=(10, 4))

line_cpu, = ax_cpu.plot([], [], color="tab:blue")
ax_cpu.set_title("CPU Utilization (%)")
ax_cpu.set_xlabel("Time (s)")
ax_cpu.set_ylabel("Util (%)")

line_mhz, = ax_mhz.plot([], [], color="tab:orange")
ax_mhz.set_title("Average MHz")
ax_mhz.set_xlabel("Time (s)")
ax_mhz.set_ylabel("MHz")

fig.tight_layout()

canvas = FigureCanvasTkAgg(fig, master=root)
canvas.get_tk_widget().pack(fill=tk.BOTH, expand=1)

button_frame = tk.Frame(root)
button_frame.pack(pady=10) # pady is y padding


def send_command(cmd_dict):
    try:
        with open(control_pipe_path, "w") as pipe:
            pipe.write(json.dumps(cmd_dict) + "\n")
    except Exception as e:
        print("Control pipe error:", e)

def on_start():
    global is_monitoring, start_time

    # Clear all stored data
    timestamps.clear()
    cpu_vals.clear()
    mhz_vals.clear()
    ipc_vals.clear()
    start_time = None

    # Drain any stale queued data
    while not data_queue.empty():
        try:
            data_queue.get_nowait()
        except queue.Empty:
            break

    # Reset graph lines
    line_cpu.set_data([], [])
    line_mhz.set_data([], [])
    ax_cpu.relim()
    ax_mhz.relim()
    canvas.draw_idle()

    is_monitoring = True
    send_command({"cmd": "START"})

def on_stop():
    global is_monitoring
    is_monitoring = False
    send_command({"cmd": "STOP"})

def on_close():
    global is_monitoring
    is_monitoring = False
    shutdown_event.set()
    try:
        send_command({"cmd": "QUIT"})
    except Exception:
        pass
    root.destroy()

root.protocol("WM_DELETE_WINDOW", on_close)


tk.Button(button_frame, text="Start",
          command=on_start, bg="#4CAF50", fg="white", width=10).pack(side=tk.LEFT, padx=5)

tk.Button(button_frame, text="Stop",
          command=on_stop, bg="#f44336", fg="white", width=10).pack(side=tk.LEFT, padx=5)

tk.Button(button_frame, text="Auto Mode",
          command=lambda: send_command({"cmd": "SET_MODE", "mode": "AUTO"})).pack(side=tk.LEFT, padx=5)

tk.Button(button_frame, text="Manual Mode",
          command=lambda: send_command({"cmd": "SET_MODE", "mode": "MANUAL"})).pack(side=tk.LEFT, padx=5)

tk.Button(button_frame, text="Performance",
          command=lambda: send_command({"cmd": "SET_GOV", "gov": "performance"})).pack(side=tk.LEFT, padx=5)

tk.Button(button_frame, text="Powersave",
          command=lambda: send_command({"cmd": "SET_GOV", "gov": "powersave"})).pack(side=tk.LEFT, padx=5)



def read_fifo():
    try:
        print(f"Waiting to connect to FIFO at {monitor_pipe_path}...")
        with open(monitor_pipe_path, "r") as pipe:
            print("Connected to monitor FIFO")
            for line_data in pipe:
                if shutdown_event.is_set():
                    break
                try:
                    data = json.loads(line_data.strip())
                    data_queue.put(data)
                except json.JSONDecodeError:
                    continue
        # this check is to prevent program trying to destroy root after its already destroyed.
        # if terminal is closed during execution/ pipe is closed it reaches this line and destroyed GUI
        # if GUI is closed using close button, pipe is destroyed and it again reaches this line. but set is true so condition becomes false
        if not shutdown_event.is_set():
            print("C++ process has exited, closing GUI...")
            root.after(0, on_close)
    except Exception as e:
        if not shutdown_event.is_set():
            print(f"FIFO reader error: {e}")
            root.after(0, on_close)


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
            cpu_vals.append(data["util"])
            mhz_vals.append(data["mhz"])
            ipc_vals.append(data.get("ipc", 0))
            updated = True

        if updated:
            line_cpu.set_data(timestamps, cpu_vals)
            ax_cpu.relim()
            ax_cpu.autoscale_view()

            line_mhz.set_data(timestamps, mhz_vals)
            ax_mhz.relim()
            ax_mhz.autoscale_view()

            canvas.draw_idle()

    root.after(100, update_gui)

# ---------------- START ----------------

print("Starting OS Monitor GUI...")
print(f"Looking for FIFO at: {monitor_pipe_path}")

threading.Thread(target=read_fifo, daemon=True).start()
root.after(100, update_gui)

print("GUI window should appear now...")
root.mainloop()

