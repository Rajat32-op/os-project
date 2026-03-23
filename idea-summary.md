# Project Design — Adaptive CPU Persona Manager
> Last updated: March 2026
> Course: CS3523 Operating Systems II — IIT Hyderabad (Track P5: Linux)

---

## Elevator Pitch

A four-mode adaptive CPU manager that simultaneously optimizes core frequency, uncore
frequency, core parking, task affinity, and page cache pressure based on runtime workload
classification. Unlike existing Linux governors which react only to utilization%, this system
uses IPS frequency-sensitivity probing to distinguish compute-bound from memory-bound
phases within each mode, and introduces a latency-critical mode for mixed
interactive-compute workloads (e.g. gaming) where probe-based adaptation is explicitly
suppressed to prevent configuration thrashing.

---

## Academic Framing (for report)

> "We propose a four-mode adaptive CPU manager where each mode applies a topology-aware
> configuration across core frequency, uncore frequency, core parking, and page cache
> pressure. Within compute and interactive modes, a secondary IPS-sensitivity probe derived
> from ReAPER distinguishes compute-bound from memory-bound phases and applies orthogonal
> optimizations to each. A third latency-critical mode is introduced for mixed
> interactive-compute workloads characterized by periodic burst patterns, where probe-based
> adaptation is explicitly suppressed to prevent configuration thrashing."

### Key citations
- **ReAPER** (Yue et al., SC-W 2023) — IPS-based compute/memory boundedness formula
- **EVeREST** (Yue et al., PPoPP 2025) — frequency-sensitivity probing, voltage-frequency
  floor insight, phase detection with stability window

---

## The 4 Modes

| Mode | Trigger signature | User analogy |
|---|---|---|
| IDLE | util% low across all cores | Screen dimmed, nothing running |
| INTERACTIVE | Bursty util%, high context switches, mixed I/O | Normal desktop use |
| COMPUTE | Sustained high util%, high IPC, low context switches | Video encoding, ML training |
| LATENCY-CRITICAL | High util% + high context switches + periodic burst pattern | Gaming, real-time audio |

AUTO is the default — the primary classifier picks the mode automatically.
User can pin any mode manually from the GUI.

---

## Primary Classifier Inputs

Read from `/proc` and `/sys` — no sudo required for reads:

| Metric | Source |
|---|---|
| CPU util % per core | `/proc/stat` |
| IPC (instructions per cycle) | `perf_event_open` syscall |
| Context switches | `/proc/stat` (or `/proc/vmstat`) |
| I/O wait % | `/proc/stat` |
| Current freq per core | `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq` |
| Core online/offline status | `/sys/devices/system/cpu/cpu*/online` |
| Temperature | `/sys/class/thermal/thermal_zone*/temp` |
| Periodic burst pattern | Rolling autocorrelation on util% time series |

### Confidence Filter
Only switch modes after N consecutive samples (e.g. 5s window) agree on the new mode.
Prevents oscillation — directly inspired by EVeREST's stability window approach.

---

## Secondary Classifier — IPS Probe

Runs **inside** COMPUTE and INTERACTIVE modes.
Skipped entirely in LATENCY-CRITICAL (too risky) and IDLE (trivially memory-bound).

Based directly on ReAPER's formula:

```
%CB = 100% × (IPS_high/IPS_low - 1) / (Freq_high/Freq_low - 1)
%MB = 100% - %CB

Freq_ideal = Freq_high / (1 + PD / (%CB × (1 - PD)))
```

Where PD = Performance Degradation budget (user-configurable slider, 0–20%).

### Frequency Floor
From EVeREST's voltage-frequency insight: below ~60-70% of max freq, voltage plateau
means diminishing power returns and energy can actually increase. Never go below this
floor. Calibrated once per machine on first launch.

---

## Config Matrix — What Gets Set Per State

```
                     core_freq    uncore_freq   parking    governor     cache_pressure
─────────────────────────────────────────────────────────────────────────────────────
IDLE                 minimum      minimum       max park   powersave    high
INTERACTIVE + CB     IPS ideal    low           none       schedutil    medium
INTERACTIVE + MB     IPS ideal    high          half       schedutil    low
COMPUTE + CB         high         low           none       performance  medium
COMPUTE + MB         IPS ideal    high          half       performance  low
LATENCY-CRITICAL     maximum      maximum       none       performance  low
```

### Actuator Paths (all need root for writes)

