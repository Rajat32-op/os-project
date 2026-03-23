#pragma once
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <iostream>

struct PerfCounters {
    int fd_cycles;
    int fd_instrs;
};

class MonitorManager {
private:
    std::atomic<bool> running;
    std::thread       reader_thread;
    int               fifo_fd;

    std::vector<PerfCounters> perf_fds;
    int num_cpus;

    // Per-CPU /proc/stat snapshot for deltas
    struct CoreStat {
        long user, nice, system, idle, iowait, irq, softirq, steal;
    };
    std::vector<CoreStat> prev_core;
    CoreStat              prev_total;
    long                  prev_ctxt;
    bool                  first_sample;

    // RAPL
    std::string rapl_path;
    long long   prev_energy_uj;
    bool        rapl_available;

    void   readerLoop();

    // Readers
    bool   readProcStat(double &util_pct, double &iowait_pct, double &ctxsw_rate,
                        std::vector<double> &core_utils);
    bool   readFreqs(double &avg_mhz, std::vector<double> &core_mhz);
    double readTemperature();
    double readIPC();
    double readRAPL();          // returns watts, -1 if unavailable

    bool openPerfCounters();
    void closePerfCounters();
    void initRAPL();

public:
    MonitorManager();
    ~MonitorManager();
    void start();
    void stop();
};