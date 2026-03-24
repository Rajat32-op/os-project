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

// ── Helpers ───────────────────────────────────────────────────────────────
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
      last_committed(CpuMode::IDLE),
      is_pinned(false), pinned_mode(CpuMode::IDLE) {
    num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus < 1) num_cpus = 1;
    memset(&prev_total, 0, sizeof(prev_total));
    prev_ctxt = 0;
    prev_core.resize(num_cpus);
    for (auto &c : prev_core) memset(&c, 0, sizeof(c));

    // Load calibration → derive thresholds
    classifier.loadCalibration(Calibrator::CALIB_PATH);

    // Init governor → discover available governors
    governor.init();
}

MonitorManager::~MonitorManager() {
    stop();
    if (fifo_fd >= 0) { close(fifo_fd); fifo_fd = -1; }
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
    perf_fds.resize(num_cpus, {-1, -1});
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
        perf_fds[cpu] = {fd_c, fd_i};
    }
    printf("[perf] Opened counters for %d CPUs\n", num_cpus);
    return true;
}

void MonitorManager::closePerfCounters() {
    for (auto &pf : perf_fds) {
        if (pf.fd_cycles >= 0) { close(pf.fd_cycles); pf.fd_cycles = -1; }
        if (pf.fd_instrs >= 0) { close(pf.fd_instrs); pf.fd_instrs = -1; }
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
    double watts = -1.0;
    if (prev_energy_uj >= 0) {
        long long delta = energy_uj - prev_energy_uj;
        if (delta < 0) delta = 0;
        watts = delta / 1e6;
    }
    prev_energy_uj = energy_uj;
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

// ── readIPC ───────────────────────────────────────────────────────────────
double MonitorManager::readIPC() {
    long long tc = 0, ti = 0;
    for (auto &pf : perf_fds) {
        if (pf.fd_cycles < 0 || pf.fd_instrs < 0) continue;
        long long c = 0, i = 0;
        if (read(pf.fd_cycles, &c, sizeof(c)) == sizeof(c)) tc += c;
        if (read(pf.fd_instrs, &i, sizeof(i)) == sizeof(i)) ti += i;
        ioctl(pf.fd_cycles, PERF_EVENT_IOC_RESET, 0);
        ioctl(pf.fd_instrs, PERF_EVENT_IOC_RESET, 0);
    }
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

        double temp  = readTemperature();
        double ipc   = readIPC();
        double power = readRAPL();

        if (util < 0)   util   = 0;
        if (util > 100) util   = 100;
        if (iowait < 0) iowait = 0;

        // ── Mode decision ─────────────────────────────────────────────────
        CpuMode mode;
        bool    is_auto;
        {
            std::lock_guard<std::mutex> lk(mode_mutex);
            if (is_pinned) {
                mode    = pinned_mode;
                is_auto = false;
            } else {
                mode    = classifier.update(util, ipc, ctxsw, iowait);
                is_auto = true;
            }

            // Apply governor only when committed mode actually changes
            if (mode != last_committed) {
                governor.apply(mode);
                last_committed = mode;
            }
        }

        std::string mode_str = modeToString(mode);
        int streak     = classifier.getStreak();
        int confidence = classifier.CONFIDENCE_N;
        std::string gov = governor.current();

        printf("[%s%s] Util:%.1f%% MHz:%.0f IPC:%.3f "
               "IOWait:%.1f%% CtxSw:%.0f/s Temp:%.1f°C Power:%.1fW"
               " Gov:%s | candidate:%s streak:%d/%d\n",
               mode_str.c_str(), is_auto ? "" : "(PINNED)",
               util, avg_mhz, ipc, iowait, ctxsw, temp, power,
               gov.c_str(),
               modeToString(classifier.getCandidate()).c_str(),
               streak, confidence);

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
        json += "\"auto\":"    + std::string(is_auto ? "true" : "false") + ",";
        json += "\"streak\":"  + std::to_string(streak) + ",";
        json += "\"gov\":\""   + gov + "\",";

        json += "\"core_util\":[";
        for (int i = 0; i < (int)core_utils.size(); ++i) {
            if (i) json += ",";
            json += dtos(core_utils[i], 1);
        }
        json += "],";

        json += "\"core_mhz\":[";
        for (int i = 0; i < (int)core_mhz.size(); ++i) {
            if (i) json += ",";
            json += dtos(core_mhz[i], 0);
        }
        json += "]}\n";

        if (fifo_fd >= 0) {
            ssize_t n = write(fifo_fd, json.c_str(), json.size());
            if (n < 0) {
                printf("[monitor] FIFO write failed (errno=%d)\n", errno);
                running = false;
                break;
            }
        }

        sleep(1);
    }
}

// ── start / stop ──────────────────────────────────────────────────────────
void MonitorManager::start() {
    if (running) return;

    if (fifo_fd < 0) {
        fifo_fd = open("config/monitor_pipe", O_WRONLY);
        if (fifo_fd < 0) {
            perror("[monitor] FIFO open failed");
            return;
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
    printf("[monitor] Stopped\n");
}