#pragma once
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <iostream>
#include "../classifier/classifier.h"
#include "../policy/governor.h"

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

    // Classifier + pin state
    Classifier  classifier;
    Governor    governor;
    CpuMode     last_committed;    // track changes to avoid redundant apply()
    std::mutex  mode_mutex;
    bool        is_pinned;
    CpuMode     pinned_mode;

    void   readerLoop();
    bool   readProcStat(double &util, double &iowait, double &ctxsw,
                        std::vector<double> &core_utils);
    bool   readFreqs(double &avg_mhz, std::vector<double> &core_mhz);
    double readTemperature();
    double readIPC();
    double readRAPL();
    bool   openPerfCounters();
    void   closePerfCounters();
    void   initRAPL();

public:
    MonitorManager();
    ~MonitorManager();
    void start();
    void stop();
    void setPinnedMode(CpuMode m);
    void setAuto();
};