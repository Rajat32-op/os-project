#include "monitor_manager.h"
#include "../calibrator/calibrator.h"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include<syscall.h>
#include <time.h>

// ── Helpers ───────────────────────────────────────────────────────────────
double get_time_seconds() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static int perf_event_open(struct perf_event_attr *attr, pid_t pid,
                           int cpu, int group_fd, unsigned long flags) {
    return (int)syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static std::string dtos(double v, int prec = 3) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return std::string(buf);
}

// ── Constructor / Destructor ──────────────────────────────────────────────
MonitorManager::MonitorManager()
    : running(false), fifo_fd(-1), first_sample(true),
      prev_energy_uj(-1), rapl_available(false),
      last_committed(CpuMode::IDLE), prev_pg_io(0),
      is_pinned(false), pinned_mode(CpuMode::IDLE), rl_enabled(false), sum_power(0.0), sum_ipc(0.0), sum_util(0.0), sum_freq(0.0), sample_count(0), action_changes(0), run_start_time(0.0), run_end_time(0.0), summary_path(""), csv_path(""), csv_fp(nullptr), csv_header_written(false), prev_time(-1.0), last_action("DO_NOTHING"), last_score(0.0) {
    num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus < 1) num_cpus = 1;
    memset(&prev_total, 0, sizeof(prev_total));
    prev_ctxt = 0;
    prev_core.resize(num_cpus);
    for (auto &c : prev_core) memset(&c, 0, sizeof(c));

    // Read runtime mode from environment
    const char *mode = getenv("OSMGR_MODE");
    if (mode && strcmp(mode, "RL") == 0) {
        setRLEnabled(true);
        if (summary_path.empty()) summary_path = "results/raw/rl.summary.csv";
        if (csv_path.empty()) csv_path = "results/raw/rl.csv";
    } else {
        setRLEnabled(false);
        if (summary_path.empty()) summary_path = "results/raw/baseline.summary.csv";
        if (csv_path.empty()) csv_path = "results/raw/baseline.csv";
    }
    const char *summary_env = getenv("OSMGR_SUMMARY");
    if (summary_env) summary_path = summary_env;
    const char *csv_env = getenv("OSMGR_CSV");
    if (csv_env) csv_path = csv_env;

    // Load calibration → derive thresholds
    classifier.loadCalibration(Calibrator::CALIB_PATH);

    // Init governor → discover available governors
    governor.init();
}

MonitorManager::~MonitorManager() {
    stop();
    if (fifo_fd >= 0) { close(fifo_fd); fifo_fd = -1; }
    if (csv_fp) { fclose(csv_fp); csv_fp = nullptr; }
}

// ── Mode pin / auto ───────────────────────────────────────────────────────
void MonitorManager::setPinnedMode(CpuMode m) {
    std::lock_guard<std::mutex> lk(mode_mutex);
    is_pinned   = true;
    pinned_mode = m;
    // Apply governor immediately on pin
    governor.apply(m);
    last_committed = m;
    printf("[mode] Pinned to %s\n", modeToString(m).c_str());
}

void MonitorManager::setAuto() {
    std::lock_guard<std::mutex> lk(mode_mutex);
    is_pinned = false;
    printf("[mode] Switched to AUTO\n");
}

// ── perf counters ─────────────────────────────────────────────────────────
bool MonitorManager::openPerfCounters() {
    perf_fds.clear();
    perf_fds.resize(num_cpus, {-1, -1, -1});
    for (int cpu = 0; cpu < num_cpus; ++cpu) {
        struct perf_event_attr pe{};
        pe.size = sizeof(pe); pe.type = PERF_TYPE_HARDWARE;
        pe.exclude_hv = 1;
        pe.config = PERF_COUNT_HW_CPU_CYCLES;
        int fd_c = perf_event_open(&pe, -1, cpu, -1, 0);
        if (fd_c < 0) { printf("[perf] cycles failed cpu%d\n", cpu); return false; }
        pe.config = PERF_COUNT_HW_INSTRUCTIONS;
        int fd_i = perf_event_open(&pe, -1, cpu, -1, 0);
        if (fd_i < 0) { close(fd_c); printf("[perf] instrs failed cpu%d\n", cpu); return false; }
        pe.config = PERF_COUNT_HW_CACHE_MISSES;
        int fd_miss = perf_event_open(&pe, -1, cpu, -1, 0); // Ignore error as cache misses may not be available on all arch
        perf_fds[cpu] = {fd_c, fd_i, fd_miss};
    }
    printf("[perf] Opened counters for %d CPUs\n", num_cpus);
    return true;
}

