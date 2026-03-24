#include "calibrator.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <vector>
#include <string>
#include <ctime>
#include <numeric>
#include <algorithm>

// ── perf_event_open wrapper ────────────────────────────────────────────────
static int perf_event_open(struct perf_event_attr *attr, pid_t pid,
                            int cpu, int group_fd, unsigned long flags) {
    return (int)syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

// ── /proc/stat snapshot ───────────────────────────────────────────────────
struct StatSnap {
    long user, nice, system, idle, iowait, irq, softirq, steal;
    long ctxt;
};

static bool readStat(StatSnap &s) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return false;
    char line[512];
    memset(&s, 0, sizeof(s));
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu ", 4) == 0)
            sscanf(line, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",
                   &s.user, &s.nice, &s.system, &s.idle,
                   &s.iowait, &s.irq, &s.softirq, &s.steal);
        else if (strncmp(line, "ctxt", 4) == 0)
            sscanf(line, "ctxt %ld", &s.ctxt);
    }
    fclose(f);
    return true;
}

// ── IPC via perf ──────────────────────────────────────────────────────────
struct PerfPair { int fd_c, fd_i; };

static std::vector<PerfPair> openPerf(int ncpu) {
    std::vector<PerfPair> fds(ncpu, {-1,-1});
    for (int cpu = 0; cpu < ncpu; ++cpu) {
        struct perf_event_attr pe{};
        pe.size = sizeof(pe); pe.type = PERF_TYPE_HARDWARE; pe.exclude_hv = 1;
        pe.config = PERF_COUNT_HW_CPU_CYCLES;
        fds[cpu].fd_c = perf_event_open(&pe, -1, cpu, -1, 0);
        pe.config = PERF_COUNT_HW_INSTRUCTIONS;
        fds[cpu].fd_i = perf_event_open(&pe, -1, cpu, -1, 0);
    }
    return fds;
}

static void closePerf(std::vector<PerfPair> &fds) {
    for (auto &p : fds) {
        if (p.fd_c >= 0) close(p.fd_c);
        if (p.fd_i >= 0) close(p.fd_i);
    }
}

static double readIPC(std::vector<PerfPair> &fds) {
    long long tc = 0, ti = 0;
    for (auto &p : fds) {
        long long c = 0, i = 0;
        if (p.fd_c >= 0) read(p.fd_c, &c, sizeof(c));
        if (p.fd_i >= 0) read(p.fd_i, &i, sizeof(i));
        tc += c; ti += i;
        if (p.fd_c >= 0) ioctl(p.fd_c, PERF_EVENT_IOC_RESET, 0);
        if (p.fd_i >= 0) ioctl(p.fd_i, PERF_EVENT_IOC_RESET, 0);
    }
    return (tc > 0) ? (double)ti / tc : 1.0;
}

// ── runAndSave ────────────────────────────────────────────────────────────
bool Calibrator::runAndSave() {
    printf("\n");
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   Adaptive CPU Manager — Baseline Calibration ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║  Keep the system IDLE for %2d seconds...      ║\n", SAMPLE_S);
    printf("╚══════════════════════════════════════════════╝\n\n");

    int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;

    auto perf_fds = openPerf(ncpu);

    StatSnap prev{};
    readStat(prev);
    sleep(1);  // discard first delta

    std::vector<double> utils, ctxsws, ipcs;

    for (int s = 0; s < SAMPLE_S; ++s) {
        sleep(1);
        StatSnap cur{};
        if (!readStat(cur)) continue;

        long total =
            (cur.user-prev.user)+(cur.nice-prev.nice)+(cur.system-prev.system)+
            (cur.idle-prev.idle)+(cur.iowait-prev.iowait)+
            (cur.irq-prev.irq)+(cur.softirq-prev.softirq)+(cur.steal-prev.steal);
        long idle  = cur.idle   - prev.idle;
        long iow   = cur.iowait - prev.iowait;

        double util  = (total > 0) ? 100.0*(total-idle-iow)/total : 0.0;
        double ctxsw = (double)(cur.ctxt - prev.ctxt);
        double ipc   = readIPC(perf_fds);

        utils.push_back(util);
        ctxsws.push_back(ctxsw);
        ipcs.push_back(ipc);

        printf("  [%2d/%d] Util: %5.1f%%  CtxSw: %7.0f/s  IPC: %.3f\n",
               s+1, SAMPLE_S, util, ctxsw, ipc);

        prev = cur;
    }

    closePerf(perf_fds);

    if (utils.empty()) {
        printf("[calibrate] No samples collected — aborting\n");
        return false;
    }

    // Use median to resist outliers
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };

    double base_util  = median(utils);
    double base_ctxsw = median(ctxsws);
    double base_ipc   = median(ipcs);

    // Clamp to sensible minimums so thresholds stay meaningful
    if (base_util  < 1.0)    base_util  = 1.0;
    if (base_ctxsw < 500.0)  base_ctxsw = 500.0;
    if (base_ipc   < 0.1)    base_ipc   = 0.1;

    printf("\n  Baseline — Util: %.1f%%  CtxSw: %.0f/s  IPC: %.3f\n",
           base_util, base_ctxsw, base_ipc);

    // Get current timestamp
    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    // Write JSON manually — no external lib needed
    FILE *f = fopen(Calibrator::CALIB_PATH, "w");
    if (!f) {
        perror("[calibrate] Cannot write calibration.json");
        return false;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"baseline_util\":  %.4f,\n", base_util);
    fprintf(f, "  \"baseline_ctxsw\": %.4f,\n", base_ctxsw);
    fprintf(f, "  \"baseline_ipc\":   %.4f,\n", base_ipc);
    fprintf(f, "  \"calibrated_at\": \"%s\",\n", ts);
    fprintf(f, "  \"num_cpus\": %d\n", ncpu);
    fprintf(f, "}\n");
    fclose(f);

    printf("  Saved → %s\n\n", Calibrator::CALIB_PATH);
    return true;
}

// ── load ─────────────────────────────────────────────────────────────────
bool Calibrator::load(CalibrationData &out) {
    FILE *f = fopen(Calibrator::CALIB_PATH, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        double v = 0;
        if (sscanf(line, " \"baseline_util\": %lf",  &v) == 1) out.baseline_util  = v;
        if (sscanf(line, " \"baseline_ctxsw\": %lf", &v) == 1) out.baseline_ctxsw = v;
        if (sscanf(line, " \"baseline_ipc\": %lf",   &v) == 1) out.baseline_ipc   = v;
    }
    fclose(f);

    printf("[calibrate] Loaded — Util:%.1f%%  CtxSw:%.0f/s  IPC:%.3f\n",
           out.baseline_util, out.baseline_ctxsw, out.baseline_ipc);
    return true;
}