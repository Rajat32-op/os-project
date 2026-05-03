#include "adaptive_policy_engine.h"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

AdaptivePolicyEngine::AdaptivePolicyEngine() : ema_util(0), ema_ipc(1.0), ema_io(0), ema_disk_io(0), ema_mem(0), cooldown_timer(0), state_fifo_fd(-1), action_fifo_fd(-1), last_reward(0), rl_enabled(false) {
    current_action = {"DO_NOTHING",100, "powersave", 0, false, 60, "mq-deadline", "BASELINE"};
    initFIFOs();
}

AdaptivePolicyEngine::~AdaptivePolicyEngine() {
    if (state_fifo_fd >= 0) close(state_fifo_fd);
    if (action_fifo_fd >= 0) close(action_fifo_fd);
}

void AdaptivePolicyEngine::initFIFOs() {
    // Note: The FIFOs are created by the makefile
    state_fifo_fd = open("/tmp/state_fifo", O_WRONLY | O_NONBLOCK);
    action_fifo_fd = open("/tmp/action_fifo", O_RDONLY | O_NONBLOCK);
}

void AdaptivePolicyEngine::setRLEnabled(bool enabled) {
    rl_enabled = enabled;
}

void AdaptivePolicyEngine::sendStateToRL(const SystemState& state, double reward) {
    if (!rl_enabled) return;
    if (state_fifo_fd < 0) {
        state_fifo_fd = open("/tmp/state_fifo", O_WRONLY | O_NONBLOCK);
        if (state_fifo_fd < 0) return;
    }
    
    last_reward = reward;
    
    std::string json = "{";
    json += "\"util\":" + std::to_string(state.util) + ",";
    json += "\"ipc\":" + std::to_string(state.ipc) + ",";
    json += "\"iowait\":" + std::to_string(state.iowait) + ",";
    json += "\"disk_io\":" + std::to_string(state.disk_io) + ",";
    json += "\"mpki\":" + std::to_string(state.mem_bw) + ",";
    json += "\"mhz\":" + std::to_string(state.mhz) + ",";
    json += "\"temp\":" + std::to_string(state.temp) + ",";
    json += "\"power\":" + std::to_string(state.power) + ",";
    json += "\"ctxsw\":" + std::to_string(state.ctxsw) + ",";
    json += "\"reward\":" + std::to_string(last_reward) + "}\n";
    
    write(state_fifo_fd, json.c_str(), json.size());
}

bool AdaptivePolicyEngine::readActionFromRL(SysAction& out_action) {
    if (action_fifo_fd < 0) {
        action_fifo_fd = open("/tmp/action_fifo", O_RDONLY | O_NONBLOCK);
        if (action_fifo_fd < 0) return false;
    }
    
    char buffer[256];
    int n = read(action_fifo_fd, buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        std::string s(buffer);
        // Simple parsing since we don't have a JSON library easily accessible
        // Expected format: {"action": "PERF_100", "confidence": 0.8}
        // Let's decode few explicit actions based on string match.
        SysAction temp = current_action;
        temp.source = "RL";
        
        bool found_action = false;
        if (s.find("PERF_100") != std::string::npos) {
            temp.name = "PERF_100"; temp.freq_cap_pct = 100; temp.governor = "performance"; temp.reserved_p_cores = 4; temp.migrate_to_e = false;
            found_action = true;
        } else if (s.find("POWERSAVE_50") != std::string::npos) {
            temp.name="POWERSAVE_50"; temp.freq_cap_pct = 50; temp.governor = "powersave"; temp.reserved_p_cores = 0; temp.migrate_to_e = true;
            found_action = true;
        } else if (s.find("SWAP_80") != std::string::npos) {
            temp.name="SWAP_80"; temp.swappiness = 80; temp.freq_cap_pct = 80;
            found_action = true;
        } else if (s.find("IO_BFQ") != std::string::npos) {
            temp.name="IO_BFQ"; temp.io_sched = "bfq"; temp.governor = "powersave"; temp.migrate_to_e = true;
            found_action = true;
        } else if (s.find("DO_NOTHING") != std::string::npos) {
            temp.name="DO_NOTHING";
            found_action = true; // Make no changes
        }
        
        if (found_action) {
            out_action = temp;
            return true;
        }
    }
    return false;
}

bool AdaptivePolicyEngine::isActionSafe(const SysAction& action, double util) {
    // Example safety guard: don't allow deep powersave if utilization is very high
    if (util > 80.0 && action.freq_cap_pct < 60) {
        std::cout << "[AdaptivePolicy] Guard triggered: Rejected RL deep powersave under heavy load.\n";
        return false;
    }
    return true;
}

void AdaptivePolicyEngine::updateEMA(double util, double ipc, double iowait, double disk_io, double mem_bw) {
    const double alpha = 0.3;
    ema_util = alpha * util + (1.0 - alpha) * ema_util;
    ema_ipc = alpha * ipc + (1.0 - alpha) * ema_ipc;
    ema_io = alpha * iowait + (1.0 - alpha) * ema_io;
    ema_disk_io = alpha * disk_io + (1.0 - alpha) * ema_disk_io;
    ema_mem = alpha * mem_bw + (1.0 - alpha) * ema_mem;
}

SystemState AdaptivePolicyEngine::processState(double util, double ipc, double iowait, double disk_io, double mem_bw, double mhz, double temp, double power, double ctxsw) {
    if (mem_bw > 100.0) mem_bw = 100.0;
    
    updateEMA(util, ipc, iowait, disk_io, mem_bw);
    
    SystemState s;
    s.util = ema_util;
    s.ipc = ema_ipc;
    s.iowait = ema_io;
    s.disk_io = ema_disk_io;
    s.mem_bw = ema_mem;
    s.mhz = mhz;
    s.temp = temp;
    s.power = power;
    s.ctxsw = ctxsw;
    
    return s;
}

SysAction AdaptivePolicyEngine::decidePolicy(const SystemState& state) {
    if (cooldown_timer > 0) {
        cooldown_timer--;
        return current_action;
    }
    
    SysAction next_action = current_action;
    bool changed = false;

    // 1. Try to read from RL when enabled
    SysAction rl_action;
    if (rl_enabled && readActionFromRL(rl_action) && isActionSafe(rl_action, state.util)) {
        next_action = rl_action;
        changed = true;
    } else {
        // Simple safety fallback
        next_action.source = "BASELINE";
        if (state.temp > 85.0 || state.util < 10.0) {
             next_action.freq_cap_pct = 50;
             next_action.governor = "powersave";
             next_action.reserved_p_cores = 0;
             next_action.migrate_to_e = true;
             changed = true;
        } else if (state.util > 80.0) {
             next_action.freq_cap_pct = 100;
             next_action.governor = "performance";
             next_action.reserved_p_cores = 4;
             next_action.migrate_to_e = false;
             changed = true;
        }
    }
    
    if (changed) {
        cooldown_timer = 2; // Reduced cooldown loop
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