void MonitorManager::closePerfCounters() {
    for (auto &pf : perf_fds) {
        if (pf.fd_cycles >= 0) { close(pf.fd_cycles); pf.fd_cycles = -1; }
        if (pf.fd_instrs >= 0) { close(pf.fd_instrs); pf.fd_instrs = -1; }
        if (pf.fd_cache_misses >= 0) { close(pf.fd_cache_misses); pf.fd_cache_misses = -1; }
    }
    perf_fds.clear();
}

// ── RAPL ──────────────────────────────────────────────────────────────────
void MonitorManager::initRAPL() {
    const char* candidates[] = {
        "/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj",
        "/sys/class/powercap/intel-rapl:0/energy_uj",
        nullptr
    };
    for (int i = 0; candidates[i]; ++i) {
        FILE *f = fopen(candidates[i], "r");
        if (f) {
            fclose(f);
            rapl_path = candidates[i];
            rapl_available = true;
            prev_energy_uj = -1;
            printf("[rapl] Using %s\n", rapl_path.c_str());
            return;
        }
    }
    printf("[rapl] Not available — power will be -1\n");
    rapl_available = false;
}

double MonitorManager::readRAPL() {
    if (!rapl_available) return -1.0;

    FILE *f = fopen(rapl_path.c_str(), "r");
    if (!f) return -1.0;

    long long energy_uj = 0;
    int rc = fscanf(f, "%lld", &energy_uj);
    fclose(f);
    if (rc != 1) return -1.0;

    double now = get_time_seconds(); // use clock_gettime or similar
    double watts = -1.0;

    if (prev_energy_uj >= 0 && prev_time >= 0) {
        long long delta_energy = energy_uj - prev_energy_uj;
        if (delta_energy < 0) delta_energy = 0;

        double delta_time = now - prev_time;
        if (delta_time > 0) {
            watts = (delta_energy / 1e6) / delta_time;
        }
    }

    prev_energy_uj = energy_uj;
    prev_time = now;

    return watts;
}

