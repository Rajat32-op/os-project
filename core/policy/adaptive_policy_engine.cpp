#include "adaptive_policy_engine.h"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <string>

AdaptivePolicyEngine::AdaptivePolicyEngine() : ema_util(0), ema_io(0), ema_mem(0), cooldown_timer(0) {
    current_action = {100, "powersave", 0, false, 60, "mq-deadline"};
}

void AdaptivePolicyEngine::updateEMA(double util, double iowait, double mem_bw) {
    const double alpha = 0.3;
    ema_util = alpha * util + (1.0 - alpha) * ema_util;
    ema_io = alpha * iowait + (1.0 - alpha) * ema_io;
    ema_mem = alpha * mem_bw + (1.0 - alpha) * ema_mem;
}

WorkloadDict AdaptivePolicyEngine::classifyWorkload(double util, double iowait, double mem_bw) {
    updateEMA(util, iowait, mem_bw);
    
    WorkloadDict wl = {0,0,0,0};
    
    // Simplistic heuristic to build the distribution
    // util and iowait are percentages (0-100).
    // ema_mem (cache misses in '000s) can be arbitrarily large. We normalize it to a 0-100 scale.
    double mem_intensity = std::min(100.0, ema_mem / 50.0); // Scale factor: e.g., 5000k misses = 100%
    
    // Calculate how much of the CPU is actually doing nothing.
    double idle_weight = std::max(0.0, 100.0 - (ema_util + ema_io));
    
    double total = ema_util + ema_io + mem_intensity + idle_weight;
    if (total <= 0.0) {
        wl.compute = 0;
        wl.io = 0;
        wl.memory = 0;
        wl.idle = 1.0;
    } else {
        wl.compute = ema_util / total;
        wl.io = ema_io / total;
        wl.memory = mem_intensity / total;
        wl.idle = idle_weight / total;
    }
    
    return wl;
}

SysAction AdaptivePolicyEngine::decidePolicy(const WorkloadDict& wl, double current_freq) {
    if (cooldown_timer > 0) {
        cooldown_timer--;
        return current_action;
    }
    
    SysAction next_action = current_action;
    bool changed = false;

    // Rule baseline
    if (wl.idle > 0.5 || (ema_util < 5.0 && ema_io < 5.0 && std::min(100.0, ema_mem / 50.0) < 5.0)) {
        next_action.freq_cap_pct = 50;
        next_action.governor = "powersave";
        next_action.reserved_p_cores = 0;
        next_action.migrate_to_e = true;
        changed = true;
    } else if (wl.compute > 0.6) {
        next_action.freq_cap_pct = 100;
        next_action.governor = "performance";
        next_action.reserved_p_cores = 4;
        next_action.migrate_to_e = false;
        changed = true;
    } else if (wl.io > 0.4) {
        next_action.governor = "powersave";
        next_action.io_sched = "bfq";
        next_action.migrate_to_e = true;
        changed = true;
    } else if (wl.memory > 0.4) {
        next_action.swappiness = 80;
        next_action.freq_cap_pct = 80;
        changed = true;
    }
    
    if (changed) {
        cooldown_timer = 5; // 5 cycles (~1s - 2.5s)
        current_action = next_action;
    }
    
    return current_action;
}

bool writeToFile(const std::string& path, const std::string& val) {
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << val;
    return true;
}

void AdaptivePolicyEngine::applyAction(const SysAction& action) {
    std::cout << "[AdaptivePolicy] Applied action: FreqCap " << action.freq_cap_pct << "%"
              << " Gov " << action.governor 
              << " P-Cores " << action.reserved_p_cores
              << " Swap " << action.swappiness << std::endl;
              
    // Write swappiness
    writeToFile("/proc/sys/vm/swappiness", std::to_string(action.swappiness));
    
    // In a real multi-core environment with heterogenous architecture:
    // P-Core / E-Core migration typically involves cpusets or sched_setaffinity.
    // Log the cpuset instruction here instead of modifying global tree root to avoid disrupting user system.
    if (action.migrate_to_e) {
        std::cout << "[AdaptivePolicy] Action: migrating background tasks to E-cores" << std::endl;
    }
    
    // CPU Governor (Handled already partially by governor class, but this sets explicitly)
    // we would loop through /sys/devices/system/cpu/cpuX/cpufreq/scaling_governor
}

double AdaptivePolicyEngine::computeReward(double latency, double power_proxy) {
    // Reward objective: - (latency + lambda * power_proxy)
    double lambda = 0.5;
    return -(latency + lambda * power_proxy);
}
