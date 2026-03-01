import json
import tkinter as tk
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
import matplotlib.pyplot as plt

PIPE_PATH = "config/monitor_pipe"

timestamps = []
cpu_vals = []
energy_vals=[]
start_time = None

root = tk.Tk()
root.title("OS Monitor")

fig, (ax_cpu, ax_energy) = plt.subplots(1, 2, figsize=(10, 4))
line, = ax_cpu.plot([], [])
ax_cpu.set_title("CPU Utilization Over Time")
ax_cpu.set_xlabel("Time (ms)")
ax_cpu.set_ylabel("CPU Utilization (%)")

bar = ax_energy.bar(["Energy"], [0],width=0.3)
energy_text = ax_energy.text(0, 0, "", ha='center', va='bottom')
ax_energy.set_title("Energy (J)")
ax_energy.set_ylim(0,10)

canvas = FigureCanvasTkAgg(fig, master=root)
canvas.get_tk_widget().pack(fill=tk.BOTH, expand=1)

def read_fifo():
    with open(PIPE_PATH, "r") as pipe:
        print("FIFO connected")

        for line_data in pipe:

            data = json.loads(line_data.strip())

            global start_time

            if start_time is None:
                start_time = data["timestamp"]

            relative_time = (data["timestamp"] - start_time) / 1000.0
            timestamps.append(relative_time)
            cpu_vals.append(data["total_cpu"])
            energy_vals.append(data["energy"])

            line.set_data(timestamps,cpu_vals )
            ax_cpu.relim()
            ax_cpu.autoscale_view()

            energy_val = data["energy"]

            bar[0].set_height(energy_val)
            energy_text.set_text(f"{energy_val:.2f} J")
            energy_text.set_position((0, energy_val))
            canvas.draw()

    print("Writer closed")
    root.quit()

# Run FIFO reading in background
import threading
threading.Thread(target=read_fifo, daemon=True).start()

root.mainloop()