// ── readProcStat ──────────────────────────────────────────────────────────
bool MonitorManager::readProcStat(double &util_pct, double &iowait_pct,
                                   double &ctxsw_rate,
                                   std::vector<double> &core_utils) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return false;

    CoreStat cur_total{};
    long     cur_ctxt = 0;
    std::vector<CoreStat> cur_core(num_cpus);
    for (auto &c : cur_core) memset(&c, 0, sizeof(c));

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            sscanf(line, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
                   &cur_total.user, &cur_total.nice, &cur_total.system,
                   &cur_total.idle, &cur_total.iowait,
                   &cur_total.irq,  &cur_total.softirq, &cur_total.steal);
        } else if (strncmp(line, "cpu", 3) == 0 &&
                   line[3] >= '0' && line[3] <= '9') {
            int id = 0; CoreStat cs{};
            sscanf(line, "cpu%d %ld %ld %ld %ld %ld %ld %ld %ld",
                   &id, &cs.user, &cs.nice, &cs.system, &cs.idle,
                   &cs.iowait, &cs.irq, &cs.softirq, &cs.steal);
            if (id >= 0 && id < num_cpus) cur_core[id] = cs;
        } else if (strncmp(line, "ctxt", 4) == 0) {
            sscanf(line, "ctxt %ld", &cur_ctxt);
        }
    }
    fclose(f);

    core_utils.resize(num_cpus, 0.0);

    if (first_sample) {
        prev_total = cur_total; prev_core = cur_core; prev_ctxt = cur_ctxt;
        first_sample = false;
        util_pct = iowait_pct = ctxsw_rate = 0.0;
        return true;
    }

    auto tot_delta = [](const CoreStat &c, const CoreStat &p) {
        return (c.user-p.user)+(c.nice-p.nice)+(c.system-p.system)+
               (c.idle-p.idle)+(c.iowait-p.iowait)+
               (c.irq-p.irq)+(c.softirq-p.softirq)+(c.steal-p.steal);
    };

    long total = tot_delta(cur_total, prev_total);
    long idle  = cur_total.idle   - prev_total.idle;
    long iow   = cur_total.iowait - prev_total.iowait;

    util_pct   = (total > 0) ? 100.0*(total-idle-iow)/total : 0.0;
    iowait_pct = (total > 0) ? 100.0*iow/total : 0.0;
    ctxsw_rate = (double)(cur_ctxt - prev_ctxt);

    for (int i = 0; i < num_cpus; ++i) {
        long t = tot_delta(cur_core[i], prev_core[i]);
        long d = cur_core[i].idle   - prev_core[i].idle;
        long w = cur_core[i].iowait - prev_core[i].iowait;
        core_utils[i] = (t > 0) ? 100.0*(t-d-w)/t : 0.0;
    }

    prev_total = cur_total; prev_core = cur_core; prev_ctxt = cur_ctxt;
    return true;
}

// ── readFreqs ─────────────────────────────────────────────────────────────
bool MonitorManager::readFreqs(double &avg_mhz, std::vector<double> &core_mhz) {
    core_mhz.resize(num_cpus, 0.0);
    double sum = 0.0; int count = 0;
    for (int cpu = 0; cpu < num_cpus; ++cpu) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        long khz = 0;
        if (fscanf(f, "%ld", &khz) == 1) {
            double mhz = khz / 1000.0;
            core_mhz[cpu] = mhz; sum += mhz; ++count;
        }
        fclose(f);
    }
    avg_mhz = (count > 0) ? sum/count : 0.0;
    return count > 0;
}

// ── readTemperature ───────────────────────────────────────────────────────
double MonitorManager::readTemperature() {
    double best = -1.0; bool has_pkg = false;
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir) return -1.0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "thermal_zone", 12) != 0) continue;
        char type_path[320], temp_path[320];
        snprintf(type_path, sizeof(type_path),
                 "/sys/class/thermal/%s/type", ent->d_name);
        snprintf(temp_path, sizeof(temp_path),
                 "/sys/class/thermal/%s/temp", ent->d_name);
        char type_buf[64] = {};
        FILE *tf = fopen(type_path, "r");
        if (tf) { (void)fscanf(tf, "%63s", type_buf); fclose(tf); }
        FILE *f = fopen(temp_path, "r");
        if (!f) continue;
        long milli_c = 0;
        if (fscanf(f, "%ld", &milli_c) == 1) {
            double deg = milli_c / 1000.0;
            if (strstr(type_buf, "pkg") || strstr(type_buf, "x86")) {
                if (!has_pkg || deg > best) { best = deg; has_pkg = true; }
            } else if (!has_pkg && (best < 0 || deg > best)) {
                best = deg;
            }
        }
        fclose(f);
    }
    closedir(dir);
    return best;
}

