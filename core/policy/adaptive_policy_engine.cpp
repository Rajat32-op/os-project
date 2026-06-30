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

    // 1) Swappiness
    if (!writeToFile("/proc/sys/vm/swappiness", std::to_string(action.swappiness))) {
        std::cout << "[AdaptivePolicy] Warning: failed to write swappiness\n";
    }

    // Discover number of CPUs
    int num_cpus = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus < 1) num_cpus = 1;

    // 2) Apply governor per-CPU if specified
    if (!action.governor.empty()) {
        for (int cpu = 0; cpu < num_cpus; ++cpu) {
            std::string gov_path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_governor";
            if (!writeToFile(gov_path, action.governor)) {
                // CPU may be offline or path missing — skip silently but log debug
                // std::cout << "[AdaptivePolicy] Warning: failed to write governor for cpu" << cpu << "\n";
            }
        }
    }

    // 3) Apply frequency cap (scaling_max_freq) by percentage of cpuinfo_max_freq
    if (action.freq_cap_pct > 0 && action.freq_cap_pct <= 100) {
        for (int cpu = 0; cpu < num_cpus; ++cpu) {
            std::string info_path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/cpuinfo_max_freq";
            std::ifstream inf(info_path);
            if (!inf.is_open()) continue;
            long max_khz = 0;
            inf >> max_khz;
            inf.close();
            if (max_khz <= 0) continue;
            long cap_khz = (max_khz * action.freq_cap_pct) / 100;
            std::string cap_path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_max_freq";
            if (!writeToFile(cap_path, std::to_string(cap_khz))) {
                // scaling_max_freq may not be available or writable; skip
            }
        }
    }

    // 4) Try to apply IO scheduler (best-effort) for common device (sda)
    if (!action.io_sched.empty()) {
        if (!writeToFile("/sys/block/sda/queue/scheduler", action.io_sched)) {
            // try generic fallback - don't treat as fatal
        }
    }

    // 5) Migration hint / reserved P-cores
    if (action.migrate_to_e) {
        std::cout << "[AdaptivePolicy] Action: migrating background tasks to E-cores\n";
    }
    if (action.reserved_p_cores > 0) {
        std::cout << "[AdaptivePolicy] Note: reserved_p_cores=" << action.reserved_p_cores << " (no-op)\n";
    }
}

