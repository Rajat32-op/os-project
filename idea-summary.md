**Project summary (concise)**

Build a **hybrid-core aware Linux runtime manager** that improves performance-per-watt by combining workload detection, core placement, and frequency control.

**Main idea**

1. **Workload mode detection** using system metrics (CPU utilization, I/O wait, context switches) to classify the system as **compute mode** or **interactive mode**.
2. **Hybrid core scheduling:** place compute/latency-sensitive tasks on **P-cores** and background/I/O tasks on **E-cores** using CPU affinity.
3. **Per-core phase detection + DVFS:** detect compute-bound vs memory/I/O-bound phases on each core and adjust CPU frequency accordingly.

**Additional features added**

* **Confidence filter:** only change scheduling policies if the detected mode is stable for several consecutive samples, preventing oscillation.
* **Core parking:** when load is low, consolidate tasks onto fewer cores (preferably E-cores) and **disable unused cores** to save power.
* **Performance safeguard:** continuously monitor performance; if degradation exceeds a threshold, automatically revert to the previous configuration.

**Optional extension**

* **Exploration / reinforcement learning:** periodically test alternative scheduling or frequency policies and learn which configuration provides the best **performance-per-watt**.

**Goal**
Create an **adaptive hybrid-CPU scheduler + power manager** that dynamically balances **performance, responsiveness, and energy efficiency** better than default Linux scheduling.