// ── readDiskIO ─────────────────────────────────────────────────────────────
double MonitorManager::readDiskIO() {
    FILE *f = fopen("/proc/vmstat", "r");
    if (!f) return 0.0;
    char line[256];
    long long pg_in = 0, pg_out = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "pgpgin ", 7) == 0) {
            sscanf(line + 7, "%lld", &pg_in);
        } else if (strncmp(line, "pgpgout ", 8) == 0) {
            sscanf(line + 8, "%lld", &pg_out);
        }
    }
    fclose(f);
    long long current_total = pg_in + pg_out;
    double delta = 0.0;
    if (prev_pg_io > 0 && current_total >= prev_pg_io) {
        delta = (double)(current_total - prev_pg_io) / 1024.0; // MB
    }
    prev_pg_io = current_total;
    return delta; // MB per ~interval (500ms -> times 2 for MB/s conceptually if needed, but relative scales are fine)
}

// ── readIPC ───────────────────────────────────────────────────────────────
double MonitorManager::readIPC(double &cache_misses) {
    long long tc = 0, ti = 0, tm = 0;
    for (auto &pf : perf_fds) {
        if (pf.fd_cycles < 0 || pf.fd_instrs < 0) continue;
        long long c = 0, i = 0, m = 0;
        if (read(pf.fd_cycles, &c, sizeof(c)) == sizeof(c)) tc += c;
        if (read(pf.fd_instrs, &i, sizeof(i)) == sizeof(i)) ti += i;
        if (pf.fd_cache_misses >= 0 && read(pf.fd_cache_misses, &m, sizeof(m)) == sizeof(m)) tm += m;
        
        ioctl(pf.fd_cycles, PERF_EVENT_IOC_RESET, 0);
        ioctl(pf.fd_instrs, PERF_EVENT_IOC_RESET, 0);
        if (pf.fd_cache_misses >= 0) ioctl(pf.fd_cache_misses, PERF_EVENT_IOC_RESET, 0);
    }
    // Report MPKI (Misses Per Kilo Instruction)
    cache_misses = (ti > 0) ? ((double)tm * 1000.0) / ti : 0.0;
    return (tc > 0) ? (double)ti/tc : 0.0;
}