| Actuator | Path |
|---|---|
| CPU governor | `/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor` |
| CPU freq (userspace) | `/sys/devices/system/cpu/cpu*/cpufreq/scaling_setspeed` |
| Core online/offline | `/sys/devices/system/cpu/cpu*/online` |
| Uncore freq (Intel) | `/sys/devices/system/cpu/intel_uncore_frequency/package_00_die_00/{min,max}_freq_khz` |
| Task affinity | `sched_setaffinity` syscall |
| Page cache pressure | `/proc/sys/vm/vfs_cache_pressure` |

Note: Uncore freq is Intel-only. Graceful fallback for AMD — skip silently, log unavailable.

---

## Memory-Bound Specific Optimizations

Memory-bound is not a top-level mode — it is a sub-decision within COMPUTE and
INTERACTIVE. When detected, four things happen simultaneously:

1. **Lower core frequency** to IPS-derived ideal (not below frequency floor)
2. **Raise uncore frequency** — memory controller needs bandwidth, not compute
3. **Core consolidation / parking** — fewer cores = less memory bus contention per core,
   same throughput with less power
4. **Lower vfs_cache_pressure** — keep working set in page cache, reduce DRAM fetches

This treats the memory subsystem as the primary resource rather than an afterthought —
no standard Linux governor does all four simultaneously.

---

## Latency-Critical Mode

### Detection signature
```
high util%
+ high context switches       (threads constantly handing off)
+ low IPC variance            (each thread does predictable work)
+ periodic burst pattern      (frame loop ~16-50ms, detectable via autocorrelation)
```

The periodic burst pattern is the key differentiator from pure compute.

### What it does
- Full performance governor on all cores
- Maximum uncore frequency
- No core parking (need all cores for burst frames)
- No affinity overrides (let scheduler work freely)
- Confidence filter temporarily relaxed (respond fast)
- Low vfs_cache_pressure (keep assets in page cache)
- IPS probe suppressed entirely

### Academic contribution
Existing two-class (compute/memory) frameworks misclassify gaming workloads and thrash
between policies. This mode detects the pattern and holds a stable configuration.

---

## Performance Safeguard

Wraps every mode/config change:

1. Record baseline IPC before applying new config
2. Monitor IPC for 10s after change
3. If IPC drops >20% → auto-revert to previous config
4. Log the revert event → show in GUI with timestamp

Makes the manager self-correcting. No other student project will have this.

---

## Sudo / Deployment Strategy

Option A (chosen): sudoers entry via `make install`
```makefile
install:
    echo "$$USER ALL=(ALL) NOPASSWD: $(CURDIR)/os_manager" | \
    sudo tee /etc/sudoers.d/os_manager
    sudo chmod 440 /etc/sudoers.d/os_manager
```
One-time setup. Works on every Linux distro. How real tools (tlp, cpupower) handle it.

---

## GUI Layout

```
┌─────────────────────────────────────────────────┐
│  Mode:  [ AUTO ▼ ]                              │
│         [ IDLE         ]                        │
│         [ INTERACTIVE  ]                        │
│         [ COMPUTE      ]                        │
│         [ LATENCY-CRIT ]                        │
│                                                 │
│  PD Budget:  [────●────────] 10%               │
│                                                 │
│  Current:  COMPUTE (memory-bound)              │
│  Safeguard: ✓ no revert in last 60s            │
│                                                 │
│  [Live graphs: util%, freq, IPC, temp]         │
│  [Per-core heatmap]                            │
│  [Safeguard revert log]                        │
└─────────────────────────────────────────────────┘
```

---

## Build Order

```
1.  /proc + /sys reader — replaces turbostat entirely (no sudo for reads)
2.  perf_event_open IPS measurement
3.  Primary classifier (util, IPC, ctxsw, iowait, burst detection)
4.  Confidence filter
5.  IPS probe + ReAPER formula + frequency floor calibration
6.  Governor switching
7.  Core parking / unparking
8.  Uncore frequency (Intel, with AMD fallback)
9.  Task affinity setter
10. vfs_cache_pressure tuning
11. Performance safeguard
12. GUI: mode selector, PD slider, per-core heatmap, safeguard log
13. Stress test evaluation (stress-ng scenarios per mode)
```

---

## What Makes This Novel (checklist for report)

- [ ] IPS-sensitivity probing instead of util% (cite ReAPER)
- [ ] Memory-bound sub-classification within each mode
- [ ] Simultaneous 5-dimensional config (freq + uncore + parking + affinity + cache)
- [ ] Frequency floor enforcement (cite EVeREST)
- [ ] Latency-critical mode with burst pattern detection
- [ ] Confidence filter preventing oscillation
- [ ] Performance safeguard with auto-revert
- [ ] User-configurable PD budget exposed in GUI
- [ ] Deployable on any Linux machine via sudoers entry