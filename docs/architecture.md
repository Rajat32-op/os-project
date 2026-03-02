# Architecture

## Overview

The project is split into two processes: a **C++ backend** that collects CPU metrics using `turbostat`, and a **Python GUI frontend** that visualises them. They communicate via named FIFOs (pipes on the filesystem).

```
┌──────────────────────────────────────────────────────┐
│                   C++ Backend (os_manager)           │
│                                                      │
│  main()                                              │
│    └── EventController                               │
│            └── listenLoop() [thread]                 │
│                    └── MonitorManager                │
│                            ├── start() / stop()      │
│                            └── readerLoop() [thread] │
│                                    │                 │
│                             turbostat [child process]│
└────────────────────┬─────────────────────────────────┘
                     │
          Named FIFOs (config/)
          ┌──────────┴──────────┐
          │                     │
   monitor_pipe             event_pipe
   (JSON metrics)           (commands)
   C++ → Python             Python → C++
          │                     │
└─────────┴─────────────────────┴──────────────────────┐
│                  Python Frontend (monitor.py)         │
│                                                      │
│  read_fifo() [thread] ──► data_queue                 │
│  update_gui() [tk loop] ──► matplotlib graphs        │
│  Buttons ──► send_command() ──► event_pipe           │
└──────────────────────────────────────────────────────┘
```

---

## Components

### `main.cpp`
Entry point. Instantiates `EventController` and blocks with `pause()` until `SIGINT` or `SIGTERM`.

---

### `EventController` (`core/controllers/`)
Runs a background thread (`listenLoop`) that reads JSON commands from `config/event_pipe`.

Supported commands:
| Command | Action |
|---|---|
| `START` | Calls `MonitorManager::start()` |
| `STOP` | Calls `MonitorManager::stop()` |
| `QUIT` | Calls `exit(0)` |

---

### `MonitorManager` (`core/monitor/`)
Manages the lifecycle of `turbostat` and the data pipeline.

**On `start()`:**
1. Creates an anonymous pipe (`pipefd`)
2. Opens `config/monitor_pipe` FIFO for writing (once; reused across cycles)
3. `fork()`s a child — child redirects stdout to `pipefd[1]` and `exec`s `turbostat`
4. Spawns `readerLoop()` thread to consume `pipefd[0]`

**On `stop()`:**
1. Sets `running = false`
2. Sends `SIGTERM` to `turbostat`, waits for it
3. Joins `readerLoop` thread
4. Closes `pipefd` — `monitor_pipe` FIFO stays open

**`readerLoop()`:**
Reads raw turbostat text lines, parses columns, builds a JSON string, and writes it to `fifo_fd` (the monitor FIFO). Exits with `exit(0)` on `EPIPE` (Python died).

---

### `monitor.py` (`interface/`)
Tkinter GUI with two matplotlib graphs (CPU utilisation %, Average MHz).

- `read_fifo()` — daemon thread; opens `config/monitor_pipe` and pushes parsed JSON into `data_queue`. On EOF, triggers `on_close()` (C++ exited).
- `update_gui()` — Tkinter `after` loop every 100 ms; only plots when `is_monitoring` is `True`.
- `on_start()` — clears all data/graphs, sets `is_monitoring = True`, sends `START`.
- `on_stop()` — sets `is_monitoring = False`, sends `STOP`. Graph freezes.
- `on_close()` — sets `shutdown_event`, sends `QUIT`, destroys window.

---

## IPC: Two FIFOs

| FIFO | Path | Direction | Format |
|---|---|---|---|
| Monitor pipe | `config/monitor_pipe` | C++ → Python | JSON per line: `{"util":…,"mhz":…,"ipc":…}` |
| Event pipe | `config/event_pipe` | Python → C++ | JSON per line: `{"cmd":"START"}` etc. |

Both are named FIFOs created by the makefile with `mkfifo`.

---

## Data Flow (per cycle)

```
turbostat stdout
      │ (raw text rows)
      ▼
pipefd[0] ──► readerLoop() ──► parse columns ──► JSON string
                                                      │
                                                  fifo_fd
                                                      │
                                           config/monitor_pipe
                                                      │
                                               read_fifo() [Python]
                                                      │
                                               data_queue
                                                      │
                                              update_gui()
                                                      │
                                           matplotlib graphs
```

---

## Shutdown Behaviour

| Trigger | What happens |
|---|---|
| GUI window closed | `on_close()` sends `QUIT` → C++ calls `exit(0)` |
| C++ exits (any reason) | `fifo_fd` closes → Python sees EOF → `on_close()` called |
| Terminal Ctrl+C | `SIGINT` → `handle_signal()` → `main()` returns → destructors run → `fifo_fd` closes → Python closes |
| `make run` Ctrl+C | makefile kills Python PID after C++ exits |