// ── readerLoop ────────────────────────────────────────────────────────────
void MonitorManager::readerLoop() {
    while (running) {
        double util = 0, iowait = 0, ctxsw = 0;
        std::vector<double> core_utils, core_mhz;

        readProcStat(util, iowait, ctxsw, core_utils);

        double avg_mhz = 0;
        readFreqs(avg_mhz, core_mhz);

        double temp = readTemperature();
        double disk_io = readDiskIO();

        double cache_misses = 0;
        double ipc   = readIPC(cache_misses);
        double power = readRAPL();

        if (util < 0)   util   = 0;
        if (util > 100) util   = 100;
        if (iowait < 0) iowait = 0;

        // ── Mode decision and Policy mapping ──────────────────────────────
        CpuMode mode;
        bool    is_auto;
        
        // 1. Process state through new controller
        SystemState state = adaptive_policy.processState(util, ipc, iowait, disk_io, cache_misses, avg_mhz, temp, power, ctxsw);
        
        // Output real-time state down FIFO to external RL pipeline
        // Reward function: IPC - beta*power. Penalize freq boost if mem/io bound.
        adaptive_policy.setRLEnabled(rl_enabled);
        SysAction action = adaptive_policy.decidePolicy(state);
        last_action = action.name;
        double ipc_n   = state.ipc;
        double util_n  = state.util/100.0;
        double power_n = state.power/30.0;
        double io_n    = state.iowait / 100.0;
        double mem_n   = state.mem_bw / 50.0;
        double freq_n  = state.mhz / 3500.0;

        // Base performance vs power

       double perf =0.5*ipc_n +0.3*freq_n +0.2*util_n;

        double cost =0.7*power_n +0.2*io_n +0.1*mem_n;

        double score =perf - cost;

        double reward = score;

        // --- Context-aware penalties ---

        // 1. CPU-heavy → penalize low-performance modes
        if (util_n > 0.6 && ipc_n > 0.5) {
            if (last_action == "POWERSAVE_50") reward -= 1.0;
        }

        // 2. Memory-bound → penalize aggressive CPU boosting
        if (mem_n > 0.5 && ipc_n < 0.5) {
            if (last_action == "PERF_100") reward -= 1.0;
        }

        // 3. IO-bound → penalize CPU tuning
        if (io_n > 0.4) {
            if (last_action == "PERF_100") reward -= 1.0;
        }

        // 4. Idle → penalize wasting power
        if (util_n < 0.2) {
            if (last_action == "PERF_100") reward -= 1.0;
        }

            
        adaptive_policy.sendStateToRL(state, reward);

        // Collect sample-level statistics for summary output.
        run_end_time = get_time_seconds();
        if (power >= 0.0) sum_power += power;
        sum_ipc += ipc;
        sum_util += util;
        sum_freq += avg_mhz;
        sample_count += 1;

        {
            std::lock_guard<std::mutex> lk(mode_mutex);
            if (is_pinned) {
                mode    = pinned_mode;
                is_auto = false;
            } else {
                mode    = classifier.update(util, ipc, ctxsw, iowait);
                is_auto = true;
            }

            if (mode != last_committed || action.governor != governor.current()) {
                governor.apply(mode); // baseline
                adaptive_policy.applyAction(action);
                last_committed = mode;
                action_changes += 1;
            }
        }

        // Write per-sample data to CSV
        if (csv_fp && csv_header_written) {
            double elapsed = run_end_time - run_start_time;
            fprintf(csv_fp, "%.1f,%.1f,%.3f,%.1f,%.1f,%.1f,%.2f,%s,%s,%d\n",
                    elapsed,
                    util,
                    ipc,
                    iowait,
                    cache_misses,
                    avg_mhz,
                    power,
                    last_action.c_str(),
                    action.source.c_str(),
                    (int)rl_enabled);
            fflush(csv_fp);
        }

        std::string mode_str = modeToString(mode);
        std::string dom_wl = "UNKNOWN";

        int streak     = classifier.getStreak();
        std::string gov = governor.current();

        printf("[%s] Source:%s Gov:%s Util:%.1f%% IPC:%.2f MPKI:%.1f IO:%.1f\n",
               is_auto ? "AUTO  " : "PINNED",
               action.source.c_str(),
               gov.c_str(), util, ipc, cache_misses, disk_io);

        // ── Build JSON ────────────────────────────────────────────────────
        std::string json = "{";
        json += "\"util\":"   + dtos(util,    2) + ",";
        json += "\"mhz\":"    + dtos(avg_mhz, 1) + ",";
        json += "\"ipc\":"    + dtos(ipc,     3) + ",";
        json += "\"iowait\":" + dtos(iowait,  2) + ",";
        json += "\"ctxsw\":"  + dtos(ctxsw,   0) + ",";
        json += "\"temp\":"   + dtos(temp,     1) + ",";
        json += "\"power\":"  + dtos(power,    2) + ",";
        json += "\"mode\":\""  + mode_str + "\",";
        json += "\"source\":\"" + action.source + "\",";
        json += "\"reward\":" + dtos(reward, 3) + ",";
        json += "\"auto\":"    + std::string(is_auto ? "true" : "false") + ",";
        json += "\"streak\":"  + std::to_string(streak) + ",";
        json += "\"gov\":\""   + gov + "\",";

        json += "\"core_util\":[";
        for (int i = 0; i < (int)core_utils.size(); ++i) {
            if (i) json += ",";
            json += dtos(core_utils[i], 1);
        }
        json += "],\"core_mhz\":[";
        for (int i = 0; i < (int)core_mhz.size(); ++i) {
            if (i) json += ",";
            json += dtos(core_mhz[i], 1);
        }
        json += "]}\n";

        if (fifo_fd >= 0) {
            ssize_t n = write(fifo_fd, json.c_str(), json.size());
            if (n < 0 && errno != EAGAIN && errno != EPIPE) {
                // Just log the error, don't crash the monitor loop just because GUI disconnected or buffer full
            }
        }

        // Run cycle at 500ms
        usleep(500000);
    }
}

