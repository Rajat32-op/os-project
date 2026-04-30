#pragma once
#include <map>
#include <string>
#include <vector>

// Workload distribution
struct WorkloadDict {
    double compute;
    double memory;
    double io;
    double idle;
};

// System action state
struct SysAction {
    int freq_cap_pct;
    std::string governor;
    int reserved_p_cores; // how many P cores to use
    bool migrate_to_e;
    int swappiness;
    std::string io_sched;
};

class AdaptivePolicyEngine {
private:
    double ema_util;
    double ema_io;
    double ema_mem;
    
    SysAction current_action;
    int cooldown_timer;
    
    // Smooth input metrics to EMA
    void updateEMA(double util, double iowait, double mem_bw);
    
public:
    AdaptivePolicyEngine();
    
    // 1. Classify workload mix -> Distribution
    WorkloadDict classifyWorkload(double util, double iowait, double mem_bw);
    
    // 2. Decide Policy -> Feedback Action
    SysAction decidePolicy(const WorkloadDict& wl, double current_freq);
    
    // 3. Apply Action (simulated atomic apply log)
    void applyAction(const SysAction& action);
    
    // 4. Compute reward
    double computeReward(double latency, double power_proxy);
};