// ── start / stop ──────────────────────────────────────────────────────────
void MonitorManager::setRLEnabled(bool enabled) {
    rl_enabled = enabled;
    adaptive_policy.setRLEnabled(enabled);
}

bool MonitorManager::isRLEnabled() const {
    return rl_enabled;
}

void MonitorManager::start() {
    if (running) return;

    run_start_time = get_time_seconds();
    run_end_time = run_start_time;
    sum_power = 0.0;
    sum_ipc = 0.0;
    sum_util = 0.0;
    sum_freq = 0.0;
    sample_count = 0;
    action_changes = 0;

    if (fifo_fd < 0) {
        // Open with O_RDWR so it doesn't block waiting for a Python reader,
        // and O_NONBLOCK to prevent writes from blocking if the buffer fills
        fifo_fd = open("config/monitor_pipe", O_RDWR | O_NONBLOCK);
        if (fifo_fd < 0) {
            perror("[monitor] FIFO open failed");
            return;
        }
    }

    // Open CSV file for raw per-sample logging
    if (!csv_path.empty()) {
        std::string csv_dir = csv_path.substr(0, csv_path.find_last_of('/'));
        if (!csv_dir.empty()) {
            std::string mkdir_cmd = "mkdir -p " + csv_dir;
            system(mkdir_cmd.c_str());
        }
        csv_fp = fopen(csv_path.c_str(), "w");
        if (csv_fp) {
            fprintf(csv_fp, "timestamp,util,ipc,iowait,mpki,freq,power,action,source,rl_enabled\n");
            fflush(csv_fp);
            csv_header_written = true;
        }
    }

    first_sample   = true;
    prev_energy_uj = -1;
    memset(&prev_total, 0, sizeof(prev_total));
    prev_core.assign(num_cpus, CoreStat{});
    prev_ctxt = 0;

    initRAPL();

    if (!openPerfCounters())
        printf("[monitor] perf counters unavailable — IPC will be 0\n");

    running       = true;
    reader_thread = std::thread(&MonitorManager::readerLoop, this);
    printf("[monitor] Started\n");
}

void MonitorManager::stop() {
    if (!running) return;
    running = false;
    if (reader_thread.joinable()) reader_thread.join();
    closePerfCounters();

    // Close CSV file if open
    if (csv_fp) {
        fclose(csv_fp);
        csv_fp = nullptr;
    }

    if (!summary_path.empty() && sample_count > 0) {
        std::string summary_dir;
        size_t pos = summary_path.find_last_of('/');
        if (pos != std::string::npos) summary_dir = summary_path.substr(0, pos);
        if (!summary_dir.empty()) {
            std::string mkdir_cmd = "mkdir -p " + summary_dir;
            system(mkdir_cmd.c_str());
        }
        FILE *f = fopen(summary_path.c_str(), "w");
        if (f) {
            double duration = run_end_time - run_start_time;
            double avg_power = sample_count ? sum_power / sample_count : 0.0;
            double avg_ipc = sample_count ? sum_ipc / sample_count : 0.0;
            double avg_util = sample_count ? sum_util / sample_count : 0.0;
            double avg_freq = sample_count ? sum_freq / sample_count : 0.0;
            double actions_per_min = duration > 0 ? (double)action_changes / (duration / 60.0) : 0.0;
            fprintf(f, "run_name,mode,duration_s,avg_power_w,avg_ipc,avg_util,avg_freq_mhz,actions_per_minute\n");
            fprintf(f, "%s,%s,%.1f,%.3f,%.3f,%.3f,%.1f,%.2f\n",
                    rl_enabled ? "rl" : "baseline",
                    rl_enabled ? "RL" : "BASELINE",
                    duration,
                    avg_power,
                    avg_ipc,
                    avg_util,
                    avg_freq,
                    actions_per_min);
            fclose(f);
        }
    }

    printf("[monitor] Stopped\n");
}